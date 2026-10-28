/* Patch 1096 host test (SPU pass 71): direct hops between lifted SPU functions (SPU_HOP / SPU_IB_HOP in
 * spu_context.h). Self-contained: the runtime pieces the macros use are stubs here, the "lifted" functions are
 * hand-written in the form spu_lifter.py emits.
 *  1. a 2-function loop (static hop A -> B, computed hop B -> A, the selb loop idiom of the audio mixer jobs),
 *     20 M iterations: with hops on, no transfer reaches the drain loop and the host stack does not grow (the run
 *     would overflow the 8 MB stack otherwise); with hops off, every transfer goes through the drain loop; the
 *     final registers and the step/recent-pc bookkeeping are the same both ways;
 *  2. an enabled pending interrupt, lockstep, launch diagnostics and a stack-reset entry each send the hop to the
 *     trampoline (the drain's hooks must see it);
 *  3. a computed hop to the innermost bracket's return pc (hop_ret_pc) returns with nothing pending (patch 1097), so
 *     the bracket's inline spu_drain_call takes its fast path; with the frame-trace hook armed it is left to the drain;
 *  4. a computed hop the resolver would not answer with a plain lookup (stub returns 0) is left to the drain;
 *  5. lift-time guesses (patch 1098, SPU_IB_HOP_G): the loop's computed hop back to the head is a direct call
 *     without spu_ib_direct, with the same registers and bookkeeping; KZ3_SPU_IBGUESS=0 (g_spu_ib_guess 0), a TU
 *     without a verified image id, another resident overlay or context image, and each of spu_ib_direct's state
 *     tests (IRQ registers saved, pmend ring undecided, trace context, overlay skip of the module the job manager calls, task, resolver-only image,
 *     taskset filter bits, a resident code slot) send it down the old path. Built only when the patched tree has
 *     SPU_IB_HOP_G. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"

/* runtime stubs */
int g_spu_hop_on = 1;
volatile int g_yz_lockstep_on = 0;
int g_spu_launch_diag = 0;
void* volatile g_pm_flow_ctx = 0;
volatile unsigned g_spu_stack_reset_n = 0;
uint32_t g_spu_depth_max = 1000;
void spu_hop_init(void) {}
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t) = 0;
#ifdef SPU_IB_GUESS
int g_spu_ib_guess = 1;
volatile unsigned long long g_spu_ib_guess_n;
int g_kz3_pmend_ib = 0;
void* volatile g_kz3_trace_ctx = 0;
int g_spu_ovl_skip_any = 0;
uint8_t g_spu_ovl_skip[512];
uint64_t g_spu_task_entry_bits[16];
uint64_t g_spu_task_pol_bits[16];
int g_spu_task_pol_big;
static int s_spu_ib_gid = 7; /* the TU's image id (spu_ib_guess_register in the runtime) */
#endif
static int s_full_drains;
void spu_drain_call_full(spu_context* c, uint32_t ret)
{
    (void)c;
    (void)ret;
    s_full_drains++;
}
static void wt_stub(const char* t, int k, uint32_t s, uint64_t a, uint64_t b)
{
    (void)t, (void)k, (void)s, (void)a, (void)b;
}
void spu_task_launch_check_full(spu_context* c, void* fn)
{
    (void)c;
    (void)fn;
}
void spu_check_stack_reset(spu_context* c, void (*f)(spu_context*))
{
    (void)c;
    (void)f;
}
void spu_launch_ls0_end(spu_context* c)
{
    (void)c;
}
void yz_lockstep_tick(spu_context* c)
{
    (void)c;
}

