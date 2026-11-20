/* Host test for KZ3 patch 2195: the [o0-vtx] probe core in libs/video/rsx_o0.h (range parse, classification, per-draw
 * accounting with known bytes, per-flip distinct/reuse, line format). Needs rsx_o0v_* from the patched toolkit. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_o0.h"

rsx_o0 g_rsx_o0;
static rsx_o0v V;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int near(double a, double b) { return fabs(a - b) < 1e-6; }

int main(void)
{
    unsigned long long lo, hi;
    /* parse */
    CHECK(rsx_o0v_parse("0xC0B00000,0xC1000000", &lo, &hi) && lo == 0xC0B00000ull && hi == 0xC1000000ull);
    CHECK(rsx_o0v_parse("100,200", &lo, &hi) && lo == 100 && hi == 200);
    CHECK(!rsx_o0v_parse("200,100", &lo, &hi));        /* lo >= hi */
    CHECK(!rsx_o0v_parse("5,5", &lo, &hi));
    CHECK(!rsx_o0v_parse("1,0x200000000", &lo, &hi));  /* > 2^32 */
    CHECK(!rsx_o0v_parse("1;2", &lo, &hi) && !rsx_o0v_parse("1,", &lo, &hi) && !rsx_o0v_parse("", &lo, &hi) && !rsx_o0v_parse(NULL, &lo, &hi));
    CHECK(!rsx_o0v_parse("1,2x", &lo, &hi));
    rsx_o0v_init(&V, NULL); CHECK(V.lo == 0xC0B00000ull && V.hi == 0xC1000000ull);
    rsx_o0v_init(&V, "bad"); CHECK(V.lo == 0xC0B00000ull);   /* falls back (prints a warning) */
    rsx_o0v_init(&V, "0xC0000000,0xC0100000"); CHECK(V.lo == 0xC0000000ull && V.hi == 0xC0100000ull);
    rsx_o0v_init(&V, NULL);
    /* classification: location 0 is 0xC0000000 + off; edges are half open */
    CHECK(rsx_o0v_in_range(&V, 0, 0x00B00000u, 16));
    CHECK(rsx_o0v_in_range(&V, 0, 0x00AFFFF0u, 17));     /* last byte inside */
    CHECK(!rsx_o0v_in_range(&V, 0, 0x00AFFFF0u, 16));    /* ends exactly at lo */
    CHECK(rsx_o0v_in_range(&V, 0, 0x0100000u - 16u + 0x00F00000u, 16));   /* 0x00FFFFF0..0x01000000 */
    CHECK(!rsx_o0v_in_range(&V, 0, 0x01000000u, 16));    /* starts at hi */
    CHECK(!rsx_o0v_in_range(&V, 1, 0x00B00000u, 16));    /* main memory offset never matches a 0xC... range */
    CHECK(!rsx_o0v_in_range(&V, 0, 0x00B00000u, 0));
    /* synthetic stream, flip 1: A (range) twice, B (range, other loc-0 span) once, C (outside) twice.
     * Ticks: hash/conv/up as given; bytes as given. */
    memset(&V, 0, sizeof V); rsx_o0v_init(&V, NULL);
    const rsx_o0v_span A[1] = {{0, 0x00B10000u, 4096}};
    const rsx_o0v_span B[2] = {{0, 0x00C00000u, 8192}, {0, 0x00100000u, 512}};
    const rsx_o0v_span C[1] = {{0, 0x00100000u, 1024}};
    rsx_o0_lock(&g_rsx_o0); rsx_o0_unlock(&g_rsx_o0);
    rsx_o0v_draw(&V, &g_rsx_o0, 1, A, 1, 0, 0, 10, 20, 30, 100, 4096, 4096);    /* miss */
    rsx_o0v_draw(&V, &g_rsx_o0, 1, A, 1, 0, 1, 5, 0, 0, 4096, 0, 0);            /* hit, same range: a reuse */
    rsx_o0v_draw(&V, &g_rsx_o0, 1, B, 2, 1, 0, 1, 2, 3, 0, 8704, 8704);         /* mixed, 1 attr without span */
    rsx_o0v_draw(&V, &g_rsx_o0, 1, C, 1, 0, 0, 7, 8, 9, 0, 1024, 1024);
    rsx_o0v_draw(&V, &g_rsx_o0, 1, C, 1, 0, 1, 1, 0, 0, 1024, 0, 0);
    /* flip 2: A again (new flip: distinct again, no reuse yet), flip 3 empty draw opens the fold of flip 2 */
    rsx_o0v_draw(&V, &g_rsx_o0, 2, A, 1, 0, 0, 10, 20, 30, 100, 4096, 4096);
    rsx_o0v_draw(&V, &g_rsx_o0, 3, C, 1, 0, 0, 1, 1, 1, 0, 1, 1);
    const rsx_o0v_side* in = &V.c.s[0]; const rsx_o0v_side* out = &V.c.s[1];
    CHECK(in->draws == 4 && in->mixed == 1 && in->hit == 1 && in->miss == 3 && in->fetches == 3);
    CHECK(in->nospan_draws == 1 && in->nospan_spans == 1);
    CHECK(in->b_hash == 100 + 4096 + 0 + 100 && in->b_conv == 4096 + 0 + 8704 + 4096 && in->b_up == 4096 + 8704 + 4096);
    CHECK(in->t_hash == 10 + 5 + 1 + 10 && in->t_conv == 20 + 0 + 2 + 20 && in->t_up == 30 + 0 + 3 + 30);
    CHECK(in->span_bytes == 4096 + 4096 + 8192 + 4096);   /* only the in-range spans of the draws */
    CHECK(out->draws == 3 && out->hit == 1 && out->miss == 2 && out->t_hash == 7 + 1 + 1 && out->b_conv == 1024 + 1);
    /* ranges: flip 1: A, B-range = 2 distinct, 1 reuse (A) ; flip 2: A = 1 distinct, 0 reuse; flip 3 open (not folded) */
    CHECK(V.c.flips_seen == 2 && V.c.dist_sum == 3 && V.c.dist_max == 2 && V.c.reuse_n == 1 && V.c.reused_ranges == 1);
    CHECK(V.c.ranges_sum == 4 && V.c.ovf == 0);
    /* the same stream with another range sorts differently (negative control: classification really depends on the range) */
    memset(&V, 0, sizeof V); rsx_o0v_init(&V, "0xC0100000,0xC0100400");
    rsx_o0v_draw(&V, &g_rsx_o0, 1, A, 1, 0, 0, 10, 20, 30, 100, 4096, 4096);
    rsx_o0v_draw(&V, &g_rsx_o0, 1, C, 1, 0, 0, 7, 8, 9, 0, 1024, 1024);
    CHECK(V.c.s[0].draws == 1 && V.c.s[1].draws == 1 && V.c.s[0].t_conv == 8 && V.c.s[1].t_conv == 20);
    /* overflow: more distinct spans in one flip than the table can hold is counted, never loops */
    memset(&V, 0, sizeof V); rsx_o0v_init(&V, NULL);
    for (unsigned i = 0; i < RSX_O0V_TAB; i++) {
        rsx_o0v_span s = {0, 0x00B00000u + i * 16u, 16};
        rsx_o0v_draw(&V, &g_rsx_o0, 7, &s, 1, 0, 0, 1, 1, 1, 0, 0, 0);
    }
    CHECK(V.c.ovf == (long long)RSX_O0V_TAB - (long long)RSX_O0V_TAB / 2 && V.cur_dist == (long long)RSX_O0V_TAB / 2);
    /* the line: per flip values, exact format */
    memset(&V, 0, sizeof V); rsx_o0v_init(&V, NULL);
    rsx_o0v_draw(&V, &g_rsx_o0, 1, A, 1, 0, 0, 1000, 2000, 3000, 100, 4096, 4096);
    rsx_o0v_draw(&V, &g_rsx_o0, 1, A, 1, 0, 1, 500, 0, 0, 4096, 0, 0);
    rsx_o0v_draw(&V, &g_rsx_o0, 2, C, 1, 0, 0, 100, 100, 100, 0, 64, 64);
    rsx_o0v_draw(&V, &g_rsx_o0, 3, C, 1, 0, 0, 100, 100, 100, 0, 64, 64);
    rsx_o0v_ctr z; memset(&z, 0, sizeof z);
    char ln[1024]; rsx_o0v_fmt(ln, sizeof ln, 12, 0.001, 2, V.lo, V.hi, &V.c, &z);
    const char* want =
        "[o0-vtx] t=12 flips=2 range=0xC0B00000..0xC1000000"
        " | in draws=1.0 mixed=0.0 fetches=0.5 vc_hit=0.5 vc_miss=0.5 nospan=0.0 hashB=2098 convB=2048 upB=2048 hash_ms=0.750 conv_ms=1.000 up_ms=1.500"
        " | out draws=1.0 mixed=0.0 fetches=1.0 vc_hit=0.0 vc_miss=1.0 nospan=0.0 hashB=0 convB=64 upB=64 hash_ms=0.100 conv_ms=0.100 up_ms=0.100"
        " | ranges per_flip_mean=0.50 max=1 range_span_B=4096 reuses=0.50 reused_ranges=0.50 reused_in_flip=1 ovf=0";
    if (strcmp(ln, want)) { printf("[FAIL] line\n got:  %s\n want: %s\n", ln, want); fails++; }
    /* a period with no flips prints totals, never divides by zero */
    rsx_o0v_fmt(ln, sizeof ln, 1, 0.001, 0, V.lo, V.hi, &V.c, &V.c);
    CHECK(strstr(ln, "flips=0") && strstr(ln, "in draws=0.0") && !strstr(ln, "nan") && !strstr(ln, "inf"));
    (void)near;
    if (fails) { printf("[test_o0_vtx] %d FAILED\n", fails); return 1; }
    printf("[test_o0_vtx] ok\n");
    return 0;
}
