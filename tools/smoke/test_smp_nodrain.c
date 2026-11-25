/* Host test for KZ3 patch 2213 (LD_SMP_RING_NODRAIN): the per-table last-use fences of rsx_smp_use.h against an actual-reader oracle.
 *
 * Model: a walker records draws (1 time unit each), every K draws the open list is submitted (fence f, GPU completes it
 * `gpu_cost` units after the previous one), a draw binds the sampler table of its key (hit = reuse of a table of this
 * generation, else take the next table; a full ring wraps). Four wrap policies run on the same draw stream:
 *   A  the old drain: submit, wait for the newest fence, restart at 0.
 *   B  patch 2213: submit, restart at 0, every take waits only for rsx_smpu_need(table).
 *   C  broken: wrap like B, never wait (the oracle must flag it).
 *   D  patch 2153/2155's marks (rsx_ring_marks.h, rsx_rm_submit_live): wrap like B, take waits for rsx_rm_peek.
 * The oracle knows the true reader of every table independently of the policy (it records each bind and each submit): a take must
 * never overwrite a table whose last reader is unsubmitted or not completed at the walker's clock.
 * Asserts: A, B, D never violate; C does; B waits strictly less than A and than D (D keeps the whole prefix live: its first take
 * waits for the wrap submit's fence, which is the drain again); every flip a wrap without a take for the draws in between. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_smp_use.h"
#include "rsx_ring_marks.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RING 128u
#define MAXF 20000u
enum { POL_A, POL_B, POL_C, POL_D };
typedef struct {
    int pol;
    double now;                 /* walker clock */
    double gpu_done[MAXF];      /* completion time per fence */
    uint64_t nfence;            /* last fence */
    double gpu_last;            /* gpu completion time of the newest fence */
    double waited;              /* total walker wait */
    uint64_t waits, wraps, viol, takes, hits;
    uint32_t used;              /* ring cursor */
    uint32_t key_at[RING];      /* key stored in each table of this ring */
    /* the oracle */
    uint64_t real_fence[RING]; uint8_t pending[RING];
    rsx_smp_use u; rsx_ring_marks rm;
    uint32_t open_draws;
} sim;

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void submit(sim* s, double gpu_cost)
{
    if (!s->open_draws) return;
    const uint64_t f = ++s->nfence;
    const double start = s->gpu_last > s->now ? s->gpu_last : s->now;
    s->gpu_done[f] = start + gpu_cost; s->gpu_last = s->gpu_done[f];
    rsx_smpu_submit(&s->u, f);
    rsx_rm_submit_live(&s->rm, s->used, f);
    for (uint32_t t = 0; t < RING; t++) if (s->pending[t]) { s->pending[t] = 0; s->real_fence[t] = f; }
    s->open_draws = 0;
}
static void wait_fence(sim* s, uint64_t f)
{
    if (!f) return;
    if (s->gpu_done[f] > s->now) { s->waited += s->gpu_done[f] - s->now; s->now = s->gpu_done[f]; s->waits++; }
}
static void bind(sim* s, uint32_t t) { rsx_smpu_bind(&s->u, 0, t); s->pending[t] = 1; }

/* one draw with key `key` */
static void draw(sim* s, uint32_t key, uint32_t K, double gpu_cost)
{
    s->now += 1.0;
    uint32_t hit = RING;
    for (uint32_t i = 0; i < s->used; i++) if (s->key_at[i] == key) { hit = i; break; }
    if (hit != RING) { s->hits++; bind(s, hit); }
    else {
        if (s->used >= RING) {   /* wrap */
            s->wraps++;
            submit(s, gpu_cost);
            if (s->pol == POL_A) { wait_fence(s, s->nfence); rsx_smpu_init(&s->u); rsx_rm_clear(&s->rm); memset(s->real_fence, 0, sizeof s->real_fence); }
            if (s->pol == POL_D) rsx_rm_wrap(&s->rm);
            s->used = 0;
        }
        const uint32_t t = s->used;
        if (s->pol == POL_B) wait_fence(s, rsx_smpu_need(&s->u, 0, t));
        if (s->pol == POL_D) { const uint64_t f = rsx_rm_peek(&s->rm, (uint64_t)t + 1u); wait_fence(s, f); rsx_rm_acquire(&s->rm, (uint64_t)t + 1u); }
        /* oracle: the table's last reader must be submitted and complete */
        s->takes++;
        if (s->pending[t] || (s->real_fence[t] && s->gpu_done[s->real_fence[t]] > s->now)) s->viol++;
        s->used++; s->key_at[t] = key;
        bind(s, t);
    }
    if (++s->open_draws >= K) submit(s, gpu_cost);
}

