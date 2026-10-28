/* Focused test of port/spu/guest_fiber.c (pass 13): two guest fibers ping-pong over one shared register block,
 * the way two PPU fibers share one ppu_context on a host thread.
 *   tools/spu/test_guest_fiber.sh   (native build; the Windows backend is exercised by the game runs)
 * Checks: each fiber sees its own registers after every switch (the block is saved and reloaded), the scheduler's
 * registers survive, a body that returns goes back to its creator, and destroy frees suspended fibers.
 * Pass 19: per-fiber locals (guest_fiber_add_local): a job fiber that leaves from inside its "frames" (raises a
 * depth counter, switches home, is destroyed) must not leak its count into the thread (patch 1031). */
#include "../../port/spu/guest_fiber.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint64_t gpr[32]; uint64_t lr; } regs;   /* stand-in for ppu_context */
static regs g_ctx;
static guest_fiber *g_home, *g_a, *g_b;
static int g_fail, g_trace_len;
static char g_trace[64];
static __thread int t_depth;                 /* stand-in for the runtime's thread-local call-depth guard */
static int* depth_slot(void) { return &t_depth; }

/* A job that enters 5 "indirect calls" and finishes by switching home without unwinding them. */
static void leaky_job(void* vctx, void* arg)
{
    (void)vctx; (void)arg;
    if (t_depth != 0) { printf("FAIL: new fiber starts at depth %d\n", t_depth); g_fail++; }
    t_depth += 5;
    guest_fiber_switch(g_home);              /* finish(): never resumed */
}

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void body(void* vctx, void* arg)
{
    regs* r = (regs*)vctx;
    char name = *(const char*)arg;
    guest_fiber* other = name == 'A' ? g_b : g_a;
    CHECK(r->gpr[3] == (uint64_t)name, "%c starts with r3=%llu", name, (unsigned long long)r->gpr[3]);
    for (int i = 0; i < 1000; i++) {
        r->gpr[14] = (uint64_t)name * 100000 + i;            /* callee-saved value that must survive the switch */
        r->gpr[1] = 0x10000000u + (uint64_t)name * 0x1000;   /* own guest stack pointer */
        if (i < 3 && g_trace_len < 60) g_trace[g_trace_len++] = name;
        guest_fiber_switch(other);
        CHECK(r->gpr[14] == (uint64_t)name * 100000 + i, "%c r14 %llu after switch %d", name,
              (unsigned long long)r->gpr[14], i);
        CHECK(r->gpr[1] == 0x10000000u + (uint64_t)name * 0x1000, "%c r1 lost", name);
    }
    r->gpr[3] = name == 'A' ? 0xAAAA : 0xBBBB;
    /* A returns first (B is suspended in its last switch to A): back to the creator (home). */
}

int main(void)
{
    CHECK(guest_fiber_add_local(depth_slot) == 0, "add_local");
    g_home = guest_fiber_self(&g_ctx, sizeof g_ctx);
    g_ctx.gpr[14] = 0x1234; g_ctx.gpr[1] = 0xD0000000u;
    static const char A = 'A', B = 'B';
    g_a = guest_fiber_create(body, (void*)&A, 1 << 20);
    g_b = guest_fiber_create(body, (void*)&B, 1 << 20);
    ((regs*)guest_fiber_state(g_a))->gpr[3] = 'A';
    ((regs*)guest_fiber_state(g_b))->gpr[3] = 'B';
    guest_fiber_switch(g_a);
    /* A finished its 1000 rounds and returned; B is suspended at its last switch. */
    CHECK(guest_fiber_current() == g_home, "not back on home");
    CHECK(g_ctx.gpr[14] == 0x1234 && g_ctx.gpr[1] == 0xD0000000u, "home registers lost: r14=%llx r1=%llx",
          (unsigned long long)g_ctx.gpr[14], (unsigned long long)g_ctx.gpr[1]);
    regs* bs = (regs*)guest_fiber_state(g_b);
    CHECK(bs->gpr[14] == 'B' * 100000ull + 999, "B's saved r14 %llu", (unsigned long long)bs->gpr[14]);
    g_trace[g_trace_len] = 0;
    CHECK(g_trace_len == 6 && g_trace[0] == 'A' && g_trace[1] == 'B' && g_trace[2] == 'A', "order %s", g_trace);
    guest_fiber_destroy(g_a);
    guest_fiber_destroy(g_b);
    /* Many short-lived fibers (one per guest job, as the scheduler creates them). */
    for (int i = 0; i < 2000; i++) {
        static const char C = 'C';
        guest_fiber* f = guest_fiber_create(body, (void*)&C, 64 << 10);
        guest_fiber_destroy(f);
    }
    /* 5000 jobs that each leave 5 levels on their own stack: the home thread's count stays where it was. */
    t_depth = 3;
    for (int i = 0; i < 5000; i++) {
        guest_fiber* f = guest_fiber_create(leaky_job, NULL, 64 << 10);
        guest_fiber_switch(f);
        guest_fiber_destroy(f);
    }
    CHECK(t_depth == 3, "home depth %d after 5000 finished jobs (leak)", t_depth);
    printf("%s: ping-pong 2x1000 switches, order %s, per-fiber depth %d, %d failure(s)\n", g_fail ? "FAIL" : "PASS",
           g_trace, t_depth, g_fail);
    return g_fail != 0;
}
