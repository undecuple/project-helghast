/* Patch 1104 host test (upstream ps3recomp PR #266 fixture, trimmed): hand-encoded guest bytes force the full
 * interpreter (ori is outside the SMC microstep subset), then reach or call lifted functions. Real channel runtime,
 * interpreter, function registry and drain driver; no game data. Modes: top, top-zero, nested-calls (fail without the
 * patch), nested-return (owning drain keeps its own continuation), pure-interpreter, stepcap (unchanged). */
#include "spu_helpers.h"
#include "spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);
static spu_context ctx;
static void word(uint32_t pc, uint32_t w) {
    ctx.ls[pc] = w >> 24; ctx.ls[pc+1] = w >> 16; ctx.ls[pc+2] = w >> 8; ctx.ls[pc+3] = w;
}
/* The SPU_RET macro names the context `ctx`, so the pointer parameters are called that (shadowing the global). */
static void finish(spu_context* ctx) {
    ctx->gpr[11]._u32[0]++;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(ctx);
}
static void helper(spu_context* ctx) { ctx->gpr[10] = spu_ai(ctx->gpr[10], 1); SPU_RET(ctx); }
static void wrong_continuation(spu_context* ctx) { ctx->gpr[21]._u32[0]++; finish(ctx); }
static void tail_entry(spu_context* ctx) { ctx->pc = 0x4000; g_spu_trampoline_fn = spu_indirect_branch; }
static int queued_call;
static void nested_entry(spu_context* ctx) {
    ctx->gpr[0] = spu_link(0x204);
    ctx->host_depth++;
    ctx->pc = 0x4000;
    if (queued_call) g_spu_trampoline_fn = spu_indirect_branch;
    else spu_indirect_branch(ctx);
    spu_drain_call(ctx, 0x204);
    ctx->host_depth--;
    ctx->gpr[20]._u32[0]++;
    finish(ctx);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* mode = argv[1];
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.policy_mode = 0; /* policy_mode 1 + LS 0x4000..0x3FE80 takes the 1069 path before the fallback */
    ctx.status = SPU_STATUS_RUNNING;
    spu_begin_image(77);
    spu_register_function(0x200, helper);
    spu_register_function(0x204, wrong_continuation);
    spu_register_function(0x240, finish);
    if (!strcmp(mode, "top-zero")) spu_register_function(0, finish);
    word(0x4000, 0x04000082); /* ori $2,$1,0: force the full interpreter */
    void (*entry)(spu_context*) = tail_entry;
    if (!strcmp(mode, "top-zero")) {
        word(0x4004, 0x30000000); /* bra 0: registered lifted entry */
    } else if (!strcmp(mode, "top")) {
        word(0x4004, 0x30004800); /* bra 0x240 */
    } else if (!strcmp(mode, "nested-return")) {
        entry = nested_entry; queued_call = 1;
        word(0x204, 0x1C004A95); /* ai $21,$21,1: must NOT be interpreted */
        word(0x208, 0x30004800); /* bra 0x240 */
        word(0x4004, 0x35000000); /* bi $0 -> 0x204 */
    } else if (!strcmp(mode, "nested-calls")) {
        entry = nested_entry;
        word(0x4004, 0x31004000); /* brasl $0,0x200 */
        word(0x4008, 0x04000082); /* ori $2,$1,0 */
        word(0x400C, 0x31004000); /* brasl $0,0x200 */
        word(0x4010, 0x04000082); /* ori $2,$1,0 */
        word(0x4014, 0x30004800); /* bra 0x240 */
    } else if (!strcmp(mode, "stepcap")) {
        word(0x4004, 0x04000082);
        word(0x4008, 0x30004800);
    } else if (!strcmp(mode, "pure-interpreter")) {
        word(0x4004, 0x30004800);
        uint32_t pc = spu_interp_run_until(&ctx, 0x4000, 0);
        assert(pc == 0x240 && ctx.status == SPU_STATUS_RUNNING);
        assert(ctx.tramp_fn == NULL);
        puts("pure-interpreter PASS"); return 0;
    } else { fprintf(stderr, "unknown mode\n"); return 2; }
    int halted = spu_run_with_halt(entry, &ctx);
    printf("%s: halted=%d status=%X pc=%X helper=%u finish=%u host-cont=%u wrong-cont=%u\n",
           mode, halted, ctx.status, ctx.pc, ctx.gpr[10]._u32[0], ctx.gpr[11]._u32[0],
           ctx.gpr[20]._u32[0], ctx.gpr[21]._u32[0]);
    fflush(stdout);
    if (!strcmp(mode, "stepcap")) {
        assert(halted == 0 && ctx.status == SPU_STATUS_RUNNING);
        assert(ctx.pc == 0x4004 && ctx.gpr[11]._u32[0] == 0);
        assert(ctx.tramp_fn == NULL);
        puts("PASS"); return 0;
    }
    assert(ctx.gpr[21]._u32[0] == 0);
    assert(halted == 1 && ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    assert(ctx.gpr[11]._u32[0] == 1);
    assert(ctx.gpr[20]._u32[0] == (!strcmp(mode, "nested-return") ? 1u : 0u));
    assert(ctx.gpr[10]._u32[0] == (!strcmp(mode, "nested-calls") ? 2u : 0u));
    puts("PASS"); return 0;
}

/* Link-only boundaries outside this CPU-only fixture. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; abort(); }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
