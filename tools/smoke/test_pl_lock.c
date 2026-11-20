/* Host test for KZ3 patch 2182: rsx_pl_lock.h (per-key lock around pipeline-library Load + create + Store).
 * N threads x M overlapping keys against a fake library that records calls: at most one store per key, no Load
 * concurrent with a Store (or two Stores) of one key, every thread gets a pipeline, the waiters' Loads hit.
 * Negative control: lock switched off -> the checker must flag a double store or an overlap. Build with
 * -fsanitize=thread (the script does); the fake library itself is made lock-free per key (atomics) so TSan only sees
 * the races in the code under test. */
#include "rsx_pl_lock.h"
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>
#define NK 48
#define NT 8
typedef struct {
    unsigned stored;                  /* library has this name */
    int in_load, in_store;            /* active callers on this name */
    unsigned store_calls, create_calls;
    unsigned overlap, double_store, bad_store_during_load;
} fkey;
static fkey g_k[NK];
static fkey* keyslot(uint64_t key) { return &g_k[key % NK]; }
typedef struct { uint64_t key; } fctx;
static void spin(void) { struct timespec t = {0, 20000}; nanosleep(&t, NULL); sched_yield(); }
static int f_load(void* c, void** out)
{
    fkey* k = keyslot(((fctx*)c)->key);
    __atomic_add_fetch(&k->in_load, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&k->in_store, __ATOMIC_SEQ_CST)) __atomic_add_fetch(&k->overlap, 1, __ATOMIC_SEQ_CST);
    spin();
    if (__atomic_load_n(&k->in_store, __ATOMIC_SEQ_CST)) __atomic_add_fetch(&k->overlap, 1, __ATOMIC_SEQ_CST);
    const int hit = __atomic_load_n(&k->stored, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&k->in_load, 1, __ATOMIC_SEQ_CST);
    if (hit) *out = (void*)(uintptr_t)(((fctx*)c)->key | 1);
    return hit;
}
static int f_create(void* c, void** out)
{
    fkey* k = keyslot(((fctx*)c)->key);
    __atomic_add_fetch(&k->create_calls, 1, __ATOMIC_SEQ_CST);
    spin(); spin();
    *out = (void*)(uintptr_t)(((fctx*)c)->key | 1);
    return 1;
}
static int f_store(void* c, void* obj)
{
    (void)obj;
    fkey* k = keyslot(((fctx*)c)->key);
    __atomic_add_fetch(&k->store_calls, 1, __ATOMIC_SEQ_CST);
    if (__atomic_add_fetch(&k->in_store, 1, __ATOMIC_SEQ_CST) > 1) __atomic_add_fetch(&k->overlap, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&k->in_load, __ATOMIC_SEQ_CST)) __atomic_add_fetch(&k->overlap, 1, __ATOMIC_SEQ_CST);
    spin();
    int rejected = 0;
    if (__atomic_exchange_n(&k->stored, 1, __ATOMIC_SEQ_CST)) { __atomic_add_fetch(&k->double_store, 1, __ATOMIC_SEQ_CST); rejected = 1; }
    __atomic_sub_fetch(&k->in_store, 1, __ATOMIC_SEQ_CST);
    return !rejected;
}
static rsx_pl_locks g_L;
static const rsx_pl_ops g_ops = { f_load, f_create, f_store };
static unsigned g_nopipe;
static void* thread_main(void* arg)
{
    unsigned t = (unsigned)(uintptr_t)arg;
    for (unsigned r = 0; r < 3; r++)
        for (unsigned i = 0; i < NK; i++) {
            fctx c; c.key = 0x1234567800000000ull + ((i + t * 5 + r * 11) % NK);   /* every thread visits every key, rotated */
            void* out; int hit;
            if (!rsx_pl_get(&g_L, c.key, &g_ops, &c, &out, &hit) || !out) __atomic_add_fetch(&g_nopipe, 1, __ATOMIC_SEQ_CST);
        }
    return NULL;
}
static int run(int off, unsigned* viol, unsigned* maxstore)
{
    memset(g_k, 0, sizeof g_k); g_nopipe = 0;
    rsx_pl_locks_init(&g_L); g_L.off = off;
    pthread_t th[NT];
    for (unsigned t = 0; t < NT; t++) pthread_create(&th[t], NULL, thread_main, (void*)(uintptr_t)t);
    for (unsigned t = 0; t < NT; t++) pthread_join(th[t], NULL);
    *viol = 0; *maxstore = 0;
    for (unsigned i = 0; i < NK; i++) {
        *viol += g_k[i].overlap + g_k[i].double_store;
        if (g_k[i].store_calls > *maxstore) *maxstore = g_k[i].store_calls;
    }
    return 0;
}
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)
int main(void)
{
    int fails = 0; unsigned viol, maxstore;
    /* striping sanity: 64 stripes all reachable, same key same stripe */
    { unsigned seen[RSX_PL_STRIPES] = {0}, n = 0;
      for (uint64_t k = 1; k < 20000; k++) { unsigned s = rsx_pl_stripe(k * 0x100000001B3ull); if (s >= RSX_PL_STRIPES) fails++; else if (!seen[s]++) n++; }
      CHECK(n == RSX_PL_STRIPES, "stripes reached %u of %u", n, RSX_PL_STRIPES);
      CHECK(rsx_pl_stripe(77) == rsx_pl_stripe(77), "stripe not deterministic"); }
    /* locked: repeat to give the scheduler chances */
    for (int rep = 0; rep < 4; rep++) {
        run(0, &viol, &maxstore);
        const unsigned total = NT * 3 * NK;
        CHECK(viol == 0, "locked run %d: %u violations", rep, viol);
        CHECK(maxstore == 1, "locked run %d: a key was stored %u times", rep, maxstore);
        CHECK(g_nopipe == 0, "locked run %d: %u callers got no pipeline", rep, g_nopipe);
        CHECK(g_L.n_calls == total, "calls %u != %u", g_L.n_calls, total);
        CHECK(g_L.n_creates == NK && g_L.n_stores == NK, "creates %u stores %u (want %u each)", g_L.n_creates, g_L.n_stores, NK);
        CHECK(g_L.n_load_hits == total - NK, "load hits %u want %u", g_L.n_load_hits, total - NK);
        CHECK(g_L.n_store_rejects == 0, "store rejects %u", g_L.n_store_rejects);
        CHECK(g_L.n_second_hits <= g_L.n_load_hits, "second hits > hits");
        CHECK(g_L.n_waits > 0, "no caller ever waited (test did not overlap)");
        printf("locked rep %d: calls %u waits %u second-thread-hits %u creates %u stores %u violations %u\n", rep, g_L.n_calls, g_L.n_waits, g_L.n_second_hits, g_L.n_creates, g_L.n_stores, viol);
    }
    /* negative control: lock off must be caught by the checker (this runs the fake library only; TSan is not used for it) */
    run(1, &viol, &maxstore);
    printf("lock off: violations %u max stores per key %u creates %u\n", viol, maxstore, g_L.n_creates);
    CHECK(viol > 0 && maxstore > 1, "negative control did not flag anything (viol %u maxstore %u)", viol, maxstore);
    if (fails) { printf("test_pl_lock: %d FAIL\n", fails); return 1; }
    printf("test_pl_lock: all checks passed\n");
    return 0;
}
