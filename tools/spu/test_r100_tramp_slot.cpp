// Patch 1115 host test (SPU pass 100): the lifter's PPU_TRAMP_TEB preamble (g_trampoline_fn through a pointer cached
// in a TEB TLS slot) keeps the per-thread semantics of the TLS variable: every thread sees only its own trampoline,
// the slot path and code naming the TLS variable directly reach the same object, a thread with an empty slot (or a
// process without the fast path) goes through the slow path and gets the same result. The TEB is emulated here (one
// thread-local byte array; PPU_TRAMP_GS_LOAD reads it); the real gs:0x1480 layout is checked at run time by
// ppu_tramp_ref_slow's self-check ("[tramp] ... on idx=N" in the log). The slow path mirrors runtime/ppu/ppu_loader.cpp.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define PPU_THREAD_LOCAL __thread
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" { PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr; }
static void (**direct_ref())(void*) { return &g_trampoline_fn; }   // the runtime's view (the TLS variable by name)

static thread_local unsigned char t_teb[0x1480 + 8 * 64];          // emulated TEB of this thread
#define PPU_TRAMP_GS_LOAD(o, p) ((p) = *(void**)(t_teb + (o)))
#define PPU_TRAMP_TEB 1
extern "C" unsigned g_ppu_tramp_slot;
unsigned g_ppu_tramp_slot = 0;
static std::atomic<int> s_state{0};
static int s_idx = -1, s_fast = 1;
static std::atomic<long> s_slow{0};
extern "C" void (**ppu_tramp_ref_slow(void))(void*)
{
    s_slow++;
    void (**p)(void*) = direct_ref();
    int z = 0;
    if (s_state.load() != 2) {
        if (s_state.compare_exchange_strong(z, 1)) { s_idx = s_fast ? 5 : -1; s_state.store(2); }
        else while (s_state.load() != 2) std::this_thread::yield();
    }
    if (s_idx < 0) return p;
    void** slot = (void**)(t_teb + 0x1480 + 8 * s_idx);              // TlsSetValue
    if (*slot != (void*)p) *slot = (void*)p;
    if (!g_ppu_tramp_slot) {
        void* q; PPU_TRAMP_GS_LOAD(0x1480ull + 8ull * s_idx, q);     // self-check
        if (q == (void*)p) g_ppu_tramp_slot = 0x1480u + 8u * s_idx;
    }
    return p;
}

#include "preamble.h"   // the lifter's SOURCE_PREAMBLE block, extracted by the .sh

static thread_local long t_sum;
static thread_local int t_id;
static void f3(void*) { t_sum += 1000 * (t_id + 1); }
static void f2(void*) { t_sum += 100; g_trampoline_fn = f3; }       // a tail call by trampoline, as the lifter emits
static void f1(void* c) { t_sum += 10; g_trampoline_fn = f2; (void)c; }

static int run(int nthr, int iters)
{
    std::atomic<int> bad{0};
    std::vector<std::thread> v;
    for (int t = 0; t < nthr; t++)
        v.emplace_back([&, t] {
            t_id = t; t_sum = 0;
            for (int i = 0; i < iters; i++) {
                if (g_trampoline_fn) bad++;                         // starts empty on every thread
                f1(nullptr);
                if (*direct_ref() != (void (*)(void*))f2) bad++;    // the same object by name
                DRAIN_TRAMPOLINE(nullptr);
                if (*direct_ref() || g_trampoline_fn) bad++;
                *direct_ref() = f3;                                 // runtime sets, generated code drains
                DRAIN_TRAMPOLINE(nullptr);
                std::this_thread::yield();
            }
            if (t_sum != (long)iters * (110 + 2000L * (t + 1))) bad++;
        });
    for (auto& x : v) x.join();
    return bad.load();
}

int main()
{
    int fails = 0, n = 0;
    s_fast = 1; s_slow = 0;
    int b = run(8, 20000); n++;
    long slow_fast = s_slow.load();
    printf("fast path: bad=%d slow calls=%ld (<= 2 per thread expected) slot=0x%x\n", b, slow_fast, g_ppu_tramp_slot);
    if (b || slow_fast > 16 || g_ppu_tramp_slot != 0x1480u + 8u * 5) fails++;
    s_state = 0; g_ppu_tramp_slot = 0; s_fast = 0; s_slow = 0;
    b = run(8, 5000); n++;
    printf("fast path off: bad=%d slow calls=%ld slot=0x%x\n", b, s_slow.load(), g_ppu_tramp_slot);
    if (b || s_slow.load() < 8L * 5000 || g_ppu_tramp_slot) fails++;
    printf("%s: %d/%d cases\n", fails ? "FAIL" : "PASS", n - fails, n);
    return fails ? 1 : 0;
}
