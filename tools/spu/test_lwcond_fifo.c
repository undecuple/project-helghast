/* Host test for KZ3 patch 1059 (sys_lwcond per-waiter FIFO, runtime/ppu/kz3_lwcond_fifo.h).
 *
 * Reproduces the lost wakeup of the shared counting semaphore that patch 0003 used (kept here as the negative
 * control, "old"): two waiters W1 and W2 are queued, the signaller does signal_all; W1 wakes, consumes its flag,
 * finds nothing else to do and waits again before W2 has run; W1 then takes the count meant for W2 and W2 sleeps
 * on with its flag set. W2's lateness is forced by a hook between "registered + guest mutex dropped" and the
 * blocking wait, the same spot in both algorithms (a thread descheduled there). The FIFO ("new") marks W2's own
 * node at signal time, so W2 returns whenever it runs. Also checks signal (one: the oldest), signal_to (the named
 * thread) and a timeout (node unlinked, no stale wakeup).
 *
 * Build/run: tools/spu/test_lwcond_fifo.sh (Linux, pthreads; the header's Windows half is the same algorithm). */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kz3_lwcond_fifo.h"

/* ---- the model: a guest lwmutex (pthread mutex) + a condition, two waiters with a flag each ---- */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_flag[2];
static volatile int g_done[2];
static sem_t g_hook_arrive, g_hook_release; /* W2 reports the hook spot and waits for the release */
static volatile int g_hook_for;             /* which waiter index pauses at the hook (-1 none) */

/* ---- old algorithm (patch 0003): counting semaphore + waiter count ---- */
static sem_t o_sem;
static volatile long o_waiters;
static void old_wait(int idx)
{
    __atomic_fetch_add(&o_waiters, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_m);
    if (idx == g_hook_for) {
        sem_post(&g_hook_arrive);
        sem_wait(&g_hook_release);
    }
    sem_wait(&o_sem);
    pthread_mutex_lock(&g_m);
}
static void old_wake(int all)
{
    if (all) {
        long n = __atomic_exchange_n(&o_waiters, 0, __ATOMIC_SEQ_CST);
        while (n-- > 0) sem_post(&o_sem);
        return;
    }
    for (;;) {
        long w = o_waiters;
        if (w <= 0) return;
        if (__atomic_compare_exchange_n(&o_waiters, &w, w - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            sem_post(&o_sem);
            return;
        }
    }
}

/* ---- new algorithm (patch 1059) ---- */
static struct kz3_lwc_queue n_q;
static void new_wait(int idx)
{
    struct kz3_lwc_waiter me;
    kz3_lwc_enqueue(&n_q, &me, (uint32_t)(idx + 1));
    pthread_mutex_unlock(&g_m);
    if (idx == g_hook_for) {
        sem_post(&g_hook_arrive);
        sem_wait(&g_hook_release);
    }
    kz3_lwc_wait(&n_q, &me, -1);
    pthread_mutex_lock(&g_m);
}
static void new_wake(int all)
{
    kz3_lwc_wake(&n_q, all, 0);
}

static int g_use_new;
struct warg {
    int idx;
    int rounds;
};
static void* waiter(void* p)
{
    struct warg* a = (struct warg*)p;
    pthread_mutex_lock(&g_m);
    for (int r = 0; r < a->rounds; r++) {
        while (!g_flag[a->idx]) {
            if (g_use_new)
                new_wait(a->idx);
            else
                old_wait(a->idx);
        }
        g_flag[a->idx] = 0;
    }
    g_done[a->idx] = 1;
    pthread_mutex_unlock(&g_m);
    return NULL;
}

static int timed_join(pthread_t t, int ms)
{
    for (int i = 0; i < ms / 5; i++) {
        if (pthread_tryjoin_np(t, NULL) == 0) return 1;
        usleep(5000);
    }
    return 0;
}

/* The lost-wakeup scenario; returns 1 when both waiters finished (no wakeup lost). */
static int scenario_signal_all(int use_new)
{
    g_use_new = use_new;
    g_flag[0] = g_flag[1] = 0;
    g_done[0] = g_done[1] = 0;
    g_hook_for = 1;
    o_waiters = 0;
    sem_init(&o_sem, 0, 0);
    kz3_lwc_reset(&n_q);
    sem_init(&g_hook_arrive, 0, 0);
    sem_init(&g_hook_release, 0, 0);
    struct warg a1 = {0, 1}, a2 = {1, 1};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, waiter, &a1);
    usleep(20000); /* W1 is parked in its wait */
    pthread_create(&t2, NULL, waiter, &a2);
    sem_wait(&g_hook_arrive); /* W2 registered, dropped the mutex, paused before the block */
    pthread_mutex_lock(&g_m);
    g_flag[0] = 1;
    g_flag[1] = 1;
    if (use_new)
        new_wake(1);
    else
        old_wake(1); /* signal_all with two waiters queued */
    pthread_mutex_unlock(&g_m);
    usleep(50000); /* W1 wakes, consumes, finishes (rounds = 1) ... */
    /* ... W1 with rounds = 1 just exits; make the re-wait explicit: a third waiter queues now, exactly what a
     * woken W1 that re-waits does (it registers before W2 has run). */
    struct warg a3 = {0, 1};
    pthread_t t3;
    g_flag[0] = 0;
    g_done[0] = 0;
    pthread_create(&t3, NULL, waiter, &a3);
    usleep(50000);             /* the re-waiter is registered and blocking (old: on the semaphore) */
    sem_post(&g_hook_release); /* W2 finally reaches its blocking wait */
    int w2 = timed_join(t2, 1000);
    /* clean up the re-waiter: signal it */
    pthread_mutex_lock(&g_m);
    g_flag[0] = 1;
    if (use_new)
        new_wake(0);
    else
        old_wake(0);
    pthread_mutex_unlock(&g_m);
    int w3 = timed_join(t3, 1000);
    if (!w2) {
        pthread_cancel(t2);
        pthread_join(t2, NULL);
    } /* parked in sem_wait, a cancellation point */
    if (!w3) {
        pthread_cancel(t3);
        pthread_join(t3, NULL);
    }
    pthread_join(t1, NULL);
    printf("  %s: W2 finished=%d  re-waiter finished=%d\n", use_new ? "new" : "old", w2, w3);
    return w2 && w3;
}

