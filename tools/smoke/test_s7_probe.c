/* Host test for KZ3 patch 2204 (S7.0 probes): the pure core in libs/video/rsx_s7p.h with an injected clock and a fake guest
 * memory buffer. Checks 1-8 here (9 = test_s7_probe_summary.py, 10 = static greps in the .sh).
 * Built with -DRSX_S7P_TEST_FAULT=N (N = 1..11) by the control builds, each must fail; without it everything must pass. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_s7p.h"

static int g_fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_fail++;                                                                                                  \
            printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #c);                                                       \
        }                                                                                                              \
    } while (0)
#define CHECKEQ(a, b)                                                                                                  \
    do {                                                                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                                                            \
        if (_a != _b) {                                                                                                \
            g_fail++;                                                                                                  \
            printf("[FAIL] %s:%d %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b);                      \
        }                                                                                                              \
    } while (0)

static long long g_clk;
static long long fclk(void)
{
    return g_clk;
}
static uint8_t g_mem[512 * 4096];

static void mk(rsx_s7p_draw_in* d, unsigned ep, int hit)
{
    memset(d, 0, sizeof *d);
    d->ep = ep;
    d->vc_hit = hit;
    d->vg_reason = -1;
}
static void sp(rsx_s7p_draw_in* d, unsigned loc, unsigned off, unsigned len, int withsrc)
{
    d->sp[d->n_spans].loc = loc;
    d->sp[d->n_spans].off = off;
    d->sp[d->n_spans].len = len;
    d->sp[d->n_spans].src = withsrc ? g_mem + off : NULL;
    d->n_spans++;
}
static void draw1(rsx_s7p* s, unsigned ep, unsigned loc, unsigned off, unsigned len)
{
    rsx_s7p_draw_in d;
    mk(&d, ep, 0);
    sp(&d, loc, off, len, 0);
    rsx_s7p_draw(s, &d);
}
static void fresh(rsx_s7p* s, const char* samp)
{
    CHECK(rsx_s7p_init(s, fclk, samp));
}

static void test_pages(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    long long r0 = s.c.read_loc[0];
    draw1(&s, 1, 0, 100, 200);
    CHECKEQ(s.c.read_loc[0] - r0, 1);
    r0 = s.c.read_loc[0]; /* inside one page */
    draw1(&s, 2, 0, 4096, 4096);
    CHECKEQ(s.c.read_loc[0] - r0, 1);
    r0 = s.c.read_loc[0]; /* exactly one page */
    draw1(&s, 3, 0, 4000, 200);
    CHECKEQ(s.c.read_loc[0] - r0, 2);
    r0 = s.c.read_loc[0]; /* crosses one boundary */
    draw1(&s, 4, 0, 4090, 8000);
    CHECKEQ(s.c.read_loc[0] - r0, 3);
    r0 = s.c.read_loc[0]; /* three pages */
    draw1(&s, 5, 0, 0, 8192);
    CHECKEQ(s.c.read_loc[0] - r0, 2);
    r0 = s.c.read_loc[0]; /* ends exactly on a boundary: no extra page */
    draw1(&s, 6, 0, 0, 100);
    draw1(&s, 6, 0, 50, 100); /* two draws share a page */
    CHECKEQ(s.c.read_loc[0] - r0, 1);
    r0 = s.c.read_loc[0];
    draw1(&s, 7, 0, 0, 10);
    draw1(&s, 7, 1, 0, 10); /* loc0 and loc1 are different pages */
    CHECKEQ(s.c.read_loc[0] - r0, 1);
    CHECKEQ(s.c.read_loc[1], 1);
    rsx_s7p_free(&s);
}

