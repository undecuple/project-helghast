/* Host test for KZ3 patch 2191: libs/kzr/core/sync (seq, tokens, kzr_seq_publish, ordered publisher) and the
 * kzr_platform events. Unity build: the library sources are #included so the test hooks below apply everywhere.
 * Properties (each has a negative control: the library is rebuilt with -DKZR_SYNC_MUTANT_x and the property must FAIL;
 * tools/smoke/test_pub.sh drives them):
 *   order       in-order retire under random gate completion, gates satisfied and worker data visible at retire
 *   idle_reads  reads_done treats an idle worker as infinity               (mutant PINNED)
 *   hol         head-of-line blocking: nothing retires past an unmet gate  (mutant ORDER)
 *   full_log    a full log stalls the producer, drops nothing             (mutant DROP)
 *   lost_wake   a wake between prepare and wait is not lost (deterministic), multi-waiter stress (mutant NOEPOCH)
 *   dekker      waiter flag vs publish: never (waiter missed the state AND signaller saw no waiter) (mutant NOFENCE)
 *   sfence      every publish runs the fence before its store; payload visible after acquire (mutant PUBLISH; TSan)
 * No game data, no GPU. No timeouts: a hang is a bug, except the single bounded observation in lost_wake that exists
 * only to let the NOEPOCH negative control terminate.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* sfence / store trace (test hooks, defined before the library headers). The macro also issues the release fence the
 * default uses off x86, so the hook records "a fence ran in this thread before this store". */
#include <stdatomic.h>
static _Thread_local int tl_fenced;
static atomic_ulong g_unfenced, g_publishes;
#define KZR_SFENCE() do { tl_fenced = 1; atomic_thread_fence(memory_order_release); } while (0)
#define KZR_PUB_STORE_HOOK(t) do { (void)(t); atomic_fetch_add(&g_publishes, 1); \
        if (!tl_fenced) { atomic_fetch_add(&g_unfenced, 1); } \
        tl_fenced = 0; } while (0)

#include "kzr/kzr_platform.h"
#include "core/sync/kzr_sync.h"
#include "platform/common/kzr_event.c"
#ifdef KZR_WIN32
#include "platform/win32/kzr_platform_win32.c"
#else
#include "platform/posix/kzr_platform_posix.c"
#endif
#include "core/sync/kzr_sync.c"

static int g_fail;
#define CHECK(prop, cond, ...) do { if (!(cond)) { g_fail++; printf("[test_pub] FAIL %s: ", prop); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void pass(const char* prop, const char* extra) { printf("[test_pub] done %s %s\n", prop, extra); }

