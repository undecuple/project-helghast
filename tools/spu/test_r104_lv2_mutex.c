/* Pass 104 (patch 1122): lv2 semantics of sys_mutex / sys_cond (runtime/syscalls/sys_mutex.c, sys_cond.c), built by
 * tools/spu/test_r104_lv2_sync.sh with the toolkit's tests/sync_stress/test_shims.c. Each case is a difference from
 * lv2 that patch 1122 fixes (idea credit: the Ratchet & Clank port, their patch 7007):
 *   csteal   sys_cond_signal from the mutex owner: at the owner's unlock the mutex goes straight to the woken waiter,
 *            so a third thread's trylock in that gap gets EBUSY (ours woke the waiter and let it re-lock).
 *   munlock  unlock with a thread asleep in lock: lv2 hands the mutex to that thread (FIFO), a barging trylock gets EBUSY.
 *   mdestroy destroy: EBUSY while owned, EPERM while a cond is bound, OK when idle, then ESRCH.
 * Self-contained (no game data). Prints PASS/FAIL per case; exit 1 on any failure. */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

uint8_t* vm_base;
static int g_fail;
static int32_t call(int64_t (*f)(ppu_context*), uint32_t tid, uint64_t a, uint64_t b)
{ ppu_context c; memset(&c, 0, sizeof c); c.thread_id = tid; c.gpr[3] = a; c.gpr[4] = b; return (int32_t)f(&c); }
static uint32_t be(unsigned off) { uint8_t* p = vm_base + off; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static uint32_t mk_mutex(unsigned out, uint32_t recursive)
{
    memset(vm_base + 0x100, 0, 32);
    vm_base[0x103] = SYS_SYNC_FIFO; vm_base[0x107] = (uint8_t)recursive;
    call(sys_mutex_create, 1, out, 0x100); return be(out);
}

static uint32_t mid, cid;
static _Atomic int st, tdone;   /* the new owner keeps the mutex until the thief has tried */
static int32_t wrc, trc;
static void* waiter(void* a)
{
    (void)a;
    call(sys_mutex_lock, 1, mid, 0); st = 1;
    wrc = call(sys_cond_wait, 1, cid, 0);
    while (!tdone) sleep_ms(1);
    call(sys_mutex_unlock, 1, mid, 0);
    return NULL;
}
static void* locker(void* a)
{
    (void)a;
    st = 1; call(sys_mutex_lock, 2, mid, 0);
    while (!tdone) sleep_ms(1);
    call(sys_mutex_unlock, 2, mid, 0);
    return NULL;
}
static void* thief(void* a)
{
    (void)a;
    trc = call(sys_mutex_trylock, 3, mid, 0);
    if (trc == 0) call(sys_mutex_unlock, 3, mid, 0);
    return NULL;
}

static void t_csteal(int iters)
{
    int bad = 0;
    for (int i = 0; i < iters; i++) {
        mid = mk_mutex(0x200, SYS_SYNC_NOT_RECURSIVE);
        call(sys_cond_create, 1, 0x204, mid); cid = be(0x204);
        /* sys_cond_create reads r5 (attr); call() leaves it 0 */
        st = 0; tdone = 0; wrc = -1;
        pthread_t t1, t3; pthread_create(&t1, NULL, waiter, NULL);
        while (!st) sleep_ms(1);
        sleep_ms(3);
        call(sys_mutex_lock, 2, mid, 0); call(sys_cond_signal, 2, cid, 0); call(sys_mutex_unlock, 2, mid, 0);
        uint64_t own = g_sys_mutexes[mid - 1].owner_tid;
        pthread_create(&t3, NULL, thief, NULL); pthread_join(t3, NULL); tdone = 1; pthread_join(t1, NULL);
        if (own != 1 || trc == 0 || wrc != 0) {
            if (bad++ < 3) printf("  csteal: owner after unlock %llu, third-thread trylock 0x%08X, wait 0x%08X\n",
                                  (unsigned long long)own, (uint32_t)trc, (uint32_t)wrc);
        }
        call(sys_cond_destroy, 1, cid, 0); call(sys_mutex_destroy, 1, mid, 0);
    }
    printf("%s csteal (%d/%d iterations wrong)\n", bad ? "FAIL" : "PASS", bad, iters); g_fail |= bad != 0;
}

static void t_munlock(int iters)
{
    int bad = 0;
    for (int i = 0; i < iters; i++) {
        mid = mk_mutex(0x200, SYS_SYNC_NOT_RECURSIVE);
        call(sys_mutex_lock, 1, mid, 0);
        st = 0; tdone = 0;
        pthread_t t2, t3; pthread_create(&t2, NULL, locker, NULL);
        while (!st) sleep_ms(1);
        sleep_ms(3);
        call(sys_mutex_unlock, 1, mid, 0);
        uint64_t own = g_sys_mutexes[mid - 1].owner_tid;
        pthread_create(&t3, NULL, thief, NULL); pthread_join(t3, NULL); tdone = 1; pthread_join(t2, NULL);
        if (own != 2 || trc == 0) {
            if (bad++ < 3) printf("  munlock: owner after unlock %llu, barging trylock 0x%08X\n", (unsigned long long)own, (uint32_t)trc);
        }
        call(sys_mutex_destroy, 1, mid, 0);
    }
    printf("%s munlock (%d/%d iterations wrong)\n", bad ? "FAIL" : "PASS", bad, iters); g_fail |= bad != 0;
}

static void t_mdestroy(void)
{
    int bad = 0;
    mid = mk_mutex(0x200, SYS_SYNC_NOT_RECURSIVE);
    call(sys_cond_create, 1, 0x204, mid); cid = be(0x204);
    call(sys_mutex_lock, 1, mid, 0);
    int32_t d1 = call(sys_mutex_destroy, 1, mid, 0);
    call(sys_mutex_unlock, 1, mid, 0);
    int32_t d2 = call(sys_mutex_destroy, 1, mid, 0);
    call(sys_cond_destroy, 1, cid, 0);
    int32_t d3 = call(sys_mutex_destroy, 1, mid, 0);
    int32_t d4 = call(sys_mutex_destroy, 1, mid, 0);
    bad = d1 != (int32_t)CELL_EBUSY || d2 != (int32_t)CELL_EPERM || d3 != 0 || d4 != (int32_t)CELL_ESRCH;
    printf("%s mdestroy (owned 0x%08X, cond bound 0x%08X, idle 0x%08X, again 0x%08X)\n", bad ? "FAIL" : "PASS",
           (uint32_t)d1, (uint32_t)d2, (uint32_t)d3, (uint32_t)d4);
    g_fail |= bad;
}

int main(int argc, char** argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 50;
    vm_base = calloc(1, 65536);
    setvbuf(stdout, NULL, _IONBF, 0);
    t_mdestroy();
    t_munlock(iters);
    t_csteal(iters);
    return g_fail ? 1 : 0;
}