static void draw_src(rsx_s7p* s, unsigned ep, unsigned off, unsigned len)
{
    rsx_s7p_draw_in d;
    mk(&d, ep, 0);
    sp(&d, 0, off, len, 1);
    rsx_s7p_draw(s, &d);
}
static void test_churn(void)
{
    rsx_s7p s;
    fresh(&s, "1");
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (uint8_t)(i * 31u + 7u);
    draw_src(&s, 1, 0, 10 * 4096); /* flip 1: ten pages, all new */
    CHECKEQ(s.c.newp, 10);
    CHECKEQ(s.c.same + s.c.changed + s.c.shape, 0);
    g_mem[2 * 4096 + 5] ^= 0xFF;
    g_mem[5 * 4096 + 4000] ^= 0x01;
    g_mem[7 * 4096] ^= 0x80; /* K = 3 pages mutated */
    draw_src(&s, 2, 0, 10 * 4096);
    CHECKEQ(s.c.changed, 3);
    CHECKEQ(s.c.same, 7);
    CHECKEQ(s.c.hashed, 20);
    draw_src(&s, 3, 0, 9 * 4096 + 100); /* flip 3: page 9 changed shape (len 100 vs 4096) */
    CHECKEQ(s.c.shape, 1);
    CHECKEQ(s.c.same, 7 + 9);
    CHECKEQ(s.c.changed, 3);
    /* a page not read in flip B does not count as `same` in flip C */
    draw_src(&s, 10, 20 * 4096, 4 * 4096); /* A: pages 20..23 */
    long long n0 = s.c.newp, sm0 = s.c.same;
    draw_src(&s, 11, 20 * 4096, 2 * 4096); /* B: 20, 21 */
    draw_src(&s, 12, 20 * 4096, 4 * 4096); /* C: 20..23: 22 and 23 were not read in B */
    CHECKEQ(s.c.newp - n0, 2);
    CHECKEQ(s.c.same - sm0, 2 + 2);
    CHECKEQ(rsx_s7p_est(5, 1), 5);
    rsx_s7p_free(&s);

    /* PAGE_SAMP = 4: the same pages are sampled every flip; est = sampled x 4 */
    fresh(&s, "4");
    draw_src(&s, 1, 0, 256 * 4096);
    long long samp1 = s.c.hashed, exp = 0;
    for (unsigned p = 0; p < 256; p++) exp += rsx_s7p_sampled((((uint64_t)0 << 40) | p) + 1ull, 4);
    CHECKEQ(samp1, exp);
    CHECK(samp1 > 20 && samp1 < 120);
    draw_src(&s, 2, 0, 256 * 4096);
    CHECKEQ(s.c.hashed, 2 * samp1);
    CHECKEQ(s.c.same, samp1);
    CHECKEQ(s.c.changed, 0);
    CHECKEQ(rsx_s7p_est(samp1, 4), samp1 * 4);
    CHECKEQ(s.samp, 4);
    rsx_s7p_free(&s);
    fresh(&s, "0"); /* N = 0 hashes nothing */
    draw_src(&s, 1, 0, 64 * 4096);
    draw_src(&s, 2, 0, 64 * 4096);
    CHECKEQ(s.c.hashed, 0);
    CHECKEQ(s.c.same + s.c.changed + s.c.newp, 0);
    CHECKEQ(rsx_s7p_est(10, 0), 0);
    rsx_s7p_free(&s);
}

