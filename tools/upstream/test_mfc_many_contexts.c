/* Focused upstream test for patch 1013 (SPU MFC engine table too small).
 *
 * KZ3 holds 11+ SPU contexts at once (policy-module contexts that keep their slot, plus SPU thread groups).
 * ps3recomp v0.13.0 has SPU_MAX_CONTEXTS = 8 engines; every context past the 8th shares one fallback engine, so
 * one context's DMA bookkeeping overwrites another's and a tag wait never completes. Upstream's own
 * test_spu_mfc_slots.c only ever fills exactly SPU_MAX_CONTEXTS, so it passes on v0.13.0 too.
 * v0.15.0 absorbed 1013 (PR #290, per-context slots without a limit): test_mfc_many_contexts.sh now wants PASS on
 * upstream and on the full stack.
 *
 * This test holds NCTX = 11 contexts at once, one per thread, and checks:
 *   - every context gets its own engine (no two share one);
 *   - a value each context writes into its engine's tag/queue fields is still there after all have written.
 * Expected: FAIL on v0.13.0, PASS with 1013.
 *
 * spu_channels.c is compiled in (the table and mfc_for() are file-static), as upstream's test does.
 * Build/run: tools/upstream/test_mfc_many_contexts.sh
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c"

/* Link stubs: the same set upstream's test_spu_mfc_slots.c defines; none is on the path this test drives. */
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

#define NCTX 11

static spu_context* g_ctx[NCTX];
static mfc_engine*  g_engine[NCTX];
static int          g_private[NCTX];
static pthread_barrier_t g_bar;

static void* worker(void* arg)
{
    int w = (int)(intptr_t)arg;
    pthread_barrier_wait(&g_bar);               /* all claim while all are held */
    mfc_engine* e = mfc_for(g_ctx[w]);
    g_engine[w] = e;
    uint32_t mark = 0x5A000000u | (uint32_t)w;
    e->tag_completed = mark;
    e->queue_count   = (uint32_t)w + 1u;
    pthread_barrier_wait(&g_bar);               /* everyone has written */
    g_private[w] = (e->tag_completed == mark) && (e->queue_count == (uint32_t)w + 1u);
    pthread_barrier_wait(&g_bar);               /* everyone has read */
    spu_mfc_release(g_ctx[w]);
    return NULL;
}

int main(void)
{
    int fail = 0;
    pthread_t th[NCTX];
    pthread_barrier_init(&g_bar, NULL, NCTX);
    for (int w = 0; w < NCTX; w++) {
        g_ctx[w] = (spu_context*)calloc(1, sizeof(spu_context));
        if (!g_ctx[w]) { printf("FAIL: out of memory\n"); return 1; }
    }
    for (int w = 0; w < NCTX; w++) pthread_create(&th[w], NULL, worker, (void*)(intptr_t)w);
    for (int w = 0; w < NCTX; w++) pthread_join(th[w], NULL);

    int shared = 0, clobbered = 0;
    for (int w = 0; w < NCTX; w++) {
        for (int v = w + 1; v < NCTX; v++)
            if (g_engine[w] == g_engine[v]) shared++;
        if (!g_private[w]) clobbered++;
    }
#ifndef SPU_MAX_CONTEXTS
#define SPU_MAX_CONTEXTS 0 /* v0.15.0 (PR #290): no fixed table any more; 0 = unbounded */
#endif
    printf("SPU_MAX_CONTEXTS=%d, contexts held at once=%d: engine pairs shared=%d, contexts whose writes were "
           "overwritten=%d\n", (int)SPU_MAX_CONTEXTS, NCTX, shared, clobbered);
    if (shared || clobbered) fail = 1;
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