#define A_PC 0x4100u
#define B_PC 0x4200u
#define EXIT_PC 0x4300u
static unsigned long long s_resolves, s_drain_steps;
static int s_ib_answer = 1; /* 0: spu_ib_direct says "not a plain lookup" */
static void fn_a(spu_context* ctx);
static void fn_b(spu_context* ctx);
static void fn_exit(spu_context* ctx)
{
    ctx->gpr[9]._u32[0] = 0xE717u;
}
static spu_hop_fn lookup(uint32_t t)
{
    return t == A_PC ? fn_a : t == B_PC ? fn_b : t == EXIT_PC ? fn_exit : 0;
}
void spu_indirect_branch(spu_context* ctx)
{
    s_resolves++;
    ctx->pc &= SPU_LS_MASK;
    spu_hop_fn f = lookup(ctx->pc);
    if (f) f(ctx);
}
static unsigned long long s_ibd_calls;
spu_hop_fn spu_ib_direct(spu_context* ctx, uint32_t t)
{
    s_ibd_calls++;
    if (!spu_hop_ok(ctx) || !s_ib_answer || t == 0 || t == ctx->hop_ret_pc) return 0;
    return lookup(t);
}
/* A: count down r3, accumulate r4; static hop to B */
static void fn_a(spu_context* ctx)
{
    ctx->gpr[4]._u32[0] += ctx->gpr[3]._u32[0] ^ 0x5Au;
    ctx->gpr[3]._u32[0] -= 1;
    SPU_HOP(ctx, B_PC, fn_b);
}
/* B: r5 = selb(loop head, exit, r3 == 0); bi r5 */
static void fn_b(spu_context* ctx)
{
    ctx->gpr[5]._u32[0] = ctx->gpr[3]._u32[0] ? A_PC : EXIT_PC;
    SPU_IB_HOP(ctx, ctx->gpr[5]._u32[0]);
}
#ifdef SPU_IB_GUESS
/* B with the lifter's guess (patch 1098): the head A */
static void fn_bg(spu_context* ctx);
static void fn_ag(spu_context* ctx)
{
    ctx->gpr[4]._u32[0] += ctx->gpr[3]._u32[0] ^ 0x5Au;
    ctx->gpr[3]._u32[0] -= 1;
    SPU_HOP(ctx, B_PC, fn_bg);
}
static void fn_bg(spu_context* ctx)
{
    ctx->gpr[5]._u32[0] = ctx->gpr[3]._u32[0] ? A_PC : EXIT_PC;
    SPU_IB_HOP_G(ctx, ctx->gpr[5]._u32[0], SPU_IB_GUESS(ctx, A_PC, fn_ag));
}
#endif
static void drain(spu_context* ctx)
{
    while (ctx->tramp_fn) {
        void (*f)(spu_context*) = ctx->tramp_fn;
        ctx->tramp_fn = 0;
        s_drain_steps++;
        spu_task_launch_check(ctx, (void*)f);
        f(ctx);
    }
}
static spu_context* mk(void)
{
    spu_context* c = calloc(1, sizeof *c);
    c->ls = calloc(1, SPU_LS_SIZE);
    return c;
}
static int fails;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fails++;                                                                                                   \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static void run_loop(spu_context* c, uint32_t n)
{
    c->gpr[3]._u32[0] = n;
    c->pc = A_PC;
    fn_a(c);
    drain(c);
}