static void feed_fill(rsx_s7p* s, long long mv)
{
    rsx_s7p_draw_in d;
    mk(&d, 1, 0);
    d.t_fetch = mv;
    d.refs = 10;
    rsx_s7p_draw(s, &d);
}
static void test_barriers(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    feed_fill(&s, 10);
    feed_fill(&s, 20);
    feed_fill(&s, 30);
    rsx_s7p_barrier_name(&s, "label");
    feed_fill(&s, 40);
    feed_fill(&s, 50);
    rsx_s7p_barrier_name(&s, "get_jump");
    feed_fill(&s, 60);
    feed_fill(&s, 70);
    feed_fill(&s, 80);
    feed_fill(&s, 90);
    rsx_s7p_barrier_name(&s, "ring_jump"); /* not a probe kind: ignored (the funnel's offr calls never reach it) */
    rsx_s7p_barrier(&s, RSX_S7P_CLS_CHUNK, -1);
    rsx_s7p_barrier(&s, RSX_S7P_CLS_CHUNK, -1); /* no draws pending */
    const rsx_s7p_ctr* c = &s.c;
    CHECKEQ(c->chunks, 2);
    CHECKEQ(c->tr[0].joins, 1);
    CHECKEQ(c->tr[0].idle, 1);
    CHECKEQ(c->tr[0].ovl, 450 - 90);
    CHECKEQ(c->tr[0].hist[3], 1);
    CHECKEQ(c->tr[1].joins, 2);
    CHECKEQ(c->tr[1].idle, 1);
    CHECKEQ(c->tr[1].ovl, (60 - 30) + (390 - 90));
    CHECKEQ(c->tr[1].hist[1], 1);
    CHECKEQ(c->tr[1].hist[2], 1);
    CHECKEQ(c->tr[2].joins, 3);
    CHECKEQ(c->tr[2].idle, 1);
    CHECKEQ(c->tr[2].ovl, (60 - 30) + (90 - 50) + (300 - 90));
    CHECKEQ(c->tr[2].hist[1], 2);
    CHECKEQ(c->tr[2].hist[2], 1);
    CHECKEQ(c->by_kind[0], 1);
    CHECKEQ(c->by_kind[13], 1);
    CHECKEQ(c->get_only_cuts, 1);
    CHECKEQ(c->disjoint_barriers, 2);
    CHECKEQ(c->reuse_barriers, 0);
    rsx_s7p_ctr z;
    memset(&z, 0, sizeof z);
    long long h[RSX_S7P_BINS];
    memcpy(h, c->tr[1].hist, sizeof h);
    CHECKEQ(rsx_s7p_pct(h, NULL, 50), 3);
    CHECKEQ(rsx_s7p_pct(h, NULL, 95), 7);
    /* a ring reset with fills pending cuts (no idle), with none it only counts */
    feed_fill(&s, 5);
    feed_fill(&s, 6);
    long long idle0 = s.c.tr[0].idle;
    rsx_s7p_ring_reset(&s);
    rsx_s7p_ring_reset(&s);
    CHECKEQ(s.c.ring_resets, 2);
    CHECKEQ(s.c.tr[0].idle, idle0);
    CHECKEQ(s.c.tr[0].joins, 2);
    rsx_s7p_free(&s);
}

static void test_reuse(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    rsx_s7p_draw_in d;
    mk(&d, 1, 0);
    sp(&d, 0, 0 * 4096, 100, 0);
    sp(&d, 0, 1 * 4096, 100, 0);
    rsx_s7p_draw(&s, &d); /* epoch 1 reads A, B */
    rsx_s7p_barrier(&s, RSX_S7P_CLS_GATE, 0);
    mk(&d, 1, 0);
    sp(&d, 0, 1 * 4096, 100, 0);
    rsx_s7p_draw(&s, &d); /* epoch 2 re-reads B */
    rsx_s7p_barrier(&s, RSX_S7P_CLS_GATE, 0);
    mk(&d, 1, 0);
    sp(&d, 0, 2 * 4096, 100, 0);
    rsx_s7p_draw(&s, &d); /* epoch 3 reads only C */
    rsx_s7p_barrier(&s, RSX_S7P_CLS_GATE, 0);
    CHECKEQ(s.c.disjoint_barriers, 2);
    CHECKEQ(s.c.reuse_barriers, 1);
    rsx_s7p_free(&s);
}

