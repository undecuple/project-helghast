/* Patches 1052 and 1109 host test: the lock-line coherency registry.
 *
 *   tools/spu/test_spu_event_race.sh   (also builds this one; SPU_TREE=DIR for another patched tree)
 *
 * 1052: before it, spu_coh_reserve stopped at the first free slot, so a context registered BEHIND a hole (left by an
 * unregister) was added a second time, and spu_coh_unregister removed only the first copy: the second stayed as a
 * dangling pointer once a stack-local job context returned (a crash in spu_coh_notify_write).
 * 1109: the registry was 8 slots and the 9th holder was dropped (it never got its lock-line events; 135 of 555 old
 * Proton logs printed the warning, newest 2026-10-10). Now chunks are appended: more than eight holders all get the
 * kill, holes are reused before a new chunk, and threads registering at once while a notifier walks lose no one and
 * duplicate no one. Exit 0 = pass. */
#include "spu_coherency.c"
#include <pthread.h>
#include <stdio.h>

static uint8_t g_guest_mem[1u << 16];
uint8_t* vm_base = g_guest_mem;
static unsigned long g_wakes;
void spu_ch_wake(spu_context* ctx)
{
    (void)ctx;
    __atomic_fetch_add(&g_wakes, 1, __ATOMIC_RELAXED);
}

static int count(const spu_context* c)
{
    int n = 0;
    SPU_COH_FOR_EACH(ch, i) n += ch->slot[i] == c;
    return n;
}

static int fail;
#define CHECK(c, ...) do { if (!(c)) { fail = 1; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void hold(spu_context* c, uint32_t line)
{
    spu_coh_reserve(c, line);
    c->resv_ea = line;
    c->resv_valid = 1;
    c->event_status = 0;
}

enum { N = 100, THREADS = 8, PER = 64, LINE = 0x2000 };
static spu_context g_many[N];
static spu_context g_thr[THREADS][PER];
static volatile int g_stop;

static void* registrar(void* arg)
{
    spu_context* mine = g_thr[(intptr_t)arg];
    for (int i = 0; i < PER; i++) spu_coh_reserve(&mine[i], 0x4000);
    return NULL;
}
static void* walker(void* arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) spu_coh_notify_write(0x8000);   /* nobody holds it */
    return NULL;
}

int main(void)
{
    static spu_context a, b;
    spu_coh_reserve(&a, 0x1000);
    spu_coh_reserve(&b, 0x1080); /* a in slot 0, b in slot 1 */
    spu_coh_unregister(&a);      /* hole at slot 0 */
    spu_coh_reserve(&b, 0x1100); /* b reserves again */
    CHECK(count(&b) == 1, "b registered %d time(s) after a re-reserve behind a hole (want 1)", count(&b));
    spu_coh_unregister(&b);
    CHECK(count(&b) == 0, "b registered %d time(s) after unregister (want 0)", count(&b));

    /* More than eight holders, more than one chunk. */
    for (int i = 0; i < N; i++) hold(&g_many[i], LINE);
    unsigned cap = spu_coh_capacity();
    CHECK(cap >= N, "capacity %u < %d holders", cap, N);
    g_wakes = 0;
    spu_coh_notify_write(LINE);
    int killed = 0;
    for (int i = 0; i < N; i++) killed += g_many[i].resv_valid == 0 && (g_many[i].event_status & SPU_EVENT_LR);
    CHECK(killed == N && g_wakes == N, "%d holders: %d killed, %lu wakes (want all)", N, killed, g_wakes);
    printf("%d holders, capacity %u: %d killed, %lu wakes\n", N, cap, killed, g_wakes);

    /* except-self skips only self, also past the first chunk. */
    for (int i = 0; i < N; i++) hold(&g_many[i], LINE);
    spu_coh_notify_write_except(LINE, &g_many[N - 1]);
    killed = 0;
    for (int i = 0; i < N; i++) killed += g_many[i].resv_valid == 0;
    CHECK(killed == N - 1 && g_many[N - 1].resv_valid == 1, "except-self: %d killed (want %d)", killed, N - 1);

    /* Holes are reused: unregister every other one, register as many new ones, capacity stays. */
    for (int i = 0; i < N; i += 2) spu_coh_unregister(&g_many[i]);
    static spu_context fresh[N / 2];
    for (int i = 0; i < N / 2; i++) spu_coh_reserve(&fresh[i], LINE);
    CHECK(spu_coh_capacity() == cap, "capacity grew %u -> %u with %d holes free", cap, spu_coh_capacity(), N / 2);
    int dup = 0;
    for (int i = 0; i < N / 2; i++) dup += count(&fresh[i]) != 1;
    for (int i = 1; i < N; i += 2) dup += count(&g_many[i]) != 1;
    for (int i = 0; i < N; i += 2) dup += count(&g_many[i]) != 0;
    CHECK(dup == 0, "%d contexts registered a wrong number of times after hole reuse", dup);
    for (int i = 0; i < N; i++) spu_coh_unregister(&g_many[i]);
    for (int i = 0; i < N / 2; i++) spu_coh_unregister(&fresh[i]);

    /* Concurrent registration (each thread its own contexts, as GETLLAR does) while a notifier walks. */
    pthread_t w, t[THREADS];
    pthread_create(&w, NULL, walker, NULL);
    for (intptr_t k = 0; k < THREADS; k++) pthread_create(&t[k], NULL, registrar, (void*)k);
    for (int k = 0; k < THREADS; k++) pthread_join(t[k], NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    pthread_join(w, NULL);
    int bad = 0, total = 0;
    for (int k = 0; k < THREADS; k++)
        for (int i = 0; i < PER; i++) bad += count(&g_thr[k][i]) != 1;
    SPU_COH_FOR_EACH(ch, i) total += ch->slot[i] != NULL;
    CHECK(bad == 0 && total == THREADS * PER, "concurrent: %d contexts not exactly once, %d registered (want %d)",
          bad, total, THREADS * PER);
    printf("concurrent: %d threads x %d contexts, %d registered, capacity %u\n", THREADS, PER, total,
           spu_coh_capacity());
    for (int k = 0; k < THREADS; k++)
        for (int i = 0; i < PER; i++) spu_coh_unregister(&g_thr[k][i]);
    total = 0;
    SPU_COH_FOR_EACH(ch, i) total += ch->slot[i] != NULL;
    CHECK(total == 0, "%d left after unregistering everyone", total);

    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
