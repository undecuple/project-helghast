/* Patch 1090 host test: the overlay source index (spu_ovl_find, spu_overlay_match_sig in spu_channels.c) returns the
 * entry the old linear scan returned (the first registered source whose EA matches, or whose 16-byte signature matches
 * a GET of >= 512 bytes), on a table with colliding buckets, duplicate EAs and signatures, and signatures that share
 * the first word. Built by tools/spu/test_spu_ovl_index.sh against work/ps3recomp-patched (SKIP without it). */
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_channels.c" /* the unit under test */

/* runtime globals the unit refers to (as tools/spu/test_spu_dma_repeat.c) */
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

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

static uint32_t s_rng = 67;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

/* the pre-1090 loop, kept as the reference */
static int ref_find(uint32_t ea, const uint8_t* ls, uint32_t size)
{
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit = o->has_sig ? (size >= 512 && memcmp(ls, o->sig, 16) == 0) : (o->src_ea == ea);
        if (hit) return i;
    }
    return -1;
}
static int ref_match_sig(const uint8_t hdr[16])
{
    for (int i = 0; i < s_ovl_src_count; i++)
        if (s_ovl_src[i].has_sig && memcmp(hdr, s_ovl_src[i].sig, 16) == 0) return s_ovl_src[i].image_id;
    return 0;
}

int main(void)
{
    static uint8_t sigs[40][16];
    static uint32_t eas[40];
    for (int i = 0; i < 40; i++) {
        for (int j = 0; j < 16; j++) sigs[i][j] = (uint8_t)rnd();
        if (i % 5 == 1) memcpy(sigs[i], sigs[i - 1], 4);  /* same first word, other bytes differ */
        if (i % 7 == 3) memcpy(sigs[i], sigs[i - 2], 16); /* duplicate signature: the first one wins */
        eas[i] = 0x01000000u + (rnd() & 0xFFFF0u);
        if (i % 6 == 2) eas[i] = eas[i - 1]; /* duplicate EA */
    }
    /* interleaved EA and signature sources, up to the table size (128) */
    int img = 1;
    for (int i = 0; i < 40; i++) {
        spu_overlay_register_sig(sigs[i], img++);
        spu_overlay_register_source(eas[i], img++);
        if (i % 4 == 0) spu_overlay_register_region(eas[(i + 11) % 40], 0x2000, img++);
    }
    long n = 0, bad = 0;
    uint8_t buf[16];
    for (int it = 0; it < 400000; it++) {
        const int k = (int)(rnd() % 40);
        uint32_t ea = (rnd() & 3) ? eas[rnd() % 40] : 0x01000000u + (rnd() & 0xFFFF0u);
        if (rnd() & 1)
            memcpy(buf, sigs[k], 16);
        else
            for (int j = 0; j < 16; j++) buf[j] = (uint8_t)rnd();
        if ((rnd() & 3) == 0) memcpy(buf, sigs[k], 4); /* first word only */
        const uint32_t size = (rnd() & 1) ? 512u + (rnd() & 0x3FF) : (rnd() & 0x1FF);
        const int got = spu_ovl_find(ea, buf, size, 0)   /* no verified sources here: lsa unused */, want = ref_find(ea, buf, size);
        n++;
        if (got != want && bad++ < 5) printf("FAIL find ea=0x%08X size=%u: %d vs %d\n", ea, size, got, want);
        const int gs = spu_overlay_match_sig(buf), ws = ref_match_sig(buf);
        if (gs != ws && bad++ < 5) printf("FAIL match_sig: %d vs %d\n", gs, ws);
    }
    printf("ovl index: %d sources, %ld lookups, %ld differ from the linear scan\n", s_ovl_src_count, n, bad);
    return bad ? 1 : 0;
}
