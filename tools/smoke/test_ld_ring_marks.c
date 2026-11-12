/* Host test for KZ3 patch 2153 (LD_RING_FENCE_WAIT): the (range, fence) mark arithmetic of rsx_ring_marks.h.
 * Hand cases (empty ring, wrap point, marks spanning the wrap, out-of-order fences, cursor going back, overflow merge)
 * plus a randomized ring against a per-slot reference: acquire(end) must never return a fence older than the newest one
 * that read a slot below `end` (a shorter wait would let the CPU overwrite memory the GPU still reads) and never
 * one newer than the last submit. No game data, no GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_ring_marks.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void hand(void)
{
    rsx_ring_marks m; rsx_rm_clear(&m);
    /* empty ring: nothing to wait for, wrap of an empty ring stays empty, no mark without bytes */
    CHECK(rsx_rm_acquire(&m, 1000) == 0);
    CHECK(rsx_rm_submit(&m, 0, 1) == 0);
    rsx_rm_wrap(&m);
    CHECK(m.old_n == 0 && rsx_rm_acquire(&m, 1000) == 0);

    /* three submits, wrap, the cursor re-enters range by range */
    rsx_rm_clear(&m);
    CHECK(rsx_rm_submit(&m, 100, 10) == 1);
    CHECK(rsx_rm_submit(&m, 250, 20) == 1);
    CHECK(rsx_rm_submit(&m, 400, 30) == 1);
    CHECK(rsx_rm_submit(&m, 400, 31) == 0);   /* a submit that wrote nothing adds no mark */
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 50) == 10);      /* [0,50) was read by fence 10 only */
    CHECK(rsx_rm_acquire(&m, 100) == 10);     /* rest of the first mark */
    CHECK(rsx_rm_acquire(&m, 100) == 0);      /* already retired */
    CHECK(rsx_rm_acquire(&m, 101) == 20);     /* crosses into the second mark */
    CHECK(rsx_rm_acquire(&m, 300) == 30);     /* spans the second (rest) and the third mark: newest wins */
    CHECK(rsx_rm_acquire(&m, 400) == 30);
    CHECK(rsx_rm_acquire(&m, 4096) == 0 && m.old_n == 0);

    /* a range that starts below `end` is waited for even when `end` only touches its first slot */
    rsx_rm_clear(&m);
    rsx_rm_submit(&m, 64, 5); rsx_rm_submit(&m, 128, 6);
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 65) == 6);

    /* second wrap before the cursor consumed the first generation: the old tail keeps its (older) fence */
    rsx_rm_clear(&m);
    rsx_rm_submit(&m, 100, 1); rsx_rm_submit(&m, 300, 2);
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 50) == 1);       /* cursor now at 50 */
    CHECK(rsx_rm_submit(&m, 50, 3) == 1);     /* gen 2 wrote [0,50) under fence 3 */
    rsx_rm_wrap(&m);                          /* old list: [0,50) f3, [50,100) f1, [100,300) f2 */
    CHECK(rsx_rm_acquire(&m, 10) == 3);
    CHECK(rsx_rm_acquire(&m, 60) == 3);       /* rest of [0,50) f3 and [50,60) f1 */
    CHECK(rsx_rm_acquire(&m, 300) == 2);

    /* fence values out of order are rejected and widen the marks (never shorten a wait) */
    rsx_rm_clear(&m);
    CHECK(rsx_rm_submit(&m, 100, 7) == 1);
    CHECK(rsx_rm_submit(&m, 200, 7) == -1);   /* not increasing */
    CHECK(rsx_rm_submit(&m, 300, 6) == -1);   /* going back */
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 1) == 7);        /* the whole range up to 300 waits for the newest fence */
    CHECK(rsx_rm_acquire(&m, 300) == 7);

    /* the cursor moving back without wrap() is rejected the same way */
    rsx_rm_clear(&m);
    rsx_rm_submit(&m, 500, 1);
    CHECK(rsx_rm_submit(&m, 100, 2) == -1);
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 500) == 2 && m.old_n == 0);

    /* overflow: more marks than the list holds are merged (union, newest fence), not dropped */
    rsx_rm_clear(&m);
    for (uint64_t i = 1; i <= 3u * RSX_RM_MAX; i++) rsx_rm_submit(&m, i * 16u, i);
    CHECK(m.cur_n <= RSX_RM_MAX);
    rsx_rm_wrap(&m);
    CHECK(rsx_rm_acquire(&m, 1) >= 1);
    CHECK(rsx_rm_acquire(&m, 3u * RSX_RM_MAX * 16u) == 3u * RSX_RM_MAX);
}

static void randomized(unsigned seed)
{
    enum { RING = 1000 };
    static uint64_t slot_fence[RING];   /* newest fence that read each slot; 0 = free */
    srand(seed); memset(slot_fence, 0, sizeof slot_fence);
    rsx_ring_marks m; rsx_rm_clear(&m);
    uint64_t fence = 0, cursor = 0, submitted_end = 0;
    for (int step = 0; step < 20000; step++) {
        const uint32_t len = 1u + (uint32_t)(rand() % 40);
        if (cursor + len > RING) {   /* full: submit the open list, wrap, restart at 0 */
            fence++; for (uint64_t u = submitted_end; u < cursor; u++) slot_fence[u] = fence;
            CHECK(rsx_rm_submit(&m, cursor, fence) >= 0);
            submitted_end = 0; rsx_rm_wrap(&m); cursor = 0;
        }
        const uint64_t end = cursor + len;
        uint64_t need = 0;
        for (uint64_t u = cursor; u < end; u++) if (slot_fence[u] > need) need = slot_fence[u];
        const uint64_t got = rsx_rm_acquire(&m, end);
        if (got < need) { printf("FAIL seed %u step %d: waits for %llu, slots [%llu,%llu) were read by %llu\n", seed, step,
                                 (unsigned long long)got, (unsigned long long)cursor, (unsigned long long)end,
                                 (unsigned long long)need); fails++; return; }
        if (got > fence) { printf("FAIL seed %u: waits for a fence %llu that was never submitted (last %llu)\n", seed,
                                  (unsigned long long)got, (unsigned long long)fence); fails++; return; }
        for (uint64_t u = cursor; u < end; u++) slot_fence[u] = 0;   /* overwritten: free until the next submit */
        cursor = end;
        if (rand() % 7 == 0) {   /* a chunk submit */
            fence++; for (uint64_t u = submitted_end; u < cursor; u++) slot_fence[u] = fence;
            CHECK(rsx_rm_submit(&m, cursor, fence) >= 0);
            submitted_end = cursor;
        }
    }
}

int main(void)
{
    hand();
    for (unsigned s = 1; s <= 20; s++) randomized(s);
    printf("[test_ld_ring_marks] %s\n", fails ? "failures" : "all cases ok");
    return fails ? 1 : 0;
}
