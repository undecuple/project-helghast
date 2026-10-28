/* Patch 1056 host test: lock-line reservations under contention, through the real channel path
 * (spu_wrch -> spu_mfc_atomic / mfc_do_transfer, as runtime/spu/tests/test_spu_lockline_peer.c does).
 * Run by tools/spu/test_lockline.sh in both lock modes (default striped + seqlock GETLLAR, KZ3_LL_OLD=1).
 *
 *  1. counter: 4 SPU threads GETLLAR/PUTLLC-increment one word, a "PPU" thread increments another word of
 *     the same line through the coherent-store path (stripe lock + notify), a 6th thread increments a word on
 *     another line (another stripe): no lost update.
 *  2. torn read: a writer PUTLLUCs a line alternately filled with 0xAA and 0x55; a reader's GETLLAR copies
 *     (local store and reservation snapshot) are never mixed.
 *  3. ABA: A reserves X; B commits Y then X (PUTLLC twice, then the PPU store path twice); A's PUTLLC fails
 *     and A has SPU_EVENT_LR, although the bytes are back to what A read.
 *  4. hand-off: A alternates GETLLAR on L1 (written constantly by B's PUTLLUC) and GETLLAR+PUTLLC on L2 (only
 *     A writes it): A's PUTLLC on L2 never fails (a notifier on L1 must not kill A's reservation after it moved).
 *  Exit 0 = pass. Args: [iterations per thread] (default 200000). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 20] __attribute__((aligned(4096)));
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c" /* via -I runtime/spu */

#define LINE_C 0x00030000u /* counter line: SPU word at +0, PPU word at +64 */
#define LINE_E 0x00030080u /* second counter line (adjacent: another stripe) */
#define LINE_T 0x00030400u /* torn-read line */
#define LINE_X 0x00030800u /* ABA line */
#define LINE_1 0x00030C00u /* hand-off: written by B */
#define LINE_2 0x00031000u /* hand-off: only A */
#define LSA 0x1000u