static uint64_t rng_next(uint64_t* s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
static void spin_delay(uint64_t* r)
{
    uint32_t k = (uint32_t)(rng_next(r) % 400);
    if ((rng_next(r) % 100) == 0) { for (int i = 0; i < 40; i++) kzr_yield(); return; }   /* a slow completion: head-of-line */
    for (volatile uint32_t i = 0; i < k; i++) { }
}

/* ============================ order: random gate completion ============================ */
#define ST_N 30000u
#define ST_W 3u
#define QN (1u << 15)
typedef struct { uint64_t q[QN]; kzr_au64 qt, qh_unused; } wq;
static struct {
    kzr_sync s; kzr_pub p;
    wq w[ST_W];
    uint8_t is_op[ST_N * 2], opdone[ST_N * 2];
    uint8_t gpu_slot[ST_N * 2];
    kzr_au64 issued; kzr_au32 stop;
    uint64_t last_retired, ck, nret, nreads, ngpu, nboth, nnone, bad_order, bad_gate, bad_data;
} S;

static void st_hook(void* ctx, const kzr_rec* r)
{
    (void)ctx;
    if (r->seq <= S.last_retired) S.bad_order++;
    S.last_retired = r->seq;
    if (!kzr_sync_gate_ok(&S.s, r->gate, r->seq)) S.bad_gate++;
    unsigned k = KZR_GATE_KIND(r->gate);
    if (k & KZR_GATE_READS) {           /* every op <= seq finished and its data is visible (TSan checks the edge) */
        for (uint64_t i = S.ck + 1; i <= r->seq; i++) if (S.is_op[i] && !S.opdone[i]) S.bad_data++;
        if (r->seq > S.ck) S.ck = r->seq;
    }
    if ((k & KZR_GATE_GPU) && !S.gpu_slot[KZR_GATE_V(r->gate)]) S.bad_data++;
    S.nret++;
}
typedef struct { unsigned w; uint64_t rng; } wctx;
static void worker_main(void* a)
{
    wctx* c = (wctx*)a;
    wq* q = &S.w[c->w];
    uint64_t qh = 0;
    for (;;) {
        if (qh < kzr_ld_acq(&q->qt)) {
            uint64_t seq = q->q[qh++ % QN];
            spin_delay(&c->rng);
            S.opdone[seq] = 1;                         /* the "worker output", published by done */
            kzr_sync_done(&S.s, c->w, seq);
        } else if (atomic_load_explicit(&S.stop, memory_order_acquire)) {
            if (qh >= kzr_ld_acq(&q->qt)) break;
        } else kzr_yield();
    }
}
static void gpu_main(void* a)
{
    uint64_t r = *(uint64_t*)a, cur = 0;
    for (;;) {
        uint64_t target = kzr_ld_acq(&S.issued);
        if (cur < target) {
            spin_delay(&r);
            uint64_t nx = cur + 1 + rng_next(&r) % 3; if (nx > target) nx = target;
            for (uint64_t v = cur + 1; v <= nx; v++) S.gpu_slot[v] = 1;
            cur = nx;
            kzr_sync_resolve_publish(&S.s, cur);
        } else if (atomic_load_explicit(&S.stop, memory_order_acquire) && cur >= kzr_ld_acq(&S.issued)) break;
        else kzr_yield();
    }
}
static void test_order(void)
{
    memset(&S, 0, sizeof S);
    kzr_sync_init(&S.s, ST_W);
    if (kzr_pub_init(&S.p, &S.s, 64, st_hook, NULL)) { CHECK("order", 0, "init"); return; }
    kzr_thread tw[ST_W], tg; wctx wc[ST_W]; uint64_t grng = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < ST_W; i++) { wc[i].w = i; wc[i].rng = 0x1234567ull * (i + 3); kzr_thread_start(&tw[i], worker_main, &wc[i]); }
    kzr_thread_start(&tg, gpu_main, &grng);
    kzr_seq sq; kzr_seq_init(&sq);
    uint64_t rng = 0xdeadbeefcafef00dull, vcount = 0;
    for (unsigned step = 0; step < ST_N; step++) {
        uint64_t seq = kzr_seq_alloc(&sq), r = rng_next(&rng) % 10;
        if (r < 5) {
            unsigned pick = (unsigned)(rng_next(&rng) % 17), w = pick < 8 ? 0 : pick < 16 ? 1 : 2;   /* worker 2 mostly idle */
            S.is_op[seq] = 1;
            kzr_sync_push(&S.s, w, seq);
            wq* q = &S.w[w]; uint64_t t = kzr_ld_rlx(&q->qt);
            q->q[t % QN] = seq; kzr_st_rel(&q->qt, t + 1);
        } else {
            kzr_rec rec; memset(&rec, 0, sizeof rec); rec.seq = seq;
            unsigned k = (unsigned)(rng_next(&rng) % 4); uint64_t v = 0;
            if (k & KZR_GATE_GPU) { v = ++vcount; kzr_st_rel(&S.issued, vcount); }
            rec.gate = KZR_GATE(k, v);
            if (k == KZR_GATE_READS) S.nreads++; else if (k == KZR_GATE_GPU) S.ngpu++; else if (k == KZR_GATE_BOTH) S.nboth++; else S.nnone++;
            kzr_pub_push(&S.p, &rec);
        }
        kzr_sync_prep_publish(&S.s, seq);
        if ((rng_next(&rng) & 3) == 0) kzr_pub_retire_ready(&S.p);
    }
    kzr_pub_drain(&S.p);
    atomic_store_explicit(&S.stop, 1, memory_order_release);
    kzr_event_wake_all(&S.s.ev);
    for (unsigned i = 0; i < ST_W; i++) kzr_thread_join(&tw[i]);
    kzr_thread_join(&tg);
    uint64_t recs = S.nreads + S.ngpu + S.nboth + S.nnone;
    CHECK("order", S.bad_order == 0, "%llu out-of-order retires", (unsigned long long)S.bad_order);
    CHECK("order", S.bad_gate == 0, "%llu retires with an unmet gate", (unsigned long long)S.bad_gate);
    CHECK("order", S.bad_data == 0, "%llu retires before worker/gpu data", (unsigned long long)S.bad_data);
    CHECK("order", S.nret == recs, "retired %llu of %llu", (unsigned long long)S.nret, (unsigned long long)recs);
    CHECK("order", kzr_token_load(&S.s.retire) == S.last_retired, "retire token");
    char b[160]; snprintf(b, sizeof b, "(%llu records: reads %llu gpu %llu both %llu none %llu; hol_blocked %llu waits %llu)",
        (unsigned long long)recs, (unsigned long long)S.nreads, (unsigned long long)S.ngpu, (unsigned long long)S.nboth,
        (unsigned long long)S.nnone, (unsigned long long)S.p.st.hol_blocked, (unsigned long long)S.p.st.stall_waits);
    pass("order", b);
    kzr_pub_destroy(&S.p); kzr_sync_destroy(&S.s);
}

