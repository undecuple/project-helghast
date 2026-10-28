/* Pass 104 (patch 1122): lv2 semantics of the HLE sys_lwmutex / sys_lwcond in runtime/ppu/ppu_sysprx.cpp, driven
 * through the registered handlers (ppu_sysprx_register) on the host. Built by tools/spu/test_r104_lv2_sync.sh against
 * the patched toolkit with stubs for everything else ppu_sysprx.cpp links. Each case is a difference from lv2 that
 * patch 1122 fixes (idea credit the Ratchet & Clank port's notes, their patch 7007):
 *   steal1  lwcond signal from the lwmutex owner (lv2 mode 1): at the owner's unlock the lwmutex goes straight to the
 *           woken waiter, so a third thread's trylock in that gap gets EBUSY (ours let the waiter re-lock it itself).
 *   steal3  signal while the lwmutex is free (lv2 mode 3): the waiter owns it as the signal returns.
 *   unlock  unlock with a thread asleep in lock: lv2 hands the lwmutex to that thread, a barging trylock gets EBUSY.
 *   ebusy   trylock on a busy lwmutex returns EBUSY 0x8001000A (ours 0x8001000B).
 *   deadlk  a non-recursive lwmutex re-locked by its owner: EDEADLK 0x80010008 (lock and trylock); recursive counts.
 *   destroy lwmutex destroy while owned -> EBUSY, lwcond destroy while waited on -> EBUSY, else OK.
 *   fifo    patch 1059's order kept: signal_all wakes the queued waiters, which then own the lwmutex in queue order.
 *   timeout wait timeout returns ETIMEDOUT holding the lwmutex at the old recursion depth.
 * Self-contained (no game data). Prints PASS/FAIL per case; exit 1 on any failure. */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <map>
