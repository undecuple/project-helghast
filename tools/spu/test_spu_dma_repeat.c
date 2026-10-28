/* Patch 1065 host test: the SPU DMA repeat guard is per context and never halts by default.
 * Case "default": 600 identical GETs on context A, interleaved with a different GET on context B every 7th
 * transfer, leave both contexts running (status unchanged) and A's counter at 599 (B's transfers must not reset it).
 * Case "limit" (SPU_DMA_REPEAT_LIMIT=256): A is halted (the armed landing pad is taken) by its 256th repeat, B is not.
 * Built by tools/spu/test_spu_dma_repeat.sh against work/ps3recomp-patched (SKIP without it). */
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c"   /* the unit under test: wrch MFC_Cmd -> mfc_submit (spu_dma.h) */

unsigned long long ps3_ms_now(void) { return 0; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) {}
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

static int g_pass, g_fail;
static const char* g_case = "(none)";
#define CHECK_EQ(actual, expected) do {                                            \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);                   \
    if (_a == _e) g_pass++;                                                         \
    else { g_fail++; fprintf(stderr, "FAIL: %s at %s:%d: %s = 0x%08X, expected 0x%08X\n", \
                             g_case, __FILE__, __LINE__, #actual, _a, _e); }        \
} while (0)

static spu_context g_a, g_b;   /* 256 KB LS each: not on the stack */

static void wrch(spu_context* ctx, uint32_t ch, uint32_t v) { spu_wrch(ctx, ch, spu_make_preferred_u32(v)); }
static void get16(spu_context* ctx, uint32_t lsa, uint32_t ea)
{
    wrch(ctx, MFC_LSA, lsa); wrch(ctx, MFC_EAH, 0); wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, 16); wrch(ctx, MFC_TagID, 3); wrch(ctx, MFC_Cmd, MFC_GET_CMD);
}

static volatile int g_i;   /* survives the halt's longjmp */
static void run(int halt_expected, uint32_t limit)
{
    spu_context* a = &g_a; spu_context* b = &g_b;
    memset(a, 0, sizeof *a); memset(b, 0, sizeof *b);
    spu_context_init(a, 0); spu_context_init(b, 1);
    a->status = SPU_STATUS_RUNNING; b->status = SPU_STATUS_RUNNING;
    a->image_id = 2; b->image_id = 3; a->pc = 0x1CEAC; b->pc = 0x1000;
    /* spu_halt returns to the lifted job's landing pad (setjmp) when one is armed, as spu_run_lifted_job arms it;
     * without a pad it only logs. The test arms its own, so a halt ends the loop like a real job's would. */
    int halted = 0;
    s_spu_halt_armed = 1;
    if (setjmp(s_spu_halt_env) == 0) {
        for (g_i = 0; g_i < 600; g_i++) {
            get16(a, 0x1000, 0x00020000u);                 /* the poll: same EA, same LSA */
            if (g_i % 7 == 0) get16(b, 0x2000, 0x00030000u + 16u * (uint32_t)g_i);   /* another lane's traffic */
        }
    } else halted = 1;
    s_spu_halt_armed = 0;
    CHECK_EQ(halted, halt_expected);
    if (halt_expected) CHECK_EQ((uint32_t)g_i, limit);   /* transfer #limit+1 (index limit) is repeat #limit */
    else CHECK_EQ(mfc_for(a)->rep_n, 599);
    CHECK_EQ(b->status, SPU_STATUS_RUNNING);
    CHECK_EQ(mfc_for(b)->rep_n, 0);                        /* B's transfers all differ: never a repeat */
}

int main(int argc, char** argv)
{
    int limit_case = argc > 1 && strcmp(argv[1], "--limit") == 0;
    if (limit_case) { g_case = "limit"; run(1, 256); }
    else            { g_case = "default"; run(0, 0); }
    printf("%s: pass=%d fail=%d\n", g_case, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