/* ============================ idle_reads ============================ */
static void test_idle(void)
{
    kzr_sync s; kzr_sync_init(&s, 3);
    kzr_sync_prep_publish(&s, 10);
    CHECK("idle_reads", kzr_sync_reads_done(&s, 10), "all idle must be done");
    kzr_sync_push(&s, 0, 7);                       /* w0 busy: pushed 7, done 0 */
    CHECK("idle_reads", !kzr_sync_reads_done(&s, 7), "busy worker must hold seq 7");
    kzr_sync_done(&s, 0, 4);
    CHECK("idle_reads", kzr_sync_reads_done(&s, 3), "w0 done 4 >= 3, w1 w2 idle");
    CHECK("idle_reads", !kzr_sync_reads_done(&s, 5), "w0 done 4 < 5 and not idle");
    kzr_sync_done(&s, 0, 7);                       /* w0 idle again (done == pushed), w1 and w2 never used */
    CHECK("idle_reads", kzr_sync_reads_done(&s, 7), "w1/w2 never used must not pin seq 7");
    kzr_sync_prep_publish(&s, 9);
    CHECK("idle_reads", kzr_sync_reads_done(&s, 9), "idle w0 (done 7 == pushed 7) is infinity for seq 9");
    kzr_sync_prep_publish(&s, 10);
    CHECK("idle_reads", !kzr_sync_reads_done(&s, 11), "prep 10 < 11");
    pass("idle_reads", "");
    kzr_sync_destroy(&s);
}

/* ============================ hol + full_log helpers ============================ */
static uint64_t g_seen[1024]; static unsigned g_nseen;
static void rec_hook(void* ctx, const kzr_rec* r) { (void)ctx; if (g_nseen < 1024) g_seen[g_nseen++] = r->seq; }