static void run(sim* s, int pol, uint32_t draws, uint32_t universe, uint32_t K, double gpu_per_draw)
{
    memset(s, 0, sizeof *s); s->pol = pol; rsx_smpu_init(&s->u); rsx_rm_clear(&s->rm);
    rng_state = 777;
    for (uint32_t i = 0; i < draws; i++) {
        /* 60 % of the draws repeat one of the last 24 keys (state locality), else a new random key of the universe */
        uint32_t key = (rnd() % 10 < 6) ? (uint32_t)(i / 3 + rnd() % 24) % universe : rnd() % universe;
        draw(s, key, K, gpu_per_draw * K);
    }
    submit(s, gpu_per_draw * K);
}

static void test_policies(void)
{
    static sim A, B, C, D;
    run(&A, POL_A, 6000, 900, 12, 1.4);
    run(&B, POL_B, 6000, 900, 12, 1.4);
    run(&C, POL_C, 6000, 900, 12, 1.4);
    run(&D, POL_D, 6000, 900, 12, 1.4);
    printf("[test_smp_nodrain] wraps A=%llu B=%llu; walker wait A=%.0f B=%.0f D=%.0f (C=%.0f), waits A=%llu B=%llu D=%llu, violations A=%llu B=%llu C=%llu D=%llu\n",
           (unsigned long long)A.wraps, (unsigned long long)B.wraps, A.waited, B.waited, D.waited, C.waited,
           (unsigned long long)A.waits, (unsigned long long)B.waits, (unsigned long long)D.waits,
           (unsigned long long)A.viol, (unsigned long long)B.viol, (unsigned long long)C.viol, (unsigned long long)D.viol);
    CHECK(A.wraps > 20 && B.wraps > 20);                  /* the stream really wraps */
    CHECK(A.viol == 0 && B.viol == 0 && D.viol == 0);     /* the three real policies never overwrite a live table */
    CHECK(C.viol > 0);                                    /* the broken policy is caught by the oracle */
    CHECK(B.waited < A.waited * 0.9);                     /* the point: less walker wait than the drain */
    CHECK(B.waited < D.waited * 0.9);                     /* and than the 2155 live-prefix marks (the drain again) */
    CHECK(B.u.unsub_seen == 0);                           /* every wrap submitted first: no table without a fence */
    /* GPU faster than the walker: nothing to wait for in B */
    run(&B, POL_B, 6000, 900, 12, 0.2); CHECK(B.waited == 0 && B.viol == 0);
    /* slow GPU, tiny chunks and big chunks stay safe */
    for (uint32_t K = 1; K <= 64; K *= 4) { run(&B, POL_B, 3000, 400, K, 2.5); CHECK(B.viol == 0); }
}
/* the bookkeeping itself: UNSUB until submit, hit re-stamps, a table in the open list has no fence to wait for (counted) */
static void test_unit(void)
{
    rsx_smp_use u; rsx_smpu_init(&u);
    CHECK(rsx_smpu_need(&u, 0, 5) == 0 && u.unsub_seen == 0);
    rsx_smpu_bind(&u, 0, 5); rsx_smpu_bind(&u, 1, 7);
    CHECK(rsx_smpu_unsub_count(&u, 0, 128) == 1 && rsx_smpu_unsub_count(&u, 1, 128) == 1 && rsx_smpu_unsub_count(&u, 0, 5) == 0);
    rsx_smpu_submit(&u, 41);
    CHECK(rsx_smpu_need(&u, 0, 5) == 41 && rsx_smpu_need(&u, 1, 7) == 41 && rsx_smpu_need(&u, 0, 7) == 0);
    rsx_smpu_bind(&u, 0, 5); rsx_smpu_submit(&u, 52);                 /* a hit in a later list moves the fence forward */
    CHECK(rsx_smpu_need(&u, 0, 5) == 52 && rsx_smpu_need(&u, 1, 7) == 41);
    rsx_smpu_bind(&u, 0, 9);
    CHECK(rsx_smpu_need(&u, 0, 9) == 0 && u.unsub_seen == 1);          /* still in the open list */
    CHECK(rsx_smpu_need(&u, 0, 4000) == 0);
}
int main(void)
{
    test_unit(); test_policies();
    printf("[test_smp_nodrain] %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