static void test_fill(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    g_clk = 100;
    long long t0 = rsx_s7p_tm_begin(&s);
    g_clk = 130;
    rsx_s7p_tm_end(&s, RSX_S7P_T_VCB, t0); /* 30 */
    g_clk = 200;
    t0 = rsx_s7p_tm_begin(&s);
    g_clk = 205;
    rsx_s7p_tm_end(&s, RSX_S7P_T_PCB, t0); /* 5 */
    rsx_s7p_t_add(&s, RSX_S7P_T_PLAN, 7);
    rsx_s7p_t_add(&s, RSX_S7P_T_FETCH, 11);
    rsx_s7p_draw_in d;
    mk(&d, 1, 0);
    d.t_ring = 13;
    d.t_idx = 17;
    rsx_s7p_draw(&s, &d);
    CHECKEQ(s.misuse, 0);
    long long sum = 0;
    for (int i = 0; i < RSX_S7P_NT; i++) sum += s.c.t[i];
    CHECKEQ(sum, 30 + 5 + 7 + 11 + 13 + 17);
    rsx_s7p_ctr z;
    memset(&z, 0, sizeof z);
    char ln[1024];
    rsx_s7p_fmt_fill(ln, sizeof ln, 143, 1.0, 1, 100.0, 0, &s.c, &z, 0, 0);
    double v = -1;
    const char* q = strstr(ln, "split_sum_ms=");
    CHECK(q && sscanf(q, "split_sum_ms=%lf", &v) == 1);
    CHECK(fabs(v - 83.0) < 1e-9);
    q = strstr(ln, "sum_err_pct=");
    CHECK(q && sscanf(q, "sum_err_pct=%lf", &v) == 1);
    CHECK(fabs(v - 17.0) < 1e-9);
    q = strstr(ln, "movable_ms=");
    CHECK(q && sscanf(q, "movable_ms=%lf", &v) == 1);
    CHECK(fabs(v - 41.0) < 1e-9); /* fetch + ring + idx */
    q = strstr(ln, "stay_ms=");
    CHECK(q && sscanf(q, "stay_ms=%lf", &v) == 1);
    CHECK(fabs(v - 42.0) < 1e-9); /* plan + vcb + pcb */
    rsx_s7p_fmt_fill(ln, sizeof ln, 143, 1.0, 1, 83.0, 0, &s.c, &z, 0, 0);
    q = strstr(ln, "sum_err_pct=");
    CHECK(q && sscanf(q, "sum_err_pct=%lf", &v) == 1);
    CHECK(fabs(v) < 1e-9);
    /* nesting misuse: end without begin, begin while open */
    rsx_s7p_tm_end(&s, RSX_S7P_T_VCB, 0);
    CHECKEQ(s.misuse, 1);
    (void)rsx_s7p_tm_begin(&s);
    (void)rsx_s7p_tm_begin(&s);
    CHECKEQ(s.misuse, 2);
    rsx_s7p_free(&s);
}

static void test_held(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    rsx_s7p_acq(&s, 1, 0x100, 5, 10, 0x40); /* met: opens nothing */
    CHECKEQ(s.c.acq_met, 1);
    CHECKEQ(s.held_open, 0);
    rsx_s7p_acq(&s, 0, 0x100, 5, 10, 0x40); /* unmet: opens a hold */
    CHECKEQ(s.c.acq_unmet, 1);
    CHECKEQ(s.held_open, 1);
    rsx_s7p_draw_in d;
    mk(&d, 1, 0);
    d.now = 20;
    sp(&d, 0, 0, 100, 0);
    rsx_s7p_draw(&s, &d);
    mk(&d, 1, 0);
    d.now = 30;
    sp(&d, 0, 0, 100, 0);
    sp(&d, 0, 4096, 100, 0);
    rsx_s7p_draw(&s, &d);
    CHECKEQ(s.c.held_draws, 2);
    CHECKEQ(s.c.held_pages, 2);
    mk(&d, 1, 0);
    d.now = 40;
    d.held_eq = 1;
    sp(&d, 0, 8192, 100, 0);
    rsx_s7p_draw(&s, &d); /* label equals: the hold closes */
    CHECKEQ(s.held_open, 0);
    CHECKEQ(s.c.held_draws, 2);
    CHECKEQ(s.c.held_ticks, 30);
    mk(&d, 1, 0);
    d.now = 50;
    sp(&d, 0, 8192, 100, 0);
    rsx_s7p_draw(&s, &d);
    CHECKEQ(s.c.held_draws, 2);
    rsx_s7p_acq(&s, 0, 0x100, 6, 60, 0x80); /* second hold, closed by the flip */
    CHECKEQ(s.held_open, 1);
    mk(&d, 2, 0);
    d.now = 70;
    sp(&d, 0, 0, 100, 0);
    rsx_s7p_draw(&s, &d);
    CHECKEQ(s.held_open, 0);
    CHECKEQ(s.c.held_draws, 2);
    CHECKEQ(s.c.held_ticks, 30 + 10);
    rsx_s7p_free(&s);
}

