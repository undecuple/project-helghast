// Patch 1116 host test, part 2 (SPU pass 101): runs the guest program tools/spu/test_r101_setjmp_gen.py lifted with
// the toolkit's ppu_lifter.py, against the toolkit's own runtime half (runtime/ppu/ppu_setjmp_rt.inc), with a small
// big-endian guest memory per thread. Cases (each fails with the unpatched lifter, except the control):
//   control  no throw;  throw  one pcall, longjmp from its body;  nested  pcall inside pcall, both throw;
//   deep     longjmp three lifted frames and a host frame below the pcall (dispatch depth restored);
//   stale    a record left below the next pcall's r1 is dropped; 300 pcalls in a row leave no records behind;
//   threads  two threads, 2000 rounds of throw/nested/deep each;  fallback  longjmp with no record returns, counted.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <thread>
#define PPU_THREAD_LOCAL __thread
/* v0.18.0 (SPU pass 108): the lifter opens every body with PPU_MM_SNAP (upstream #574's memory-model copy, ppu_recomp.h);
 * this fixture's accessors are plain vm_read*, so it takes nothing. */
#ifndef PPU_MM_SNAP
#define PPU_MM_SNAP
#endif
#include "ppu_context.h"

static thread_local uint8_t* t_mem;   // 1 MiB of guest memory per thread
static inline uint8_t* gp(uint64_t a) { if ((uint32_t)a >= (1u << 20) - 8) { fprintf(stderr, "guest addr 0x%llx\n", (unsigned long long)a); abort(); } return t_mem + (uint32_t)a; }
static inline uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, gp(a), 8); return __builtin_bswap64(v); }
static inline uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, gp(a), 4); return __builtin_bswap32(v); }
static inline void vm_write64(uint64_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(gp(a), &v, 8); }
static inline void vm_write32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(gp(a), &v, 4); }

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;
#define DRAIN_TRAMPOLINE(ctx) do { while (g_trampoline_fn) { void (*_tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; _tf((void*)(ctx)); } } while (0)
static PPU_THREAD_LOCAL int t_depth, t_vsp;
extern "C" int* ps3_ppu_dispatch_depth_slot(void) { return &t_depth; }
extern "C" int* ps3_ppu_vcall_sp_slot(void) { return &t_vsp; }

#include "syms.h"
#if SJ_PATCHED
#  include "ppu_setjmp_rt.inc"
#endif
#include "lifted.inc"

// The host function D2 calls: like ps3_indirect_call, it counts its depth around the guest call; a landing above
// skips the decrement and must restore the count.
extern "C" void func_00010F00(ppu_context* ctx) { t_depth += 3; t_vsp += 2; F_BODY_THROW5(ctx); DRAIN_TRAMPOLINE(ctx); t_depth -= 3; t_vsp -= 2; }

static const uint64_t kStack = 0xF0000, kLr = 0xDEAD0000;
static int call(ppu_context* c, void (*f)(ppu_context*), uint64_t r3 = 0)
{
    c->gpr[3] = r3; c->lr = kLr; f(c); DRAIN_TRAMPOLINE(c);
    return (int32_t)c->gpr[3];
}

struct Res { int fails = 0; char first[200] = ""; };
static void expect(Res& r, const char* what, long long got, long long want)
{
    if (got == want) return;
    if (!r.fails++) snprintf(r.first, sizeof r.first, "%s: got %lld, want %lld", what, got, want);
}

static void setup(ppu_context* c)
{
    if (!t_mem) t_mem = (uint8_t*)calloc(1, 1u << 20);
    memset(c, 0, sizeof *c);
    c->gpr[1] = kStack; c->gpr[2] = 0x2222; c->gpr[30] = 0x3030; c->gpr[31] = 0x3131;
}

static void base_checks(Res& r, ppu_context* c, const char* tag)
{
    char w[64];
    snprintf(w, sizeof w, "%s r1", tag); expect(r, w, (long long)c->gpr[1], kStack);
    snprintf(w, sizeof w, "%s r31", tag); expect(r, w, (long long)c->gpr[31], 0x3131);
    snprintf(w, sizeof w, "%s longjmp fell through", tag); expect(r, w, (long long)vm_read64(G_BUG), 0);
    snprintf(w, sizeof w, "%s current jmp_buf", tag); expect(r, w, (long long)vm_read64(G_JB), 0);
    snprintf(w, sizeof w, "%s dispatch depth", tag); expect(r, w, t_depth, 0);
    snprintf(w, sizeof w, "%s vcall sp", tag); expect(r, w, t_vsp, 0);
}

