/* MFC engine host test (patch 1083's until v0.15.0; now checks upstream PR #290's slot list): every SPU context keeps its own MFC engine past 64 live contexts (SPU pass 65: with five
 * real SPUs per instance the KZ3 levels passed 64, the rest shared one fallback engine, a peer's DMA reset a waiting
 * context's tag bit, and the jm policy module spun forever in its tag wait).
 * 1. 300 contexts each issue a GET on tag 5: 300 distinct engines, no shared fallback, every tag 5 complete.
 * 2. The last context's tag 5 is held incomplete (as a list parked at a stall-and-notify element holds it); 299 GETs on tag 5
 *    from the other contexts leave it incomplete (with a shared engine the first one completed it).
 * 3. Release 100 contexts, claim 100 new ones: the slots are reused (the high-water mark stays at 300), the engines
 *    of the 200 that stayed are unchanged.
 * Built by tools/spu/test_mfc_slots.sh against work/ps3recomp-patched (SKIP without it). */
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
#include "spu_channels.c" /* the unit under test: wrch MFC_Cmd -> mfc_submit (spu_dma.h) */

unsigned long long ps3_ms_now(void)
{
    return 0;
}
void ppu_resv_break_store(uint64_t ea)
{
    (void)ea;
}
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a;
    (void)v;
    (void)w;
}
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits)
{
    (void)ea;
    (void)bits;
}
void ydkj_wake_all_event_flags(void) {}
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c;
    (void)a;
    (void)b;
    (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx)
{
    (void)tid;
    (void)ctx;
}

static int g_pass, g_fail;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (cond)                                                                                                      \
            g_pass++;                                                                                                  \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)
#define N 300
static spu_context* g_c[N + 100];
static void wrch(spu_context* ctx, uint32_t ch, uint32_t v)
{
    spu_wrch(ctx, ch, spu_make_preferred_u32(v));
}
static void get16(spu_context* ctx, uint32_t tag)
{
    wrch(ctx, MFC_LSA, 0x1000);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, 0x00020000u);
    wrch(ctx, MFC_Size, 16);
    wrch(ctx, MFC_TagID, tag);
    wrch(ctx, MFC_Cmd, MFC_GET_CMD);
}
static spu_context* mk(int i)
{
    spu_context* c = (spu_context*)calloc(1, sizeof *c); /* 256 KB LS each: heap */
    if (!c) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    spu_context_init(c, (uint32_t)(i % 6));
    c->status = SPU_STATUS_RUNNING;
    c->image_id = 2;
    return c;
}
int main(void)
{
    for (int i = 0; i < N; i++) {
        g_c[i] = mk(i);
        get16(g_c[i], 5);
    }
    int distinct = 1;
    for (int i = 0; i < N && distinct; i++)
        for (int j = 0; j < i; j++)
            if (mfc_for(g_c[i]) == mfc_for(g_c[j])) {
                distinct = 0;
                break;
            }
    CHECK(distinct, "%d contexts do not each have their own MFC engine", N);
    int done = 0;
    for (int i = 0; i < N; i++) done += (mfc_for(g_c[i])->tag_completed >> 5) & 1;
    CHECK(done == N, "tag 5 complete on %d of %d contexts", done, N);
    /* 2: hold context 0's tag 5 incomplete, traffic on every other context */
    mfc_for(g_c[N - 1])->tag_completed &= ~(1u << 5);
    for (int i = 0; i < N - 1; i++) get16(g_c[i], 5);
    CHECK(((mfc_for(g_c[N - 1])->tag_completed >> 5) & 1) == 0, "a peer's DMA completed the last context's held tag 5");
    /* 3: release pass 100, claim 100 new */
    mfc_engine* keep[N];
    for (int i = 0; i < N; i++) keep[i] = mfc_for(g_c[i]);
    for (int i = 100; i < 200; i++) spu_mfc_release(g_c[i]);
    for (int i = N; i < N + 100; i++) {
        g_c[i] = mk(i);
        get16(g_c[i], 5);
    }
    int same = 1, reused = 1;
    for (int i = 0; i < N; i++)
        if ((i < 100 || i >= 200) && mfc_for(g_c[i]) != keep[i]) same = 0;
    for (int i = N; i < N + 100; i++) {
        int hit = 0;
        for (int k = 100; k < 200; k++)
            if (mfc_for(g_c[i]) == keep[k]) hit = 1;
        if (!hit) reused = 0;
    }
    CHECK(same, "a kept context's engine changed after the release");
    CHECK(reused, "the 100 new contexts did not reuse the released slots");
    printf("test_mfc_slots: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