static const char* const k_fill[] = {
    "[s7-fill]",   "plan_ms=", "fetch_ms=",     "ring_ms=",   "idx_ms=",      "vcb_ms=", "pcb_ms=",
    "movable_ms=", "stay_ms=", "split_sum_ms=", "bucket_ms=", "sum_err_pct=", "draws=",  "fills=",
    "hits=",       "refs=",    "refs_B=",       "probe_ms=",  "s7draw_ms=",   0};
static const char* const k_bar[] = {
    "[s7-bar]",       "chunks=",         "chunk: joins=",  "idle=",      "per_chunk=", "p50=",
    "p95=",           "ovl_ms=",         "narrow: joins=", "reuse=",     "disjoint=",  "all: joins=",
    "get_only_cuts=", "by_kind: label=", "ref=",           "report=",    "sem_be=",    "copy0039=",
    "copy3089=",      "store308a=",      "surf_wb=",       "wbd_drain=", "flip=",      "usercmd=",
    "present=",       "notify=",         "get_jump=",      "get_rel=",   "get_end=",   0};
static const char* const k_draw[] = {
    "[s7-draw]",        "fills_loc0=",      "fills_loc1=", "hits_loc0=",   "hits_loc1=",
    "inline_nostable=", "inline_late=",     "vg_ok=",      "vg_divided=",  "vg_unmapped=",
    "vg_type=",         "vg_big=",          "vg_noinput=", "vb_B=",        "ib_B=",
    "payload_B=",       "span_B=",          "size_p50=",   "size_p95=",    "refs_p50=",
    "refs_p95=",        "dev_fills=",       "dev_fill_B=", "refill_main=", "dyn_skip=",
    "inval_dyn=",       "inel_main_verts=", "src_snap_B=", "dev_frees=",   0};
static const char* const k_page[] = {
    "[s7-page]", "samp=",      "read_loc0=",  "read_loc1=",  "hashed=",     "same=",   "changed=",
    "new=",      "shape=",     "hit_nohash=", "churn_pct=",  "copy_B_est=", "ring_B=", "page_vs_ring=",
    "acq_met=",  "acq_unmet=", "held_draws=", "held_pages=", "held_ms=",    "ovf=",    0};
static const char* const k_ww[] = {"[s7-ww]",
                                   "pages=",
                                   "rounds=",
                                   "frac=",
                                   "base_us_page=",
                                   "watch_first_us_page=",
                                   "watch_again_us_page=",
                                   "arm_us=",
                                   "query_us=",
                                   "reported=",
                                   "cost_ms_flip_est=",
                                   0};