#include <string>
#include <vector>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x00F00000u;
volatile long long g_gcm_ref_pub_count;
static std::map<uint32_t, void (*)(ppu_context*)>* g_fn;
void ps3_hle_register_ctx(uint32_t nid, const char*, void (*fn)(ppu_context*))
{ if (!g_fn) g_fn = new std::map<uint32_t, void (*)(ppu_context*)>; if (!g_fn->count(nid)) (*g_fn)[nid] = fn; }
uint32_t vm_read32(uint64_t a) { const uint8_t* p = vm_base + (uint32_t)a; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t vm_read64(uint64_t a) { return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4); }
void vm_write32(uint64_t a, uint32_t v) { uint8_t* p = vm_base + (uint32_t)a; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
void vm_write64(uint64_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
void ppu_sysprx_register(void);
void avconf_register_ctx(void) {}
void cellGcm_fifo_recycle(unsigned) {}
unsigned cellGcmSetupContext(unsigned, unsigned, unsigned, unsigned, void*, void*) { return 0; }
void cellSaveData_register_ctx_handlers(void) {}
void np_score_register_ctx(void) {}
void ppu_register_function(uint64_t, void (*)(ppu_context*)) {}
unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
unsigned ppu_thread_prof_pc(unsigned) { return 0; }
void ps3_net_host_register(unsigned (*)(unsigned, unsigned)) {}
int ps3_net_online(void) { return 0; }
void spu_raw_note_image(uint32_t, uint32_t) {}
int64_t sys_ppu_thread_create(ppu_context*) { return 0; }
int64_t sys_ppu_thread_exit(ppu_context*) { return 0; }
}

static int g_fail;
static void (*H(const char* n))(ppu_context*)
{ auto it = g_fn->find(ps3_compute_nid(n)); if (it == g_fn->end()) { printf("FAIL: %s not registered\n", n); exit(1); } return it->second; }
static int32_t call(const char* n, uint32_t tid, uint64_t a = 0, uint64_t b = 0)
{ ppu_context c; memset(&c, 0, sizeof c); c.thread_id = tid; c.gpr[3] = a; c.gpr[4] = b; H(n)(&c); return (int32_t)c.gpr[3]; }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static int g_prints;
/* Counts a failed check into `bad` (once per iteration: callers pass a per-iteration counter); prints the first few. */
static void check(const char* tcase, bool ok, const char* what, int& bad)
{ if (!ok) { bad++; if (g_prints++ < 6) printf("  %s: %s\n", tcase, what); } }

enum : uint32_t { ATTR = 0x100, LWM = 0x200, LWC = 0x300, LWM2 = 0x400, LWC2 = 0x500 };
static const int32_t EBUSY_ = (int32_t)0x8001000A, EDEADLK_ = (int32_t)0x80010008, ETIMEDOUT_ = (int32_t)0x8001000B;
static void mk_lwmutex(uint32_t lwm, uint32_t recursive)
{ vm_write32(ATTR, 2 /* SYS_SYNC_PRIORITY */); vm_write32(ATTR + 4, recursive); call("sys_lwmutex_create", 1, lwm, ATTR); }
static uint32_t owner(uint32_t lwm) { return vm_read32(lwm); }

/* steal (mode 1 or 3): T1 waits on the lwcond; main (tid 2) signals with or without the lwmutex; right after the
 * lwmutex is free from main's point of view, T3's trylock must see it owned by T1. */
static void t_steal(int mode, int iters)
{
    const char* name = mode == 1 ? "steal1" : "steal3"; int bad = 0;
    for (int i = 0; i < iters; i++) {
        mk_lwmutex(LWM, 0x10); call("sys_lwcond_create", 1, LWC, LWM);
        std::atomic<int> st{0}, tdone{0}; int32_t wrc = -1;
        /* T1 keeps the lwmutex until T3 has tried it, so a correct hand-off is visible as EBUSY. */
        std::thread t1([&] { call("sys_lwmutex_lock", 1, LWM, 0); st = 1; wrc = call("sys_lwcond_wait", 1, LWC, 0);
                             st = 2; while (!tdone.load()) sleep_ms(1); call("sys_lwmutex_unlock", 1, LWM); });
        while (st.load() < 1) sleep_ms(1);
        sleep_ms(3);                                   /* T1 is parked in the wait */
        if (mode == 1) { call("sys_lwmutex_lock", 2, LWM, 0); call("sys_lwcond_signal", 2, LWC); call("sys_lwmutex_unlock", 2, LWM); }
        else call("sys_lwcond_signal", 2, LWC);
        int32_t trc = 0; uint32_t own = owner(LWM);
        std::thread t3([&] { trc = call("sys_lwmutex_trylock", 3, LWM); if (trc == 0) call("sys_lwmutex_unlock", 3, LWM); });
        t3.join(); tdone = 1; t1.join();
        int nb = 0;
        check(name, own == 1, "lwmutex not handed to the woken waiter at the signal/unlock", nb);
        check(name, trc != 0, "third thread took the lwmutex in the gap", nb);
        check(name, wrc == 0, "wait did not return OK", nb);
        bad += nb != 0;
        call("sys_lwcond_destroy", 1, LWC); call("sys_lwmutex_destroy", 1, LWM);
    }
    printf("%s %s (%d/%d iterations wrong)\n", bad ? "FAIL" : "PASS", name, bad, iters); g_fail |= bad != 0;
}

static void t_unlock(int iters)
{
    int bad = 0;
    for (int i = 0; i < iters; i++) {
        mk_lwmutex(LWM, 0x10);
        call("sys_lwmutex_lock", 1, LWM, 0);
        std::atomic<int> st{0}, tdone{0};
        std::thread t2([&] { st = 1; call("sys_lwmutex_lock", 2, LWM, 0); st = 2; while (!tdone.load()) sleep_ms(1);
                             call("sys_lwmutex_unlock", 2, LWM); });
        while (st.load() < 1) sleep_ms(1);
        sleep_ms(3);                                   /* T2 is asleep in lock */
        call("sys_lwmutex_unlock", 1, LWM);
        uint32_t own = owner(LWM); int32_t trc = 0;
        std::thread t3([&] { trc = call("sys_lwmutex_trylock", 3, LWM); if (trc == 0) call("sys_lwmutex_unlock", 3, LWM); });
        t3.join(); tdone = 1; t2.join();
        int nb = 0;
        check("unlock", own == 2, "unlock did not hand the lwmutex to the sleeping waiter", nb);
        check("unlock", trc != 0, "barging trylock took the lwmutex from the waiter", nb);
        bad += nb != 0;
        call("sys_lwmutex_destroy", 1, LWM);
    }
    printf("%s unlock (%d/%d iterations wrong)\n", bad ? "FAIL" : "PASS", bad, iters); g_fail |= bad != 0;
}

static void t_codes()
{
    int bad = 0;
    mk_lwmutex(LWM, 0x10);
    call("sys_lwmutex_lock", 1, LWM, 0);
    int32_t r = call("sys_lwmutex_trylock", 2, LWM);
    check("ebusy", r == EBUSY_, "busy trylock is not EBUSY 0x8001000A", bad);
    call("sys_lwmutex_unlock", 1, LWM); call("sys_lwmutex_destroy", 1, LWM);
    printf("%s ebusy (trylock -> 0x%08X)\n", bad ? "FAIL" : "PASS", (uint32_t)r); g_fail |= bad != 0;

    bad = 0;
    mk_lwmutex(LWM, 0x20);                            /* SYS_SYNC_NOT_RECURSIVE */
    call("sys_lwmutex_lock", 1, LWM, 0);
    int32_t r1 = call("sys_lwmutex_lock", 1, LWM, 0), r2 = call("sys_lwmutex_trylock", 1, LWM);
    check("deadlk", r1 == EDEADLK_, "non-recursive re-lock is not EDEADLK", bad);
    check("deadlk", r2 == EDEADLK_, "non-recursive re-trylock is not EDEADLK", bad);
    call("sys_lwmutex_unlock", 1, LWM);
    int32_t r3 = call("sys_lwmutex_trylock", 2, LWM);   /* one unlock freed it (no hidden count) */
    check("deadlk", r3 == 0, "lwmutex still held after one unlock", bad);
    if (r3 == 0) call("sys_lwmutex_unlock", 2, LWM);
    call("sys_lwmutex_destroy", 1, LWM);
    mk_lwmutex(LWM, 0x10);                            /* SYS_SYNC_RECURSIVE: counts */
    call("sys_lwmutex_lock", 1, LWM, 0);
    int32_t r4 = call("sys_lwmutex_lock", 1, LWM, 0);
    call("sys_lwmutex_unlock", 1, LWM);
    int32_t r5 = call("sys_lwmutex_trylock", 2, LWM);
    check("deadlk", r4 == 0 && r5 == EBUSY_, "recursive lwmutex did not count", bad);
    call("sys_lwmutex_unlock", 1, LWM); call("sys_lwmutex_destroy", 1, LWM);
    printf("%s deadlk (relock 0x%08X, retry 0x%08X)\n", bad ? "FAIL" : "PASS", (uint32_t)r1, (uint32_t)r2); g_fail |= bad != 0;

    bad = 0;
    mk_lwmutex(LWM2, 0x10); call("sys_lwcond_create", 1, LWC2, LWM2);
    call("sys_lwmutex_lock", 1, LWM2, 0);
    int32_t d1 = call("sys_lwmutex_destroy", 1, LWM2);
    check("destroy", d1 == EBUSY_, "lwmutex destroy while owned is not EBUSY", bad);
    std::atomic<int> st{0};
    std::thread tw([&] { call("sys_lwmutex_lock", 2, LWM2, 0); st = 1; call("sys_lwcond_wait", 2, LWC2, 0); call("sys_lwmutex_unlock", 2, LWM2); });
    call("sys_lwmutex_unlock", 1, LWM2);
    while (st.load() < 1) sleep_ms(1);
    sleep_ms(3);
    int32_t d2 = call("sys_lwcond_destroy", 1, LWC2);
    check("destroy", d2 == EBUSY_, "lwcond destroy while waited on is not EBUSY", bad);
    call("sys_lwcond_signal", 1, LWC2); tw.join();
    int32_t d3 = call("sys_lwcond_destroy", 1, LWC2), d4 = call("sys_lwmutex_destroy", 1, LWM2);
    check("destroy", d3 == 0 && d4 == 0, "destroy of an idle lwcond/lwmutex failed", bad);
    printf("%s destroy (owned 0x%08X, waited 0x%08X, idle %d/%d)\n", bad ? "FAIL" : "PASS", (uint32_t)d1, (uint32_t)d2, d3, d4);
    g_fail |= bad != 0;
}

static void t_fifo()
{
    int bad = 0;
    mk_lwmutex(LWM, 0x10); call("sys_lwcond_create", 1, LWC, LWM);
    std::vector<int> order; std::atomic<int> parked{0};
    std::vector<std::thread> th;
    for (int k = 0; k < 3; k++) {
        th.emplace_back([&, k] { call("sys_lwmutex_lock", 10 + k, LWM, 0); parked++;
                                 call("sys_lwcond_wait", 10 + k, LWC, 0); order.push_back(10 + k); call("sys_lwmutex_unlock", 10 + k, LWM); });
        while (parked.load() < k + 1) sleep_ms(1);
        sleep_ms(3);
    }
    call("sys_lwmutex_lock", 2, LWM, 0); call("sys_lwcond_signal_all", 2, LWC); call("sys_lwmutex_unlock", 2, LWM);
    for (auto& t : th) t.join();
    check("fifo", order.size() == 3 && order[0] == 10 && order[1] == 11 && order[2] == 12, "waiters did not own the lwmutex in queue order", bad);
    /* signal_to names the middle waiter of three */
    order.clear(); parked = 0; th.clear();
    for (int k = 0; k < 3; k++) {
        th.emplace_back([&, k] { call("sys_lwmutex_lock", 20 + k, LWM, 0); parked++;
                                 call("sys_lwcond_wait", 20 + k, LWC, 0); order.push_back(20 + k); call("sys_lwmutex_unlock", 20 + k, LWM); });
        while (parked.load() < k + 1) sleep_ms(1);
        sleep_ms(3);
    }
    call("sys_lwcond_signal_to", 2, LWC, 21); sleep_ms(5);
    call("sys_lwcond_signal", 2, LWC); sleep_ms(5);
    call("sys_lwcond_signal", 2, LWC);
    for (auto& t : th) t.join();
    check("fifo", order.size() == 3 && order[0] == 21 && order[1] == 20 && order[2] == 22, "signal_to/signal order wrong", bad);
    call("sys_lwcond_destroy", 1, LWC); call("sys_lwmutex_destroy", 1, LWM);
    printf("%s fifo\n", bad ? "FAIL" : "PASS"); g_fail |= bad != 0;
}

static void t_timeout()
{
    int bad = 0;
    mk_lwmutex(LWM, 0x10); call("sys_lwcond_create", 1, LWC, LWM);
    call("sys_lwmutex_lock", 1, LWM, 0); call("sys_lwmutex_lock", 1, LWM, 0);
    int32_t r = call("sys_lwcond_wait", 1, LWC, 20000);
    check("timeout", r == ETIMEDOUT_, "wait timeout is not ETIMEDOUT", bad);
    check("timeout", owner(LWM) == 1 && vm_read32(LWM + 0x0C) == 2, "lwmutex not re-held at depth 2", bad);
    call("sys_lwmutex_unlock", 1, LWM); call("sys_lwmutex_unlock", 1, LWM);
    int32_t t = call("sys_lwmutex_trylock", 2, LWM);
    check("timeout", t == 0, "lwmutex still held after two unlocks", bad);
    if (t == 0) call("sys_lwmutex_unlock", 2, LWM);
    call("sys_lwcond_destroy", 1, LWC); call("sys_lwmutex_destroy", 1, LWM);
    printf("%s timeout\n", bad ? "FAIL" : "PASS"); g_fail |= bad != 0;
}

int main(int argc, char** argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 50;
    vm_base = (uint8_t*)calloc(1, 0x10000);
    ppu_sysprx_register();
    setvbuf(stdout, NULL, _IONBF, 0);
    t_codes();
    t_unlock(iters);
    t_steal(1, iters);
    t_steal(3, iters);
    t_fifo();
    t_timeout();
    return g_fail ? 1 : 0;
}

/* v0.18.0 (SPU pass 108): ppu_sysprx.cpp's lwcond timeouts use sys_timer.c's host_monotonic_ns (#577); this
 * fixture does not link sys_timer.c, so it brings the same clock. */
#include <time.h>
extern "C" uint64_t host_monotonic_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* v0.18.0: ppu_sysprx.cpp also registers #577's fast forms (ppu_hle.cpp, not linked here); the fixture calls the
 * registered handlers, so the fast table is not needed. */
extern "C" void ps3_hle_register_fast(uint32_t, int (*)(ppu_context*)) {}