static void test_hol(void)
{
    kzr_sync s; kzr_sync_init(&s, 0); kzr_pub p; kzr_pub_init(&p, &s, 16, rec_hook, NULL);
    g_nseen = 0;
    kzr_rec r; memset(&r, 0, sizeof r);
    r.seq = 1; r.gate = KZR_GATE(KZR_GATE_GPU, 1); kzr_pub_push(&p, &r);
    r.seq = 2; r.gate = KZR_GATE(KZR_GATE_NONE, 0); kzr_pub_push(&p, &r);
    r.seq = 3; r.gate = KZR_GATE(KZR_GATE_NONE, 0); kzr_pub_push(&p, &r);
    kzr_sync_prep_publish(&s, 3);
    unsigned n = kzr_pub_retire_ready(&p);
    CHECK("hol", n == 0, "retired %u records past an unmet head gate", n);
    CHECK("hol", g_nseen == 0, "hook saw %u records", g_nseen);
    CHECK("hol", kzr_token_load(&s.retire) == 0, "retire token moved to %llu", (unsigned long long)kzr_token_load(&s.retire));
    CHECK("hol", p.st.hol_blocked >= 1, "hol_blocked not counted");
    kzr_sync_resolve_publish(&s, 1);
    n = kzr_pub_retire_ready(&p);
    CHECK("hol", n == 3 && g_nseen == 3 && g_seen[0] == 1 && g_seen[1] == 2 && g_seen[2] == 3, "after the gate: n=%u order %llu %llu %llu",
          n, (unsigned long long)g_seen[0], (unsigned long long)g_seen[1], (unsigned long long)g_seen[2]);
    CHECK("hol", kzr_token_load(&s.retire) == 3, "retire token");
    pass("hol", "");
    kzr_pub_destroy(&p); kzr_sync_destroy(&s);
}

typedef struct { kzr_sync* s; uint64_t n, rng; } gctx;
static void gate_feeder(void* a)
{
    gctx* g = (gctx*)a;
    for (uint64_t v = 1; v <= g->n; v++) { spin_delay(&g->rng); kzr_sync_resolve_publish(g->s, v); }
}
static void test_full(void)
{
    enum { N = 500, CAP = 8 };
    kzr_sync s; kzr_sync_init(&s, 0); kzr_pub p; kzr_pub_init(&p, &s, CAP, rec_hook, NULL);
    g_nseen = 0;
    gctx g = { &s, N, 0x77 }; kzr_thread t; kzr_thread_start(&t, gate_feeder, &g);
    for (uint64_t i = 1; i <= N; i++) {
        kzr_rec r; memset(&r, 0, sizeof r); r.seq = i; r.gate = KZR_GATE(KZR_GATE_GPU, i);
        kzr_pub_push(&p, &r);
        kzr_sync_prep_publish(&s, i);
        CHECK("full_log", kzr_pub_pending(&p) <= CAP, "log over capacity");
    }
    kzr_pub_drain(&p);
    kzr_thread_join(&t);
    int ordered = 1; for (unsigned i = 0; i < g_nseen && i < 1024; i++) if (g_seen[i] != i + 1) ordered = 0;
    CHECK("full_log", p.st.stalls > 0, "the log never filled: test did not exercise the stall");
    CHECK("full_log", p.st.dropped == 0, "%llu dropped", (unsigned long long)p.st.dropped);
    CHECK("full_log", p.st.retired == N && p.st.pushed == N, "pushed %llu retired %llu of %d", (unsigned long long)p.st.pushed, (unsigned long long)p.st.retired, N);
    CHECK("full_log", ordered, "order lost");
    char b[96]; snprintf(b, sizeof b, "(stalls %llu, waits %llu, 0 dropped)", (unsigned long long)p.st.stalls, (unsigned long long)p.st.stall_waits);
    pass("full_log", b);
    kzr_pub_destroy(&p); kzr_sync_destroy(&s);
}