static int in_order(const char* ln, const char* const* keys)
{
    if (strncmp(ln, keys[0], strlen(keys[0])) != 0) return 0;
    const char* at = ln;
    for (int i = 1; keys[i]; i++) {
        const char* q = strstr(at, keys[i]);
        if (!q) {
            printf("   missing/out of order: %s\n", keys[i]);
            return 0;
        }
        at = q + 1;
    }
    return 1;
}
static int guard_ok(const char* buf, size_t n, size_t total)
{
    for (size_t i = n; i < total; i++)
        if ((unsigned char)buf[i] != 0xA5) return 0;
    return 1;
}
static void test_fmt(void)
{
    rsx_s7p s;
    fresh(&s, "1");
    rsx_s7p_draw_in d;
    mk(&d, 1, 0);
    d.refs = 100;
    d.vb_B = 4000;
    d.ib_B = 600;
    d.t_fetch = 9;
    d.vg_reason = 2;
    sp(&d, 1, 0, 8192, 1);
    rsx_s7p_draw(&s, &d);
    rsx_s7p_barrier(&s, RSX_S7P_CLS_CHUNK, -1);
    rsx_s7p_ctr z;
    memset(&z, 0, sizeof z);
    rsx_s7p_ext e0, e1;
    memset(&e0, 0, sizeof e0);
    memset(&e1, 0, sizeof e1);
    e1.dev_fills = 3;
    e1.dev_fill_B = 6000;
    rsx_s7p_ww_res r;
    memset(&r, 0, sizeof r);
    r.first_us_page = 2.0;
    char ln[2048], big[256];
    rsx_s7p_fmt_fill(ln, sizeof ln, 143, 0.001, 1, 7.9, 1.25, &s.c, &z, 5, 100);
    CHECK(in_order(ln, k_fill));
    double v0 = -1;
    const char* q0 = strstr(ln, "s7draw_ms=");
    CHECK(q0 && sscanf(q0, "s7draw_ms=%lf", &v0) == 1 &&
          fabs(v0 - 1.25) < 1e-9); /* the hook's own bucket, not bucket_ms (7.9) */
    rsx_s7p_fmt_bar(ln, sizeof ln, 143, 0.001, 1, &s.c, &z);
    CHECK(in_order(ln, k_bar));
    rsx_s7p_fmt_draw(ln, sizeof ln, 143, 1, &s.c, &z, &e1, &e0);
    CHECK(in_order(ln, k_draw));
    double v = -1;
    const char* q = strstr(ln, "vg_divided=");
    CHECK(q && sscanf(q, "vg_divided=%lf", &v) == 1 && fabs(v - 1.0) < 1e-9);
    q = strstr(ln, "fills_loc1=");
    CHECK(q && sscanf(q, "fills_loc1=%lf", &v) == 1 && fabs(v - 1.0) < 1e-9);
    q = strstr(ln, "dev_fill_B=");
    CHECK(q && sscanf(q, "dev_fill_B=%lf", &v) == 1 && fabs(v - 6000.0) < 1e-9);
    rsx_s7p_fmt_page(ln, sizeof ln, 143, 0.001, 1, 1, &s.c, &z, &e1, &e0);
    CHECK(in_order(ln, k_page));
    q = strstr(ln, "read_loc1=");
    CHECK(q && sscanf(q, "read_loc1=%lf", &v) == 1 && fabs(v - 2.0) < 1e-9);
    rsx_s7p_fmt_ww(ln, sizeof ln, 1280, 60, 100, &r, 25, &r, 10.0);
    CHECK(in_order(ln, k_ww));
    q = strstr(ln, "cost_ms_flip_est=");
    CHECK(q && sscanf(q, "cost_ms_flip_est=%lf", &v) == 1 && fabs(v - 0.02) < 1e-9);
    /* a 40-byte buffer is never overrun */
    memset(big, 0xA5, sizeof big);
    rsx_s7p_fmt_fill(big, 40, 143, 1.0, 1, 7.9, 1.25, &s.c, &z, 5, 100);
    CHECK(guard_ok(big, 40, sizeof big));
    CHECKEQ(big[39], 0);
    memset(big, 0xA5, sizeof big);
    rsx_s7p_fmt_bar(big, 40, 143, 1.0, 1, &s.c, &z);
    CHECK(guard_ok(big, 40, sizeof big));
    memset(big, 0xA5, sizeof big);
    rsx_s7p_fmt_draw(big, 40, 143, 1, &s.c, &z, &e1, &e0);
    CHECK(guard_ok(big, 40, sizeof big));
    memset(big, 0xA5, sizeof big);
    rsx_s7p_fmt_page(big, 40, 143, 1.0, 1, 1, &s.c, &z, &e1, &e0);
    CHECK(guard_ok(big, 40, sizeof big));
    memset(big, 0xA5, sizeof big);
    rsx_s7p_fmt_ww(big, 40, 1280, 60, 100, &r, 25, &r, 1.0);
    CHECK(guard_ok(big, 40, sizeof big));
    rsx_s7p_free(&s);
    /* table overflow is counted, never a crash: 40000 distinct pages in one flip, the table holds 32768 */
    fresh(&s, "0");
    draw1(&s, 1, 0, 0, 40000u * 4096u);
    CHECK(s.c.ovf > 0);
    CHECKEQ(s.c.read_loc[0], RSX_S7P_EP_MAX);
    rsx_s7p_fmt_page(ln, sizeof ln, 143, 1.0, 1, 0, &s.c, &z, &e0, &e0);
    q = strstr(ln, "ovf=");
    CHECK(q && sscanf(q, "ovf=%lf", &v) == 1 && v > 0);
    rsx_s7p_free(&s);
}

