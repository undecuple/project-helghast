/* Pass 143 candidate patch r143b (KZ3_PUTLLC_FAST): semantics and lock avoidance of PUTLLC, single guest, helper thread.
 * Run with the switch unset and set to 1; the expectations differ only in what is counted and what takes the stripe.
 *   1. PUTLLC with no reservation fails, memory untouched; with the switch it neither counts nor takes the stripe.
 *   2. GETLLAR, then a PPU-side store to the line: PUTLLC fails, the PPU value stays; with the switch it does not
 *      take the stripe (the line differs from the snapshot).
 *   3. GETLLAR, PUTLLC unchanged: succeeds, memory = local store, takes the stripe in both modes.
 *   4. GETLLAR on line A, PUTLLC on line B: fails and the reservation on A is gone.
 * "Takes the stripe" is observed by holding every stripe from the main thread while a helper thread issues the PUTLLC:
 * a PUTLLC that needs the stripe cannot finish until it is released.
 * Exit 0 = pass. Build like tools/spu/test_lockline_stress.c (spu_channels.c included, -I runtime/spu). */
#include <pthread.h>
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
#define LINE_B 0x00030400u
#define LSA 0x1000u

static spu_context g_ctx;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void wrch(spu_context* ctx, uint32_t ch, uint32_t v) { spu_wrch(ctx, ch, spu_make_preferred_u32(v)); }
static void atomic_cmd(spu_context* ctx, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, LSA); wrch(ctx, MFC_EAH, 0); wrch(ctx, MFC_EAL, ea); wrch(ctx, MFC_Cmd, cmd);
}
static void ppu_store_word(uint32_t ea, uint32_t v)
{
    const uint64_t m = spu_ll_mask(ea, 4);
    spu_ll_lock_mask(m);
    memcpy(g_guest_mem + ea, &v, 4);
    spu_coh_notify_write(ea);
    spu_ll_unlock_mask(m);
}

static volatile int t_done;
static uint32_t t_ea;
static void* putllc_thread(void* a)
{
    (void)a;
    atomic_cmd(&g_ctx, t_ea, MFC_PUTLLC_CMD);
    __atomic_store_n(&t_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
/* Issue a PUTLLC on a helper thread while the main thread holds every stripe; returns 1 if it finished while held. */
static int putllc_finishes_while_locked(uint32_t ea)
{
    pthread_t th;
    t_done = 0; t_ea = ea;
    spu_lockline_lock();
    pthread_create(&th, NULL, putllc_thread, NULL);
    usleep(300 * 1000);
    const int fin = __atomic_load_n(&t_done, __ATOMIC_ACQUIRE);
    spu_lockline_unlock();
    pthread_join(th, NULL);
    return fin;
}

/* stubs for what the full runtime provides (same as test_lockline_stress.c) */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
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

int main(void)
{
    const char* e = getenv("KZ3_PUTLLC_FAST");
    const int fast = (e && *e && *e != '0') ? 1 : 0;
    spu_context* c = &g_ctx;
    spu_context_init(c, 0);
    printf("KZ3_PUTLLC_FAST=%d\n", fast);

    /* 1: no reservation */
    memset(g_guest_mem + LINE_A, 0x11, 128);
    memset(c->ls + LSA, 0x22, 128);
    unsigned n0 = g_spu_putllc_count;
    int fin = putllc_finishes_while_locked(LINE_A);
    CHECK(c->atomic_stat == 1, "1: PUTLLC without reservation must fail (stat=%u)", (unsigned)c->atomic_stat);
    CHECK(g_guest_mem[LINE_A] == 0x11, "1: memory changed by a failed PUTLLC");
    CHECK(fin == fast, "1: finished-while-locked=%d, want %d", fin, fast);
    CHECK(g_spu_putllc_count - n0 == (fast ? 0u : 1u), "1: counter delta %u", g_spu_putllc_count - n0);

    /* 2: line changed by the PPU after GETLLAR */
    atomic_cmd(c, LINE_A, MFC_GETLLAR_CMD);
    ppu_store_word(LINE_A + 4, 0xDEADBEEFu);
    memset(c->ls + LSA, 0x33, 128);
    fin = putllc_finishes_while_locked(LINE_A);
    uint32_t w; memcpy(&w, g_guest_mem + LINE_A + 4, 4);
    CHECK(c->atomic_stat == 1, "2: PUTLLC after a PPU store must fail (stat=%u)", (unsigned)c->atomic_stat);
    CHECK(w == 0xDEADBEEFu, "2: the PPU value was overwritten (%08X)", w);
    CHECK(fin == fast, "2: finished-while-locked=%d, want %d", fin, fast);

    /* 3: unchanged line: success needs the stripe in both modes */
    atomic_cmd(c, LINE_A, MFC_GETLLAR_CMD);
    memset(c->ls + LSA, 0x44, 128);
    fin = putllc_finishes_while_locked(LINE_A);
    CHECK(fin == 0, "3: a PUTLLC that can succeed finished while every stripe was held");
    CHECK(c->atomic_stat == 0, "3: PUTLLC on an unchanged line failed (stat=%u)", (unsigned)c->atomic_stat);
    CHECK(g_guest_mem[LINE_A] == 0x44 && g_guest_mem[LINE_A + 127] == 0x44, "3: commit missing");

    /* 4: reservation on A, PUTLLC on B */
    atomic_cmd(c, LINE_A, MFC_GETLLAR_CMD);
    atomic_cmd(c, LINE_B, MFC_PUTLLC_CMD);
    CHECK(c->atomic_stat == 1, "4: PUTLLC on another line must fail (stat=%u)", (unsigned)c->atomic_stat);
    CHECK(!c->resv_valid, "4: the reservation survived a PUTLLC on another line");

    printf(g_fail ? "r143 putllc fast: FAIL (%d)\n" : "r143 putllc fast: PASS (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
