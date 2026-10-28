/* Patch 1110 host test: the last check for SPU_EVENT_LR before a reservation is replaced or dropped
 * (RPCS3 PR #5435's GETLLAR ordering), and the KZ3_LR_SNAP stall snapshot. Through the real channel
 * path (spu_wrch -> spu_mfc_atomic), as test_lockline_stress.c does.
 *
 * A store that bypasses spu_coh_notify_write* (here a plain write into guest memory) used to be seen
 * only by the rdch/rchcnt SPU_RdEventStat compare. GETLLAR and PUTLLC ended the reservation without
 * that compare, so the event was never raised:
 *   1. GETLLAR A, store to A, GETLLAR B   -> LR pending
 *   2. GETLLAR A, store to A, GETLLAR A   -> LR pending
 *   3. GETLLAR A, store to A, PUTLLC A    -> PUTLLC fails, LR pending
 *   4. GETLLAR A, store to A, PUTLLC B    -> LR pending
 *   5. no store (GETLLAR A, GETLLAR B; GETLLAR A, PUTLLC A) -> no LR
 *   6. a notified store, then GETLLAR B   -> LR raised once (no double count)
 *   7. KZ3_LR_SNAP=1 snapshot: one line per registered SPU with the right class
 * Exit 0 = pass. With KZ3_LR_LASTCHECK=0 (the old path) cases 1-4 fail: test_lr_lastcheck.sh checks that. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t g_guest_mem[1u << 20] __attribute__((aligned(4096)));
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c" /* via -I runtime/spu */

#define LINE_A 0x00030000u
#define LINE_B 0x00030800u
#define LSA 0x1000u

static spu_context g_ctx[2];
static int g_fail;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_fail++;                                                                                                  \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static void wrch(spu_context* ctx, uint32_t ch, uint32_t v)
{
    spu_wrch(ctx, ch, spu_make_preferred_u32(v));
}
static void atomic_cmd(spu_context* ctx, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, LSA);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}
static int lr_pending(spu_context* c)
{
    return (spu_event_load(c) & SPU_EVENT_LR) != 0;
}
static void reset(spu_context* c)
{
    wrch(c, SPU_WrEventAck, SPU_EVENT_LR);
    c->resv_valid = 0;
}
/* A store nobody announces: an HLE memcpy into guest memory. */
static void quiet_store(uint32_t ea, uint8_t v)
{
    g_guest_mem[ea + 5] = v;
}
static void notified_store(uint32_t ea, uint8_t v)
{
    const uint64_t m = spu_ll_mask(ea, 128);
    spu_ll_lock_mask(m);
    g_guest_mem[ea + 9] = v;
    spu_coh_notify_write(ea);
    spu_ll_unlock_mask(m);
}

int main(void)
{
    for (int i = 0; i < 2; i++) {
        spu_context_init(&g_ctx[i], 0);
        g_ctx[i].spu_id = (uint32_t)i;
    }
    spu_context* a = &g_ctx[0];
    printf("last check: %s\n", getenv("KZ3_LR_LASTCHECK") ? getenv("KZ3_LR_LASTCHECK") : "default (on)");

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); quiet_store(LINE_A, 1); atomic_cmd(a, LINE_B, MFC_GETLLAR_CMD);
    CHECK(lr_pending(a), "1: GETLLAR on another line dropped a lost reservation without SPU_EVENT_LR");
    CHECK(a->lr_last_src == SPU_LR_SRC_LASTCHECK, "1: lr_last_src=%u, want %u", a->lr_last_src, SPU_LR_SRC_LASTCHECK);

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); quiet_store(LINE_A, 2); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD);
    CHECK(lr_pending(a), "2: GETLLAR on the same line overwrote the snapshot without SPU_EVENT_LR");

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); quiet_store(LINE_A, 3); atomic_cmd(a, LINE_A, MFC_PUTLLC_CMD);
    CHECK(a->atomic_stat == 1, "3: PUTLLC over a changed line succeeded");
    CHECK(lr_pending(a), "3: failed PUTLLC consumed a lost reservation without SPU_EVENT_LR");

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); quiet_store(LINE_A, 4); atomic_cmd(a, LINE_B, MFC_PUTLLC_CMD);
    CHECK(lr_pending(a), "4: PUTLLC on another line dropped a lost reservation without SPU_EVENT_LR");

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); atomic_cmd(a, LINE_B, MFC_GETLLAR_CMD);
    CHECK(!lr_pending(a), "5: SPU_EVENT_LR raised with no store (GETLLAR A, GETLLAR B)");
    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); atomic_cmd(a, LINE_A, MFC_PUTLLC_CMD);
    CHECK(a->atomic_stat == 0 && !lr_pending(a), "5: GETLLAR A, PUTLLC A: stat=%u lr=%d", a->atomic_stat, lr_pending(a));

    reset(a); atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD);
    const uint32_t n0 = a->lr_raise_n;
    notified_store(LINE_A, 6); atomic_cmd(a, LINE_B, MFC_GETLLAR_CMD);
    CHECK(lr_pending(a) && a->lr_raise_n == n0 + 1 && a->lr_last_src == SPU_LR_SRC_NOTIFY,
          "6: notified store: lr=%d raises=%u (want %u) src=%u", lr_pending(a), a->lr_raise_n, n0 + 1, a->lr_last_src);

    /* 7: the snapshot. a: LR pending and enabled; b: valid reservation on an untouched line. */
    spu_context* b = &g_ctx[1];
    reset(a); reset(b);
    atomic_cmd(a, LINE_A, MFC_GETLLAR_CMD); wrch(a, SPU_WrEventMask, SPU_EVENT_LR); notified_store(LINE_A, 7);
    atomic_cmd(b, LINE_B, MFC_GETLLAR_CMD); wrch(b, SPU_WrEventMask, SPU_EVENT_LR);
    setenv("KZ3_LR_SNAP", "1", 1);
    fflush(stderr);
    FILE* tf = tmpfile();
    int saved = dup(fileno(stderr));
    dup2(fileno(tf), fileno(stderr));
    CHECK(spu_lr_snap_on(), "7: KZ3_LR_SNAP=1 not seen");
    spu_lr_snapshot("test", a);
    fflush(stderr);
    dup2(saved, fileno(stderr));
    char buf[8192] = {0};
    rewind(tf);
    size_t got = fread(buf, 1, sizeof buf - 1, tf);
    (void)got;
    CHECK(strstr(buf, "[lr-snap] why=test spu=0") != NULL, "7: no snapshot header");
    CHECK(strstr(buf, "spu=0 img=") && strstr(buf, "class=not-advanced"), "7: SPU 0 not classed not-advanced");
    CHECK(strstr(buf, "spu=1 img=") && strstr(buf, "class=never-written"), "7: SPU 1 not classed never-written");
    CHECK(strstr(buf, "line 0x00030800:") != NULL, "7: no line words for SPU 1's reservation");

    printf("lr last check: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}

/* The runtime beyond the SPU channel layer, stubbed (as in test_lockline_stress.c). */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) {}
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