static Res case_control() { Res r; ppu_context c; setup(&c); expect(r, "pcall(ok)", call(&c, F_PCALL_OK), 0); base_checks(r, &c, "ok"); return r; }
static Res case_throw() { Res r; ppu_context c; setup(&c); expect(r, "pcall(throw 7)", call(&c, F_PCALL_THROW7), 107); base_checks(r, &c, "throw"); return r; }
static Res case_nested()
{
    Res r; ppu_context c; setup(&c);
    expect(r, "outer pcall (throw 9)", call(&c, F_PCALL_NESTED), 109);
    expect(r, "inner pcall (throw 7)", (long long)vm_read64(G_NEST), 107);
    base_checks(r, &c, "nested"); return r;
}
static Res case_deep() { Res r; ppu_context c; setup(&c); expect(r, "pcall(deep throw 5)", call(&c, F_PCALL_DEEP), 105); base_checks(r, &c, "deep"); return r; }
static Res case_stale()
{
    Res r; ppu_context c; setup(&c);
#if SJ_PATCHED
    t_sj_n = 0;   // test only: start from an empty table (earlier cases leave their landed records)
    expect(r, "pcall one frame deeper", call(&c, F_PCALL_OK_DEEP), 0);
    expect(r, "records after it (stale, below the next r1)", t_sj_n, 1);
    t_sj[0].hframe = UINTPTR_MAX;   // hide it from the host-frame rule: only its guest r1 can drop it
    expect(r, "pcall(throw 7) after the stale record", call(&c, F_PCALL_THROW7), 107);
    expect(r, "records after the landing", t_sj_n, 1);
    for (int i = 0; i < 300; i++) { call(&c, F_PCALL_OK_DEEP); call(&c, F_PCALL_OK); }
    expect(r, "records after 600 pcalls", t_sj_n, 1);
    expect(r, "pushes refused (table full)", (long long)ppu_sj_stat(2), 0);
    expect(r, "pcall(throw 7) after 600 pcalls", call(&c, F_PCALL_THROW7), 107);
    base_checks(r, &c, "stale");
#else
    expect(r, "pcall(throw 7) after a deeper pcall", (call(&c, F_PCALL_OK_DEEP), call(&c, F_PCALL_THROW7)), 107);
    expect(r, "no record table (unpatched lifter)", 0, 1);
#endif
    return r;
}
static Res case_fallback()
{
    Res r; ppu_context c; setup(&c);
    // A jmp_buf no setjmp recorded: its r1 is THROW's own frame, so the old behaviour (longjmp returns into THROW,
    // which pops its frame) leaves r1 as it was. Must not jump anywhere; counted as unmatched.
    const uint64_t buf = 0x9000;
    vm_write64(buf + 0, kStack - 0x70); vm_write64(buf + 8, 0x1234); vm_write64(buf + 16, 0x3131);
    vm_write64(buf + 24, 0x3030); vm_write64(buf + 32, 0x2222);
    vm_write64(G_JB, buf);
#if SJ_PATCHED
    const unsigned long long u0 = ppu_sj_stat(1);
#endif
    call(&c, F_THROW, 3);
    expect(r, "returned with r1", (long long)c.gpr[1], kStack);
    expect(r, "fell through (the old behaviour)", (long long)vm_read64(G_BUG), 1);
#if SJ_PATCHED
    expect(r, "unmatched count", (long long)(ppu_sj_stat(1) - u0), 1);
#else
    expect(r, "unmatched count (unpatched lifter: none)", 0, 1);
#endif
    vm_write64(G_JB, 0); vm_write64(G_BUG, 0);
    return r;
}
static void thread_body(Res* out)
{
    Res& r = *out;
    for (int i = 0; i < 2000 && !r.fails; i++) {
        Res a = case_throw(), b = case_nested(), d = case_deep();
        for (Res* x : {&a, &b, &d}) if (x->fails && !r.fails++) memcpy(r.first, x->first, sizeof r.first);
    }
}
static Res case_threads()
{
    Res a, b, r;
    std::thread t1(thread_body, &a), t2(thread_body, &b);
    t1.join(); t2.join();
    for (Res* x : {&a, &b}) if (x->fails && !r.fails++) memcpy(r.first, x->first, sizeof r.first);
    return r;
}

int main()
{
    struct { const char* name; Res (*fn)(); } cases[] = {
        {"control (no throw)", case_control}, {"throw", case_throw}, {"nested pcalls", case_nested},
        {"deep longjmp (lifted + host frames)", case_deep}, {"stale record dropped by guest r1", case_stale},
        {"two threads", case_threads}, {"no-record fallback", case_fallback}};
    int fails = 0;
    printf("lifter: %s\n", SJ_PATCHED ? "patch 1116" : "UNPATCHED");
    for (auto& k : cases) {
        Res r = k.fn();
        printf("%s %s%s%s\n", r.fails ? "FAIL" : "ok  ", k.name, r.fails ? ": " : "", r.first);
        fails += r.fails != 0;
    }
#if SJ_PATCHED
    printf("landings %llu, unmatched %llu, refused %llu\n", ppu_sj_stat(0), ppu_sj_stat(1), ppu_sj_stat(2));
#endif
    printf("%s (%d of %zu cases failed)\n", fails ? "FAIL" : "PASS", fails, sizeof cases / sizeof cases[0]);
    return fails ? 1 : 0;
}