/* ============================ lost_wake ============================ */
static struct { kzr_event ev; kzr_au32 pred, armed, signalled, returned; uint64_t seen; kzr_waiter w; } LW;
static void lw_waiter(void* a)
{
    (void)a;
    LW.seen = kzr_event_prepare(&LW.ev);
    if (atomic_load(&LW.pred)) { kzr_event_cancel(&LW.ev); atomic_store(&LW.returned, 2); return; }
    atomic_store(&LW.armed, 1);
    while (!atomic_load(&LW.signalled)) kzr_yield();     /* the signal lands between prepare and wait */
    kzr_event_wait(&LW.ev, LW.seen, &LW.w);
    kzr_event_cancel(&LW.ev);
    atomic_store(&LW.returned, 1);
}
/* multi-waiter ping-pong through the real kzr_sync wait path */
__attribute__((unused)) static struct { kzr_sync s; kzr_au64 stopv; } PP;
typedef struct { kzr_waiter w; uint64_t got; } ppc;
__attribute__((unused)) static void pp_consumer(void* a)
{
    ppc* c = (ppc*)a;
    for (uint64_t v = 1; v <= 50000; v++) { kzr_sync_wait_gate(&PP.s, &c->w, KZR_GATE(KZR_GATE_GPU, v), 0); c->got = v; }
}
static void test_lost_wake(void)
{
    memset(&LW, 0, sizeof LW);
    kzr_event_init(&LW.ev); kzr_waiter_init(&LW.w);
    kzr_thread t; kzr_thread_start(&t, lw_waiter, NULL);
    while (!atomic_load(&LW.armed)) kzr_yield();
    atomic_store(&LW.pred, 1);
    kzr_event_signal(&LW.ev);                            /* waiter registered: wake_all with nobody in the list yet */
    atomic_store(&LW.signalled, 1);
#ifdef KZR_SYNC_MUTANT_NOEPOCH
    /* the one bounded observation: the mutant sleeps forever, so look for a while, then free it to let the test end */
    for (long i = 0; i < 3000000 && !atomic_load(&LW.returned); i++) kzr_yield();
    if (!atomic_load(&LW.returned)) { CHECK("lost_wake", 0, "waiter slept through a wake sent between prepare and wait"); kzr_event_wake_all(&LW.ev); }
    kzr_thread_join(&t);
    kzr_waiter_destroy(&LW.w);
    return;                                              /* the stress below would hang under this mutant by design */
#else
    kzr_thread_join(&t);
    CHECK("lost_wake", atomic_load(&LW.returned) == 1, "waiter did not return through the epoch check");
    kzr_waiter_destroy(&LW.w);
#endif
#ifndef KZR_SYNC_MUTANT_NOFENCE
    memset(&PP, 0, sizeof PP); kzr_sync_init(&PP.s, 0);
    ppc c[3]; kzr_thread th[3];
    for (int i = 0; i < 3; i++) { kzr_waiter_init(&c[i].w); c[i].got = 0; kzr_thread_start(&th[i], pp_consumer, &c[i]); }
    for (uint64_t v = 1; v <= 50000; v++) { if ((v & 63) == 0) kzr_yield(); kzr_sync_resolve_publish(&PP.s, v); }
    for (int i = 0; i < 3; i++) { kzr_thread_join(&th[i]); CHECK("lost_wake", c[i].got == 50000, "consumer %d got %llu", i, (unsigned long long)c[i].got); kzr_waiter_destroy(&c[i].w); }
    kzr_sync_destroy(&PP.s);
#endif
    pass("lost_wake", "(deterministic interleaving + 3-consumer stress)");
}