typedef struct {
    double clock;
    unsigned dirty[1280];
    unsigned n_dirty;
} wwx;
static double ww_now(void* c)
{
    return ((wwx*)c)->clock;
}
static void ww_plain(void* c, unsigned p)
{
    (void)p;
    ((wwx*)c)->clock += 0.1;
}
static void ww_watched(void* c, unsigned p)
{
    wwx* w = (wwx*)c;
    if (w->dirty[p])
        w->clock += 0.2;
    else {
        w->clock += 2.0;
        w->dirty[p] = 1;
        w->n_dirty++;
    }
}
static void ww_reset(void* c)
{
    wwx* w = (wwx*)c;
    memset(w->dirty, 0, sizeof w->dirty);
    w->n_dirty = 0;
    w->clock += 50.0;
}
static unsigned ww_query(void* c)
{
    wwx* w = (wwx*)c;
    w->clock += 30.0;
    return w->n_dirty;
}
static void test_ww(void)
{
    static const rsx_s7p_ww_ops ops = {ww_now, ww_plain, ww_watched, ww_reset, ww_query};
    static wwx w;
    rsx_s7p_ww_res r;
    memset(&w, 0, sizeof w);
    rsx_s7p_ww_run(&ops, &w, 1280, 60, 100, &r);
    CHECK(fabs(r.base_us_page - 0.1) < 1e-6);
    CHECK(fabs(r.first_us_page - 2.0) < 1e-6);
    CHECK(fabs(r.again_us_page - 0.2) < 1e-6);
    CHECK(fabs(r.arm_us - 50.0) < 1e-6);
    CHECK(fabs(r.query_us - 30.0) < 1e-6);
    CHECKEQ(r.reported, 1280);
    rsx_s7p_ww_run(&ops, &w, 1280, 60, 25, &r);
    CHECKEQ(r.reported, 320);
    CHECK(fabs(r.first_us_page - 2.0) < 1e-6);
}