/* signal wakes the oldest; signal_to wakes the named thread; a timeout unlinks. */
static int scenario_order_and_timeout(void)
{
    kz3_lwc_reset(&n_q);
    struct kz3_lwc_waiter a, b, c;
    kz3_lwc_enqueue(&n_q, &a, 11);
    kz3_lwc_enqueue(&n_q, &b, 22);
    kz3_lwc_enqueue(&n_q, &c, 33);
    int ok = 1;
    if (kz3_lwc_wake(&n_q, 0, 0) != 1 || !a.woken || b.woken || c.woken) {
        printf("  signal did not wake the oldest only\n");
        ok = 0;
    }
    if (kz3_lwc_wake(&n_q, 0, 33) != 1 || !c.woken || b.woken) {
        printf("  signal_to did not wake the named thread\n");
        ok = 0;
    }
    if (kz3_lwc_wake(&n_q, 0, 99) != 1 || !b.woken) {
        printf("  signal_to(unknown) did not fall back to the oldest\n");
        ok = 0;
    }
    if (kz3_lwc_wake(&n_q, 1, 0) != 0 || n_q.waiters != 0) {
        printf("  signal_all on an empty queue woke %ld\n", n_q.waiters);
        ok = 0;
    }
    struct kz3_lwc_waiter t;
    kz3_lwc_enqueue(&n_q, &t, 44);
    if (kz3_lwc_wait(&n_q, &t, 30) != 0 || n_q.waiters != 0 || n_q.head) {
        printf("  timeout did not unlink\n");
        ok = 0;
    }
    if (kz3_lwc_wake(&n_q, 1, 0) != 0) {
        printf("  stale node woken after a timeout\n");
        ok = 0;
    }
    printf("  order/timeout: %s\n", ok ? "ok" : "FAIL");
    return ok;
}

int main(int argc, char** argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 3;
    kz3_lwc_init(&n_q);
    int old_lost = 0, new_ok = 1;
    for (int i = 0; i < iters; i++) {
        if (!scenario_signal_all(0)) old_lost++;
        if (!scenario_signal_all(1)) new_ok = 0;
    }
    int ord = scenario_order_and_timeout();
    printf("signal_all lost wakeup: old %d/%d runs lost, new %s; order/timeout %s\n", old_lost, iters,
           new_ok ? "never" : "LOST", ord ? "ok" : "FAIL");
    if (!new_ok || !ord) {
        printf("FAIL\n");
        return 1;
    }
    if (old_lost == 0) {
        printf("FAIL: the negative control (old semaphore) did not lose a wakeup\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