/* ============================ dekker ============================ */
#define DK_N 200000u
static struct { kzr_event ev; kzr_au32 pred[DK_N]; uint8_t r1[DK_N], r2[DK_N]; kzr_au32 b1, b2; } DK;
static void dk_barrier(kzr_au32* b, unsigned n)   /* spin barrier, generation = n */
{
    atomic_fetch_add(b, 1);
    for (unsigned spins = 0; atomic_load(b) < 2 * (n + 1); spins++) if ((spins & 4095) == 4095) kzr_yield();
}
static void dk_waiter(void* a)
{
    (void)a;
    for (unsigned i = 0; i < DK_N; i++) {
        dk_barrier(&DK.b1, i);
        kzr_event_prepare(&DK.ev);                         /* waiters++ (seq_cst) */
        DK.r1[i] = (uint8_t)atomic_load_explicit(&DK.pred[i], memory_order_seq_cst);
        dk_barrier(&DK.b2, i);
        kzr_event_cancel(&DK.ev);
    }
}
static void dk_signaller(void* a)
{
    (void)a;
    for (unsigned i = 0; i < DK_N; i++) {
        dk_barrier(&DK.b1, i);
        atomic_store_explicit(&DK.pred[i], 1, memory_order_release);   /* what kzr_seq_publish + sfence amounts to */
        DK.r2[i] = (uint8_t)kzr_event_needs_wake(&DK.ev);
        dk_barrier(&DK.b2, i);
    }
}
static void test_dekker(void)
{
    memset(&DK, 0, sizeof DK); kzr_event_init(&DK.ev);
    kzr_thread a, b; kzr_thread_start(&a, dk_waiter, NULL); kzr_thread_start(&b, dk_signaller, NULL);
    kzr_thread_join(&a); kzr_thread_join(&b);
    unsigned both_missed = 0; for (unsigned i = 0; i < DK_N; i++) if (!DK.r1[i] && !DK.r2[i]) both_missed++;
    CHECK("dekker", both_missed == 0, "%u of %u rounds: waiter missed the state and the signaller saw no waiter (lost wake)", both_missed, DK_N);
    char b2[64]; snprintf(b2, sizeof b2, "(%u rounds, 0 lost)", DK_N);
    pass("dekker", b2);
}

/* ============================ sfence / publish payload ============================ */
#define PB_N 20000
static struct { kzr_token tok, ack; int pl[16]; } PB;
static void pb_writer(void* a)
{
    (void)a;
    for (int k = 1; k <= PB_N; k++) {
        while (kzr_token_load(&PB.ack) < (uint64_t)(k - 1)) kzr_yield();
        for (int i = 0; i < 16; i++) PB.pl[i] = k;
        kzr_seq_publish(&PB.tok, (uint64_t)k);
    }
}
static void pb_reader(void* a)
{
    (void)a; int bad = 0;
    for (int k = 1; k <= PB_N; k++) {
        while (kzr_token_load(&PB.tok) < (uint64_t)k) kzr_yield();
        for (int i = 0; i < 16; i++) if (PB.pl[i] != k) bad++;
        kzr_seq_publish(&PB.ack, (uint64_t)k);
    }
    *(int*)a = bad;
}
static void test_publish(void)
{
    memset(&PB, 0, sizeof PB);
    int bad = 0; kzr_thread w, r; kzr_thread_start(&r, pb_reader, &bad); kzr_thread_start(&w, pb_writer, NULL);
    kzr_thread_join(&w); kzr_thread_join(&r);
    CHECK("sfence", bad == 0, "%d stale payload reads", bad);
}

/* argv[1] = one property name (the negative controls run only the property their mutant must break: the PINNED mutant
 * makes the full-stress drain hang by design, which is the bug it models). No argument = everything. */
int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : NULL;
#define WANT(p) (!only || !strcmp(only, p))
    if (WANT("idle_reads")) test_idle();
    if (WANT("hol")) test_hol();
    if (WANT("full_log")) test_full();
    if (WANT("order")) test_order();
    if (WANT("lost_wake")) test_lost_wake();
    if (WANT("dekker")) test_dekker();
    if (WANT("sfence")) {
        test_publish();
        unsigned long pubs = atomic_load(&g_publishes), un = atomic_load(&g_unfenced);
        CHECK("sfence", un == 0, "%lu of %lu publishes ran without the fence before the store", un, pubs);
        char b[80]; snprintf(b, sizeof b, "(%lu publishes, all fenced; payload visible)", pubs); pass("sfence", b);
    }
    printf("[test_pub] %s\n", g_fail ? "FAILED" : "ALL OK");
    return g_fail ? 1 : 0;
}