int main(void)
{
    const uint32_t N = 20000000u;
    /* 1. direct vs trampoline */
    spu_context* on = mk();
    s_resolves = s_drain_steps = 0;
    g_spu_hop_on = 1;
    run_loop(on, N);
    const unsigned long long on_drain = s_drain_steps, on_res = s_resolves;
    spu_context* off = mk();
    s_resolves = s_drain_steps = 0;
    g_spu_hop_on = 0;
    run_loop(off, N);
    CHECK(on->gpr[9]._u32[0] == 0xE717u && off->gpr[9]._u32[0] == 0xE717u, "loop did not reach the exit");
    CHECK(on->gpr[4]._u32[0] == off->gpr[4]._u32[0], "r4 differs: %08X vs %08X", on->gpr[4]._u32[0],
          off->gpr[4]._u32[0]);
    CHECK(on_drain == 0 && on_res == 0, "hops on: %llu drain steps, %llu resolves (want 0)", on_drain, on_res);
    CHECK(s_drain_steps == 2ull * N && s_resolves == N, "hops off: %llu drain steps, %llu resolves", s_drain_steps,
          s_resolves);
    CHECK(on->steps == off->steps, "step count differs: %u vs %u", on->steps, off->steps);
    CHECK(on->recent_n == off->recent_n && !memcmp(on->recent_pc, off->recent_pc, sizeof on->recent_pc),
          "recent-pc ring differs");
    printf("loop of %u: hops on 0 drain steps; off %llu drain steps, %llu resolves; r4 %08X both\n", N, s_drain_steps,
           s_resolves, on->gpr[4]._u32[0]);

    /* 2. conditions that need the drain's hooks: the first hop goes to the trampoline */
    g_spu_hop_on = 1;
    for (int k = 0; k < 4; k++) {
        spu_context* c = mk();
        if (k == 0) {
            c->int_enable = 1;
            c->event_mask = 0x2;
            c->event_status = 0x2;
        }
        g_yz_lockstep_on = k == 1;
        g_spu_launch_diag = k == 2;
        g_spu_stack_reset_n = k == 3;
        c->host_depth = 1;
        c->gpr[3]._u32[0] = 5;
        c->pc = A_PC;
        fn_a(c);
        CHECK(c->tramp_fn == fn_b && c->pc == B_PC && c->gpr[3]._u32[0] == 4, "case %d: hop not left to the drain", k);
    }
    g_yz_lockstep_on = 0;
    g_spu_launch_diag = 0;
    g_spu_stack_reset_n = 0;

    /* 3. computed hop to the bracket's return pc */
    spu_context* r = mk();
    r->hop_ret_pc = A_PC;
    r->gpr[3]._u32[0] = 3;
    r->host_depth = 1;
    fn_b(r);
    CHECK(r->tramp_fn == 0 && r->pc == A_PC, "hop to hop_ret_pc did not return with nothing pending");
    spu_drain_call(r, A_PC); /* the bracket's drain after the call: fast path, no full drain */
    CHECK(s_full_drains == 0 && r->drain_ret_pc == 0, "bracket drain after a return hop took the full path");
    spu_context* w = mk(); /* frame-trace hook armed: left to the drain (spu_drain_call_full closes the bracket) */
    w->hop_ret_pc = A_PC;
    w->gpr[3]._u32[0] = 3;
    g_kz3_wtrace = wt_stub;
    fn_b(w);
    g_kz3_wtrace = 0;
    CHECK(w->tramp_fn == spu_indirect_branch && w->pc == A_PC,
          "hop to hop_ret_pc with the trace hook not left to the drain");

    /* 4. resolver would not answer with a plain lookup */
    spu_context* q = mk();
    s_ib_answer = 0;
    q->gpr[3]._u32[0] = 3;
    fn_b(q);
    CHECK(q->tramp_fn == spu_indirect_branch && q->pc == A_PC, "unanswered computed hop not left to the drain");
    s_ib_answer = 1;

#ifdef SPU_IB_GUESS
    /* 5. lift-time guesses. The stub resolver answers fn_a for A_PC: a guess hop runs fn_ag instead, so the
     * loop-back hops counted below are told apart by spu_ib_direct calls and g_spu_ib_guess_n. */
    {
        spu_context* gon = mk();
        gon->resident_ovl = 7;
        g_spu_ib_guess = 2;
        g_spu_ib_guess_n = 0;
        s_resolves = s_drain_steps = s_ibd_calls = 0;
        gon->gpr[3]._u32[0] = N;
        gon->pc = A_PC;
        fn_ag(gon);
        drain(gon);
        CHECK(gon->gpr[9]._u32[0] == 0xE717u && gon->gpr[4]._u32[0] == off->gpr[4]._u32[0], "guess loop: wrong result");
        CHECK(s_drain_steps == 0 && s_resolves == 0 && s_ibd_calls == 1 && g_spu_ib_guess_n == N - 1,
              "guess loop: %llu drain steps, %llu resolves, %llu spu_ib_direct calls, %llu guesses (want 0 0 1 %u)",
              s_drain_steps, s_resolves, s_ibd_calls, g_spu_ib_guess_n, N - 1);
        CHECK(gon->steps == off->steps && gon->recent_n == off->recent_n &&
                  !memcmp(gon->recent_pc, off->recent_pc, sizeof gon->recent_pc),
              "guess loop: bookkeeping differs from the trampoline path");
        printf("guess loop of %u: %llu guess hops, %llu spu_ib_direct calls\n", N, g_spu_ib_guess_n, s_ibd_calls);
        /* each case: one loop-back hop must take the old path (the stub resolver, fn_a) */
        for (int k = 0; k < 16; k++) {
            spu_context* c = mk();
            c->resident_ovl = 7;
            g_spu_ib_guess = k == 0 ? 0 : 2;
            if (k == 1) s_spu_ib_gid = 0;
            if (k == 2) c->resident_ovl = 8;
            if (k == 3) {
                c->resident_ovl = 0;
                c->image_id = 8;
            }
            if (k == 4) c->irq_saved = 1;
            if (k == 5) g_kz3_pmend_ib = -1;
            if (k == 6) g_kz3_trace_ctx = c;
            if (k == 7) { /* the job manager's call of a marked module (image 2, return link below 0x4000) */
                g_spu_ovl_skip_any = 1;
                g_spu_ovl_skip[7] = 1;
                c->image_id = 2;
                c->gpr[0]._u32[0] = 0x1000;
            }
            if (k == 8) c->resident_task = 1;
            if (k == 9) {
                c->resident_ovl = 0;
                c->image_id = SPU_RESOLVER_ONLY_IMAGE_ID;
                s_spu_ib_gid = SPU_RESOLVER_ONLY_IMAGE_ID;
            }
            if (k == 10) g_spu_task_pol_bits[0] = 1ull << 7;
            if (k == 11) g_spu_task_entry_bits[(A_PC >> 2) & 15] = 1ull << ((A_PC >> 6) & 63);
            if (k == 12) {
                c->resident_code[1].image_id = 3;
                c->resident_code[1].lsa = 0x4000;
                c->resident_code[1].size = 0x200;
            }
            if (k == 13) g_spu_task_pol_big = 1;
            if (k == 15) {
                g_spu_ovl_skip_any = 1;
                g_spu_ovl_skip[9] = 1;
            }                             /* another module marked: guess runs */
            const int positive = k >= 14; /* controls: the guess runs */
            g_spu_ib_guess_n = 0;
            s_ibd_calls = 0;
            c->gpr[3]._u32[0] = 1;
            c->pc = B_PC;
            fn_bg(c); /* r3 = 1: hop to A (guess or old path), A: r3 = 0, hop B, B: exit via spu_ib_direct */
            CHECK(c->gpr[9]._u32[0] == 0xE717u && c->gpr[3]._u32[0] == 0, "guard %d: wrong result", k);
            CHECK(positive ? (g_spu_ib_guess_n == 1 && s_ibd_calls == 1)
                           : (g_spu_ib_guess_n == 0 && s_ibd_calls == 2 && c->gpr[4]._u32[0] == (1u ^ 0x5Au)),
                  "guard %d: %llu guess hops, %llu spu_ib_direct calls", k, g_spu_ib_guess_n, s_ibd_calls);
            s_spu_ib_gid = 7;
            g_kz3_pmend_ib = 0;
            g_kz3_trace_ctx = 0;
            g_spu_ovl_skip_any = 0;
            memset(g_spu_ovl_skip, 0, sizeof g_spu_ovl_skip);
            g_spu_task_pol_bits[0] = 0;
            g_spu_task_pol_big = 0;
            memset(g_spu_task_entry_bits, 0, sizeof g_spu_task_entry_bits);
        }
        g_spu_ib_guess = 1;
    }
#else
    printf("SKIP case 5: no SPU_IB_GUESS (patch 1098 not applied)\n");
#endif

    if (fails) {
        printf("FAIL (%d)\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
