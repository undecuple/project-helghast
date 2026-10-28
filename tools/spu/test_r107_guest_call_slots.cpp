// Pass 107, patch 1128: callback-stack slot reuse. Adapted (MIT) from the Killzone 2 port's
// tools/runtime/test_guest_call_stack.cpp (patch 9489), itself built on the PR #489 fixture
// (runtime/ppu/tests/test_ppu_guest_call_stack.cpp, erikomis, MIT).
//
// Two host threads run a callback at once (each in its own stack slot); then 100 short-lived host threads make one
// callback each and finish (they call ppu_guest_call_stack_release, as sys_ppu_thread.c's thread proc does); then two
// host threads are inside a callback at once again and must still get two different stacks. Without slot reuse the
// 100 threads use up the 64 slots and the last pair shares the last one.
// Built by tools/spu/test_r106_guest_call_stack.sh against the patched toolkit's real ppu_loader.cpp.
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
const func_entry function_table[1] = {{0, nullptr, nullptr}};   // no lifted program: registered by hand below
const uint64_t function_table_count = 0;
uint32_t ps3_hle_count(void) { return 0; }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
uint64_t ppu_guest_call(uint32_t opd, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);
// Patch 1128; weak so the fixture also builds on a tree with 1126 only (and then shows the missing reuse).
void ppu_guest_call_stack_release(void) __attribute__((weak));
}

static const uint32_t OPD = 0x1000, CODE = 0x10000;
static const uint32_t FRAME = 0x200;

static void be32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(vm_base + ea, &v, 4); }
static uint32_t rd32(uint32_t ea) { uint32_t v; std::memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }

enum { THREADS = 2 };
struct record { uint32_t sp; int ok; };
static record s_rec[THREADS];
static volatile int s_inside;

static void wait_for(volatile int* n, int want)
{
    __atomic_add_fetch(n, 1, __ATOMIC_SEQ_CST);
    for (long i = 0; i < 200000000L && __atomic_load_n(n, __ATOMIC_SEQ_CST) < want; i++) sched_yield();
}

// gpr[3] = thread index, gpr[4] = 1: pair callback (rendezvous with the other thread), 2: plain short callback.
static void callback(ppu_context* ctx)
{
    const int t = (int)ctx->gpr[3];
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    const uint32_t tag = 0xC0DE0000u | (uint32_t)(t << 8) | (uint32_t)ctx->gpr[4];
    for (uint32_t o = 4; o <= FRAME; o += 4) be32(sp - o, tag);
    if (ctx->gpr[4] == 2) return;
    s_rec[t].sp = sp;
    wait_for(&s_inside, THREADS);                  // both frames are live now
    int ok = 1;
    for (uint32_t o = 4; o <= FRAME; o += 4) if (rd32(sp - o) != tag) ok = 0;
    s_rec[t].ok = ok;
}

static void release_slot() { if (ppu_guest_call_stack_release) ppu_guest_call_stack_release(); }

static void* pair_thread(void* arg)
{
    ppu_guest_call(OPD, (uint64_t)(intptr_t)arg, 1, 0, 0, 0, 0, 0, 0);
    release_slot();
    return nullptr;
}
static void* short_thread(void*)
{
    ppu_guest_call(OPD, 0, 2, 0, 0, 0, 0, 0, 0);
    release_slot();
    return nullptr;
}

static void run_pair()
{
    s_inside = 0;
    std::memset(s_rec, 0, sizeof s_rec);
    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++) pthread_create(&th[t], nullptr, pair_thread, (void*)(intptr_t)t);
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], nullptr);
}

int main()
{
    void* p = mmap(nullptr, 0x100010000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 1; }
    vm_base = (uint8_t*)p;
    be32(OPD, CODE);
    be32(OPD + 4, 0x8000);
    ppu_register_function(CODE, callback);

    unsigned fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; };

    run_pair();
    std::printf("before: callback sp 0x%08X, 0x%08X\n", s_rec[0].sp, s_rec[1].sp);
    check(s_rec[0].sp != s_rec[1].sp && s_rec[0].ok && s_rec[1].ok, "two concurrent host threads get different stacks");

    for (int i = 0; i < 100; i++) {                // 100 host threads, one after another, each make a callback and exit
        pthread_t th;
        pthread_create(&th, nullptr, short_thread, nullptr);
        pthread_join(th, nullptr);
    }
    run_pair();
    std::printf("after 100 finished host threads: callback sp 0x%08X, 0x%08X\n", s_rec[0].sp, s_rec[1].sp);
    check(s_rec[0].sp != s_rec[1].sp && s_rec[0].ok && s_rec[1].ok,
          "after 100 finished host threads, two concurrent ones still get different stacks");
    return fails ? 1 : 0;
}
