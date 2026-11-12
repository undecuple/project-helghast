/* Actual submission hook plus an independent last-reader oracle. No GPU/game data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_ring_marks.h"

typedef uint64_t u64;
enum { LD_RM_SRV, LD_RM_SMP, LD_RM_PSCB, LD_RM_N };
static struct {
    unsigned fr_cur;
    u64 srv_ring_used, smp_ring_used, ps_cb_used;
} g;
static rsx_ring_marks marks[2][LD_RM_N];
static unsigned long long g_ld_rm_bad_n;
static int enabled = 1;
static int ld_ring_fence_wait_on(void)
{
    return enabled;
}
static rsx_ring_marks* ld_rm(int r)
{
    return &marks[g.fr_cur][r];
}

/* The Python driver inserts the shipped ld_rm_note_submit here. */
#include "submission.h"

enum { CAP = 37 };
static struct {
    u64 used, reader[CAP];
    unsigned char pending[CAP];
} model[2][LD_RM_N];
static u64 fence, completed;
static u64 slot_submit[2];
static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                       \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void reset(void)
{
    memset(&g, 0, sizeof g);
    memset(marks, 0, sizeof marks);
    memset(model, 0, sizeof model);
    fence = completed = 0;
    memset(slot_submit, 0, sizeof slot_submit);
    g_ld_rm_bad_n = 0;
    enabled = 1;
}

static void submit(void)
{
    g.srv_ring_used = model[g.fr_cur][LD_RM_SRV].used;
    g.smp_ring_used = model[g.fr_cur][LD_RM_SMP].used;
    g.ps_cb_used = model[g.fr_cur][LD_RM_PSCB].used;
    ld_rm_note_submit(++fence);
    slot_submit[g.fr_cur] = fence;
    for (int r = 0; r < LD_RM_N; r++)
        for (int i = 0; i < CAP; i++) {
            if (model[g.fr_cur][r].pending[i]) model[g.fr_cur][r].reader[i] = fence;
            model[g.fr_cur][r].pending[i] = 0;
        }
}

static void wrap(int r)
{
    submit();
    rsx_rm_wrap(ld_rm(r));
    model[g.fr_cur][r].used = 0;
}

static void allocate(int r, unsigned n)
{
    u64 start = model[g.fr_cur][r].used, end = start + n, need = 0;
    CHECK(end <= CAP);
    if (end > CAP) return;
    for (u64 i = start; i < end; i++)
        if (model[g.fr_cur][r].reader[i] > need) need = model[g.fr_cur][r].reader[i];
    u64 got = rsx_rm_acquire(ld_rm(r), end);
    if (need > completed) CHECK(got >= need);
    CHECK(got <= fence);
    if (got > completed) completed = got; /* a successful queue wait completes earlier fences too */
    for (u64 i = start; i < end; i++) model[g.fr_cur][r].reader[i] = 0;
    model[g.fr_cur][r].used = end;
}

static void read_slot(int r, unsigned slot)
{
    CHECK(slot < model[g.fr_cur][r].used);
    model[g.fr_cur][r].pending[slot] = 1;
}

static void hand(void)
{
    /* Cache hits with unchanged cursors, including a later-bound pixel constant. */
    for (int r = 0; r < LD_RM_N; r++) {
        reset();
        allocate(r, 1);
        if (r != LD_RM_PSCB) read_slot(r, 0);
        submit();
        read_slot(r, 0);
        submit();
        rsx_rm_wrap(ld_rm(r));
        CHECK(rsx_rm_acquire(ld_rm(r), 1) == fence);
        printf("checked unchanged cursor / later reader ring=%d\n", r);

        reset();
        allocate(r, 1);
        read_slot(r, 0);
        submit();
        allocate(r, 1);
        read_slot(r, 1);
        read_slot(r, 0);
        submit();
        rsx_rm_wrap(ld_rm(r));
        CHECK(rsx_rm_acquire(ld_rm(r), 1) == fence);
    }

    /* A short new generation must not erase the untouched old tail. */
    reset();
    allocate(0, CAP);
    read_slot(0, CAP - 1);
    wrap(0);
    allocate(0, 1);
    read_slot(0, 0);
    wrap(0);
    CHECK(marks[0][0].old_n == 2);
    CHECK(marks[0][0].old[1].start == 1 && marks[0][0].old[1].fence == 1);
    allocate(0, CAP);

    /* Slots stay independent; recycling is permitted only after their queued work completes. */
    reset();
    allocate(0, 1);
    read_slot(0, 0);
    submit();
    g.fr_cur = 1;
    allocate(0, 2);
    read_slot(0, 1);
    submit();
    CHECK(marks[0][0].last_fence == 1 && marks[1][0].last_fence == 2);
    completed = slot_submit[0];
    for (int r = 0; r < LD_RM_N; r++) rsx_rm_clear(&marks[0][r]);
    CHECK(marks[1][0].cur_end == 2);

    reset();
    enabled = 0;
    model[0][0].used = 3;
    submit();
    CHECK(marks[0][0].cur_n == 0);
    enabled = 1;
    reset();
    submit();
    for (int r = 0; r < LD_RM_N; r++) CHECK(marks[0][r].cur_n == 0);

    /* Retain the underlying conservative recovery for invalid submit ordering. */
    rsx_ring_marks m;
    rsx_rm_clear(&m);
    CHECK(rsx_rm_submit_live(&m, 20, 7) == 1);
    CHECK(rsx_rm_submit_live(&m, 10, 6) == -1);
    CHECK(m.cur_n == 1 && m.cur_end == 20 && m.cur[0].fence == 7);
    CHECK(rsx_rm_submit_live(&m, 20, 7) == -1);
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_submit_live(&m, 0, 8) == 0);
    CHECK(m.cur_n == 0 && rsx_rm_acquire(&m, 20) == 7);
}

static unsigned random_state;
static unsigned next(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void randomized(unsigned seed)
{
    reset();
    random_state = seed;
    for (unsigned step = 0; step < 20000; step++) {
        int r = (int)(next() % LD_RM_N);
        unsigned action = next() % 9;
        if (action < 3) {
            unsigned n = 1 + next() % 5;
            if (model[g.fr_cur][r].used + n > CAP) wrap(r);
            allocate(r, n);
            if (next() & 1) read_slot(r, (unsigned)model[g.fr_cur][r].used - 1);
        } else if (action < 6 && model[g.fr_cur][r].used) {
            read_slot(r, next() % (unsigned)model[g.fr_cur][r].used);
        } else if (action == 6) {
            submit();
        } else if (action == 7) {
            wrap(r);
        } else {
            submit();
            g.fr_cur ^= 1u;
            /* Model the existing frame-slot fence wait before recycling. */
            for (int ring = 0; ring < LD_RM_N; ring++) {
                if (slot_submit[g.fr_cur] > completed) completed = slot_submit[g.fr_cur];
                rsx_rm_clear(ld_rm(ring));
                memset(&model[g.fr_cur][ring], 0, sizeof model[0][0]);
            }
        }
        if (failures) {
            fprintf(stderr, "seed=%u step=%u\n", seed, step);
            return;
        }
    }
    CHECK(g_ld_rm_bad_n == 0);
}

int main(void)
{
    hand();
    for (unsigned seed = 1; seed <= 32 && !failures; seed++) randomized(seed);
    printf("[ring_lifetime] %s: actual-reader oracle, 32 seeds x 20000 steps\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