/* the sampler is unbiased for any samp (multiply-shift), not only the powers of two */
static void test_sampler(void)
{
    static const int samps[] = {1, 2, 3, 4, 5, 7, 10, 40};
    for (unsigned k = 0; k < sizeof samps / sizeof *samps; k++) {
        const int sp_ = samps[k];
        long long n = 0;
        const unsigned keys = 1u << 20;
        for (unsigned p = 0; p < keys; p++) n += rsx_s7p_sampled(((uint64_t)p) + 1ull, sp_);
        const double got = (double)n / (double)keys, want = 1.0 / (double)sp_;
        if (fabs(got - want) / want > 0.01) {
            g_fail++;
            printf("[FAIL] %s:%d samp=%d sampled fraction %.5f, want %.5f\n", __FILE__, __LINE__, sp_, got, want);
        }
    }
    CHECKEQ(rsx_s7p_sampled(123, 0), 0); /* samp 0 = no hashing */
}
/* the cold-hash measurement: the page order is a bijection over the whole buffer (every page once) and the unit is ticks per page */
static long long g_cc, g_ct;
static long long cclk(void)
{
    g_cc++;
    return g_ct * (g_cc - 1); /* first call 0, second call g_ct */
}
static void test_cold(void)
{
    static unsigned char seen[RSX_S7P_COLD_PAGES];
    unsigned distinct = 0;
    memset(seen, 0, sizeof seen);
    for (unsigned i = 0; i < RSX_S7P_COLD_PAGES; i++) {
        const unsigned p = rsx_s7p_cold_page(i, RSX_S7P_COLD_PAGES);
        CHECK(p < RSX_S7P_COLD_PAGES);
        if (p < RSX_S7P_COLD_PAGES && !seen[p]) {
            seen[p] = 1;
            distinct++;
        }
    }
    CHECKEQ(distinct, RSX_S7P_COLD_PAGES);
    CHECK((size_t)RSX_S7P_COLD_PAGES * 4096u >=
          (size_t)32 << 20); /* beyond the caches: a smaller buffer measures a hot hash */
    static uint8_t buf[64 * 4096];
    g_cc = 0;
    g_ct = 64 * 3;
    const double u = rsx_s7p_measure_cold(cclk, buf, 64);
    CHECKEQ(g_cc, 2); /* the clock is read twice, around the whole walk, not per page */
    CHECK(fabs(u - 3.0) < 1e-9);
    rsx_s7p s;
    CHECK(rsx_s7p_init(&s, fclk, "0"));
    CHECKEQ(s.cold_hash, 1);
    rsx_s7p_free(&s);
}
/* an unmet acquire re-polled every walk pass is one acquire (GCM_SEMA_ACQUIRE=1) */
static void test_acq_repoll(void)
{
    rsx_s7p s;
    fresh(&s, "0");
    for (int i = 0; i < 5; i++) rsx_s7p_acq(&s, 0, 0x100, 5, 10 + i, 0x40);
    CHECKEQ(s.c.acq_unmet, 1);
    rsx_s7p_acq(&s, 0, 0x100, 5, 20, 0x50); /* a different acquire (position), same label and value */
    CHECKEQ(s.c.acq_unmet, 2);
    rsx_s7p_acq(&s, 1, 0x100, 5, 21, 0x50); /* it is met at last */
    CHECKEQ(s.c.acq_met, 1);
    rsx_s7p_acq(&s, 0, 0x100, 5, 22, 0x50); /* the same position later, after it was met: a new acquire */
    CHECKEQ(s.c.acq_unmet, 3);
    draw1(&s, 1, 0, 0, 100); /* the walker moved on */
    rsx_s7p_acq(&s, 0, 0x100, 5, 23, 0x50);
    CHECKEQ(s.c.acq_unmet, 4);
    rsx_s7p_free(&s);
}

int main(void)
{
    test_sampler();
    test_cold();
    test_acq_repoll();
    test_pages();
    test_churn();
    test_barriers();
    test_reuse();
    test_fill();
    test_held();
    test_fmt();
    test_ww();
    if (g_fail) {
        printf("[test_s7_probe] %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("[test_s7_probe] all checks passed (fault=%d)\n",
#ifdef RSX_S7P_TEST_FAULT
           RSX_S7P_TEST_FAULT
#else
           0
#endif
    );
    return 0;
}
