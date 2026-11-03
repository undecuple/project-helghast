/* Spu pass 139 host test: ps3_evcv (patch 1148) on win32_compat's POSIX shim (PS3_EVCV_SHIM). The Wine futex-queue race
 * itself needs Wine; this checks the contract the port relies on: no lost wakes, FIFO wake_one, wake_all, the timeout
 * result (FALSE + ERROR_TIMEOUT, the lock held again), and no stale event after a wake that met a timeout. */
#define PS3_EVCV_SHIM 1
#include "ps3_evcv.h"
#include "win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static SRWLOCK lk = SRWLOCK_INIT;
static CRITICAL_SECTION cs;
static ps3_evcv cv = PS3_EVCV_INIT;
static ps3_evcv cvc = PS3_EVCV_INIT;
static int ready, waiting, order_n, order[8], tokens, consumed;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void ms_sleep(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, 0); }

/* 1. timeout: FALSE, ERROR_TIMEOUT, elapsed >= ms, lock held on return (for SRW and CS) */
static void t_timeout(void)
{
    AcquireSRWLockExclusive(&lk);
    double t0 = now_ms();
    BOOL ok = ps3_evcv_sleep_srw(&cv, &lk, 40);
    double dt = now_ms() - t0;
    CHECK(!ok && GetLastError() == ERROR_TIMEOUT && dt >= 35 && dt < 1000);
    CHECK(!TryAcquireSRWLockExclusive(&lk));   /* still held by us */
    ReleaseSRWLockExclusive(&lk);
    EnterCriticalSection(&cs);
    ok = ps3_evcv_sleep_cs(&cvc, &cs, 20);
    CHECK(!ok && GetLastError() == ERROR_TIMEOUT);
    LeaveCriticalSection(&cs);
    CHECK(cv.w == 0 && cvc.w == 0);   /* the timed-out waiters unlinked themselves */
}

/* 2. wake_one is FIFO */
static void* w_order(void* a)
{
    AcquireSRWLockExclusive(&lk);
    waiting++;
    while (ready <= (int)(intptr_t)a) ps3_evcv_sleep_srw(&cv, &lk, INFINITE);
    order[order_n++] = (int)(intptr_t)a;
    ReleaseSRWLockExclusive(&lk);
    return 0;
}
static void t_fifo(void)
{
    pthread_t th[3]; ready = 0; waiting = 0; order_n = 0;
    for (int i = 0; i < 3; i++) {
        pthread_create(&th[i], 0, w_order, (void*)(intptr_t)i);
        for (;;) { AcquireSRWLockExclusive(&lk); int w = waiting; ReleaseSRWLockExclusive(&lk); if (w == i + 1) break; ms_sleep(1); }
        ms_sleep(5);   /* the waiter has linked itself and dropped the lock only once it sleeps */
    }
    for (int i = 0; i < 3; i++) {
        AcquireSRWLockExclusive(&lk); ready = i + 1; ReleaseSRWLockExclusive(&lk);
        ps3_evcv_wake_all(&cv);   /* all wake; the predicate lets only waiter i through */
        ms_sleep(10);
    }
    for (int i = 0; i < 3; i++) pthread_join(th[i], 0);
    CHECK(order_n == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
}

/* 3. producer/consumer with short timed waits: every token is consumed, none lost, whatever the timeout/wake mix */
static void* consumer(void* a)
{
    (void)a;
    AcquireSRWLockExclusive(&lk);
    while (consumed < 20000) {
        if (tokens > 0) { tokens--; consumed++; continue; }
        ps3_evcv_sleep_srw(&cv, &lk, 1);
    }
    ReleaseSRWLockExclusive(&lk);
    return 0;
}
static void t_stress(void)
{
    pthread_t th[4]; tokens = 0; consumed = 0;
    for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, consumer, 0);
    for (int i = 0; i < 20000; i++) {
        AcquireSRWLockExclusive(&lk); tokens++; ReleaseSRWLockExclusive(&lk);
        if (i & 1) ps3_evcv_wake_one(&cv); else ps3_evcv_wake_all(&cv);
        if ((i & 255) == 0) ms_sleep(2);   /* let the timeout path win sometimes */
    }
    for (int i = 0; i < 4; i++) { ps3_evcv_wake_all(&cv); pthread_join(th[i], 0); }
    CHECK(consumed == 20000 && tokens == 0);
}

/* 4. a wake that meets a timeout leaves no stale event: after many racing rounds the same thread's next wait with no
 *    waker still times out (an event left set would return TRUE at once). */
static volatile int stop_waker;
static void* waker(void* a) { (void)a; while (!stop_waker) { ps3_evcv_wake_one(&cv); } return 0; }
static void t_stale(void)
{
    pthread_t th; stop_waker = 0; pthread_create(&th, 0, waker, 0);
    for (int i = 0; i < 3000; i++) { AcquireSRWLockExclusive(&lk); ps3_evcv_sleep_srw(&cv, &lk, 1); ReleaseSRWLockExclusive(&lk); }
    stop_waker = 1; pthread_join(th, 0);
    int spurious = 0;
    for (int i = 0; i < 20; i++) {
        AcquireSRWLockExclusive(&lk);
        BOOL ok = ps3_evcv_sleep_srw(&cv, &lk, 2);
        ReleaseSRWLockExclusive(&lk);
        if (ok) spurious++;
    }
    CHECK(spurious == 0);
    CHECK(cv.w == 0);
}

/* 5. a wake with no waiter leaves the word untouched (the fast path) */
static void t_nowaiter(void) { ps3_evcv_wake_one(&cv); ps3_evcv_wake_all(&cv); CHECK(cv.w == 0); }

int main(void)
{
    InitializeCriticalSection(&cs);
    t_timeout(); t_fifo(); t_stress(); t_stale(); t_nowaiter();
    printf(fails ? "evcv: %d FAIL\n" : "evcv: all PASS\n", fails);
    return fails != 0;
}
