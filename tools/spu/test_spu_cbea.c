/*
 * KZ3 patch 1061 host test: three CBEA v1.01 SPU channel semantics in runtime/spu/spu_channels.c
 * (SPU pass 47, CBEA audit items 1, 2, 5). Each case is a negative control for its
 * off switch: run with the switch set, the case must FAIL (tools/spu/test_spu_cbea.sh checks both).
 *
 *   evstat   rdch SPU_RdEventStat returns pending AND the write event mask [CBEA s.9.11, s.9.11.1],
 *            so a reader that acks what it read (KZ3's job manager and lock waits) leaves a masked
 *            pending bit in place. Off: KZ3_SPU_EVSTAT_UNMASKED=1.
 *   machstat with --ie-latch: rdch SPU_RdMachStat bit 31 is the interrupt-enable latch [CBEA s.9.8]
 *            (opt-in KZ3_SPU_MACHSTAT_IE=1; upstream #289 is the same fix); without it the port's
 *            default "1 while running" (SPU pass 47: the latch hangs KZ3's level load).
 *   putlluc  PUTLLUC kills the peers' reservations with Lr but not the issuer's own with Lr: a local
 *            action never sets Lr [CBEA s.9.11.1 bit 21]. Off: KZ3_SPU_PUTLLUC_SELF_LR=1.
 *
 * Built like upstream's test_spu_lockline_peer.c (spu_channels.c compiled in: spu_mfc_atomic is
 * file-static), see tools/spu/test_spu_cbea.sh. Exit 0 = all passed, 1 = a case failed; the FAIL
 * lines name the case.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c"   /* the unit under test (-I runtime/spu) */


/* Runtime globals the linked SPU runtime references (same stubs as tools/spu/test_lockline_stress.c). */
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

#define LINE_A 0x00020000u
#define LSA_A  0x00001000u
#define LSA_B  0x00002000u
static spu_context g_a, g_b;   /* 256 KB LS each: not on the stack */

static void wrch(spu_context* ctx, uint32_t ch, uint32_t v) { spu_wrch(ctx, ch, spu_make_preferred_u32(v)); }
static uint32_t rdch(spu_context* ctx, uint32_t ch) { return spu_rdch(ctx, ch)._u32[0];   /* preferred slot */ }
static void atomic_cmd(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa); wrch(ctx, MFC_EAH, 0); wrch(ctx, MFC_EAL, ea); wrch(ctx, MFC_Cmd, cmd);
}

static void test_evstat(void)
{
    g_case = "evstat";
    spu_context* c = &g_a;
    c->status = SPU_STATUS_RUNNING;
    c->event_mask = 0; c->event_status = 0;
    /* The KZ3 lock wait: mask 0x400 (Lr) only, while SN (0x2) and Tg (0x1) are pending but masked. */
    spu_event_raise(c, SPU_EVENT_LR | 0x2u | 0x1u);
    wrch(c, SPU_WrEventMask, SPU_EVENT_LR);
    uint32_t v = rdch(c, SPU_RdEventStat);
    CHECK_EQ(v, SPU_EVENT_LR);                        /* only the enabled event [CBEA s.9.11.1] */
    wrch(c, SPU_WrEventAck, v);                        /* the reader acks exactly what it read */
    CHECK_EQ(spu_event_load(c) & 0x3u, 0x3u);          /* SN and Tg still pending [CBEA s.9.11.4] */
    CHECK_EQ(spu_event_load(c) & SPU_EVENT_LR, 0);
    /* Restoring the saved mask makes the pending SN visible again. */
    wrch(c, SPU_WrEventMask, 0x2u);
    CHECK_EQ(rdch(c, SPU_RdEventStat), 0x2u);
    /* Mask 0 reads 0 whatever is pending. */
    wrch(c, SPU_WrEventMask, 0);
    CHECK_EQ(rdch(c, SPU_RdEventStat), 0);
    wrch(c, SPU_WrEventAck, 0x3u); c->event_mask = 0;
}

static int g_ie_latch;   /* --ie-latch: assert the CBEA s.9.8 semantics (needs KZ3_SPU_MACHSTAT_IE=1) */
static void test_machstat(void)
{
    g_case = "machstat";
    spu_context* c = &g_a;
    c->status = SPU_STATUS_RUNNING;
    if (g_ie_latch) {
        c->int_enable = 0;
        CHECK_EQ(rdch(c, SPU_RdMachStat) & 1u, 0);     /* IE off: bit 31 = 0 [CBEA s.9.8] */
        c->int_enable = 1;
        CHECK_EQ(rdch(c, SPU_RdMachStat) & 1u, 1);
        c->int_enable = 0;
        CHECK_EQ(rdch(c, SPU_RdMachStat) & 1u, 0);
    } else {                                           /* the port's default: 1 while running */
        c->int_enable = 0;
        CHECK_EQ(rdch(c, SPU_RdMachStat) & 1u, 1);
        c->status = SPU_STATUS_STOPPED;
        CHECK_EQ(rdch(c, SPU_RdMachStat) & 1u, 0);
        c->status = SPU_STATUS_RUNNING;
    }
}

static void test_putlluc(void)
{
    g_case = "putlluc";
    g_a.status = g_b.status = SPU_STATUS_RUNNING;
    g_a.event_mask = g_b.event_mask = SPU_EVENT_LR;
    g_a.event_status = g_b.event_status = 0;
    memset(g_guest_mem + LINE_A, 0x11, 128);
    atomic_cmd(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    atomic_cmd(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    CHECK_EQ(g_a.resv_valid, 1); CHECK_EQ(g_b.resv_valid, 1);
    memset(g_a.ls + LSA_A, 0x22, 128);
    atomic_cmd(&g_a, LSA_A, LINE_A, MFC_PUTLLUC_CMD);  /* lock release on the line both reserved */
    CHECK_EQ(g_guest_mem[LINE_A], 0x22);               /* the store landed */
    CHECK_EQ(g_a.resv_valid, 0);                        /* own reservation gone... */
    CHECK_EQ(spu_event_load(&g_a) & SPU_EVENT_LR, 0);  /* ...silently: local action [s.9.11.1 bit 21] */
    CHECK_EQ(g_b.resv_valid, 0);                        /* the peer's gone with Lr */
    CHECK_EQ(spu_event_load(&g_b) & SPU_EVENT_LR, SPU_EVENT_LR);
    /* And the peer's PUTLLC then fails (no reservation), the issuer keeps the line. */
    memset(g_b.ls + LSA_B, 0x33, 128);
    atomic_cmd(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ(rdch(&g_b, MFC_RdAtomicStat) & 1u, 1);    /* S: PUTLLC failed */
    CHECK_EQ(g_guest_mem[LINE_A], 0x22);
}

int main(int argc, char** argv)
{
    g_ie_latch = (argc > 1 && strcmp(argv[1], "--ie-latch") == 0);
    spu_context_init(&g_a, 0); spu_context_init(&g_b, 1);
    test_evstat();
    test_machstat();
    test_putlluc();
    printf("test_spu_cbea: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