static spu_context g_ctx[8];
static int g_fail;
static long g_iters = 200000;
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
static uint32_t* ls_word(spu_context* ctx, uint32_t off)
{
    return (uint32_t*)&ctx->ls[LSA + off];
}
static void spu_increment(spu_context* ctx, uint32_t ea, uint32_t off, long* retries)
{
    for (;;) {
        atomic_cmd(ctx, ea, MFC_GETLLAR_CMD);
        (*ls_word(ctx, off))++;
        atomic_cmd(ctx, ea, MFC_PUTLLC_CMD);
        if (ctx->atomic_stat == 0) return;
        (*retries)++;
    }
}
/* The PPU coherent-store path (ppu_memory.h VM_WRITE_COH / ppu_stwcx on a reserved line). */
static void ppu_increment(uint32_t ea)
{
    const uint64_t m = spu_ll_mask(ea, 4);
    spu_ll_lock_mask(m);
    uint32_t* w = (uint32_t*)(g_guest_mem + ea);
    __atomic_store_n(w, *w + 1u, __ATOMIC_RELAXED);
    spu_coh_notify_write(ea);
    spu_ll_unlock_mask(m);
}
static void ppu_store_line(uint32_t ea, uint8_t fill)
{
    const uint64_t m = spu_ll_mask(ea, 128);
    spu_ll_lock_mask(m);
    memset(g_guest_mem + ea, fill, 128);
    spu_coh_notify_write(ea);
    spu_ll_unlock_mask(m);
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* 1. counter */
static long g_retries[8];
static void* spu_counter(void* a)
{
    int i = (int)(intptr_t)a;
    for (long n = 0; n < g_iters; n++) spu_increment(&g_ctx[i], i == 5 ? LINE_E : LINE_C, 0, &g_retries[i]);
    return NULL;
}
static void* ppu_counter(void* a)
{
    (void)a;
    for (long n = 0; n < g_iters; n++) ppu_increment(LINE_C + 64);
    return NULL;
}
static void test_counter(void)
{
    memset(g_guest_mem + LINE_C, 0, 256);
    pthread_t t[6];
    double t0 = now();
    for (int i = 0; i < 6; i++) pthread_create(&t[i], NULL, i == 4 ? ppu_counter : spu_counter, (void*)(intptr_t)i);
    for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
    double dt = now() - t0;
    uint32_t c = *(uint32_t*)(g_guest_mem + LINE_C), p = *(uint32_t*)(g_guest_mem + LINE_C + 64),
             e = *(uint32_t*)(g_guest_mem + LINE_E);
    long r = 0;
    for (int i = 0; i < 6; i++) r += g_retries[i];
    printf("counter: spu %u/%ld ppu %u/%ld other-line %u/%ld, %ld PUTLLC retries, %.0f ns per increment\n", c,
           4 * g_iters, p, g_iters, e, g_iters, r, dt / (6.0 * g_iters) * 1e9);
    CHECK(c == (uint32_t)(4 * g_iters), "SPU counter lost updates: %u of %ld", c, 4 * g_iters);
    CHECK(p == (uint32_t)g_iters, "PPU counter lost updates: %u of %ld", p, g_iters);
    CHECK(e == (uint32_t)g_iters, "other-line counter lost updates: %u of %ld", e, g_iters);
}

/* 2. torn read */
static volatile int g_stop;
static void* torn_writer(void* a)
{
    spu_context* c = &g_ctx[6];
    (void)a;
    for (long n = 0; !g_stop; n++) {
        memset(&c->ls[LSA], (n & 1) ? 0x55 : 0xAA, 128);
        atomic_cmd(c, LINE_T, MFC_PUTLLUC_CMD);
    }
    return NULL;
}
static void test_torn(void)
{
    memset(g_guest_mem + LINE_T, 0xAA, 128);
    g_stop = 0;
    pthread_t t;
    pthread_create(&t, NULL, torn_writer, NULL);
    spu_context* c = &g_ctx[7];
    long torn = 0, snap = 0;
    for (long n = 0; n < g_iters; n++) {
        atomic_cmd(c, LINE_T, MFC_GETLLAR_CMD);
        const uint8_t* l = &c->ls[LSA];
        for (int k = 1; k < 128; k++)
            if (l[k] != l[0]) {
                torn++;
                break;
            }
        snap += memcmp(l, c->resv_line, 128) != 0;
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("torn read: %ld GETLLARs against a PUTLLUC writer, %ld torn, %ld snapshot mismatches\n", g_iters, torn,
           snap);
    CHECK(torn == 0, "%ld torn GETLLAR copies", torn);
    CHECK(snap == 0, "%ld snapshot != local store", snap);
}

/* 3. ABA */
static void test_aba(void)
{
    spu_context *a = &g_ctx[0], *b = &g_ctx[1];
    for (int round = 0; round < 2; round++) {
        memset(g_guest_mem + LINE_X, 0x11, 128);
        atomic_cmd(a, LINE_X, MFC_GETLLAR_CMD);
        __atomic_store_n(&a->event_status, 0, __ATOMIC_SEQ_CST);
        if (round == 0) {
            atomic_cmd(b, LINE_X, MFC_GETLLAR_CMD);
            memset(&b->ls[LSA], 0x22, 128);
            atomic_cmd(b, LINE_X, MFC_PUTLLC_CMD);
            CHECK(b->atomic_stat == 0, "ABA: B's first PUTLLC failed");
            atomic_cmd(b, LINE_X, MFC_GETLLAR_CMD);
            memset(&b->ls[LSA], 0x11, 128);
            atomic_cmd(b, LINE_X, MFC_PUTLLC_CMD);
            CHECK(b->atomic_stat == 0, "ABA: B's second PUTLLC failed");
        } else {
            ppu_store_line(LINE_X, 0x22);
            ppu_store_line(LINE_X, 0x11);
        }
        (*ls_word(a, 0))++;
        atomic_cmd(a, LINE_X, MFC_PUTLLC_CMD);
        CHECK(a->atomic_stat == 1, "ABA (%s): A's PUTLLC committed over an A-B-A change",
              round ? "PPU stores" : "SPU PUTLLC");
        CHECK(a->event_status & SPU_EVENT_LR, "ABA (%s): A has no SPU_EVENT_LR", round ? "PPU stores" : "SPU PUTLLC");
        CHECK(g_guest_mem[LINE_X] == 0x11, "ABA: line changed by the failed PUTLLC");
    }
    printf("ABA: SPU PUTLLC and PPU store cases checked\n");
}

/* 4. hand-off */
static void* handoff_writer(void* a)
{
    spu_context* c = &g_ctx[1];
    (void)a;
    for (long n = 0; !g_stop; n++) {
        memset(&c->ls[LSA], (int)(n & 0xFF), 128);
        atomic_cmd(c, LINE_1, MFC_PUTLLUC_CMD);
    }
    return NULL;
}
static void test_handoff(void)
{
    memset(g_guest_mem + LINE_1, 0, 256);
    memset(g_guest_mem + LINE_2, 0, 128);
    g_stop = 0;
    pthread_t t;
    pthread_create(&t, NULL, handoff_writer, NULL);
    spu_context* a = &g_ctx[0];
    long fails = 0;
    for (long n = 0; n < g_iters; n++) {
        atomic_cmd(a, LINE_1, MFC_GETLLAR_CMD);
        atomic_cmd(a, LINE_2, MFC_GETLLAR_CMD);
        (*ls_word(a, 0))++;
        atomic_cmd(a, LINE_2, MFC_PUTLLC_CMD);
        fails += a->atomic_stat != 0;
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("hand-off: %ld moves L1 -> L2 under a writer on L1, %ld spurious PUTLLC failures on L2, L2 = %u\n", g_iters,
           fails, *(uint32_t*)(g_guest_mem + LINE_2));
    CHECK(fails == 0, "hand-off: %ld PUTLLCs on L2 failed (a notifier on L1 killed the moved reservation)", fails);
    CHECK(*(uint32_t*)(g_guest_mem + LINE_2) == (uint32_t)(g_iters - fails), "hand-off: L2 counter wrong");
}

int main(int argc, char** argv)
{
    if (argc > 1) g_iters = atol(argv[1]);
    for (int i = 0; i < 8; i++) {
        spu_context_init(&g_ctx[i], 0);
        g_ctx[i].spu_id = (uint32_t)i;
    }
    printf("lock mode: %s\n", spu_ll_old() ? "old (KZ3_LL_OLD=1: one lock, locked GETLLAR)" : "striped + seqlock");
    test_aba();
    test_counter();
    test_torn();
    test_handoff();
    printf("lock-line stress: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}

/* The runtime beyond the SPU channel layer, stubbed (as in test_spu_lockline_peer.c). */
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
unsigned long long ps3_ms_now(void)
{
    return 0;
}
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
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
