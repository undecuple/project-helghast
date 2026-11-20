/* Host test for KZ3 patch 2184 (libs/video/rsx_wb_probe.h): LD_WB_SKIP parser, slack/GET/PUT bookkeeping, guest hash.
 * No GPU, no game data. Exit 0 = all checks passed. With "race" runs a two-thread GET/store stress (build with TSan). */
#include <stdio.h>
#include <pthread.h>
#include "rsx_wb_probe.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static int near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }
#define MS(x) ((uint64_t)((x) * 1e6))

static void test_skip(void)
{
    uint32_t l[RSX_WBP_SKIP_MAX]; unsigned bad;
    unsigned n = rsx_wbs_parse("0x01892400", l, RSX_WBP_SKIP_MAX, &bad);
    CHECK(n == 1 && bad == 0 && l[0] == 0x01892400u);
    n = rsx_wbs_parse("0x1892400, 0x16D0400 26000000", l, RSX_WBP_SKIP_MAX, &bad);
    CHECK(n == 3 && bad == 0 && l[1] == 0x16D0400u && l[2] == 26000000u);
    n = rsx_wbs_parse("0x1892400,zz,0x,-4,12x,,", l, RSX_WBP_SKIP_MAX, &bad);
    CHECK(n == 1 && bad == 4);
    n = rsx_wbs_parse("0x100000000", l, RSX_WBP_SKIP_MAX, &bad);   /* does not fit 32 bits */
    CHECK(n == 0 && bad == 1);
    n = rsx_wbs_parse("", l, RSX_WBP_SKIP_MAX, &bad); CHECK(n == 0 && bad == 0);
    n = rsx_wbs_parse(NULL, l, RSX_WBP_SKIP_MAX, &bad); CHECK(n == 0 && bad == 0);
    n = rsx_wbs_parse("1,2,3,4", l, 2, &bad); CHECK(n == 2 && bad == 2);   /* over capacity is reported, not silent */
    n = rsx_wbs_parse("0x01892400,0x016D0400", l, RSX_WBP_SKIP_MAX, &bad);
    CHECK(rsx_wbs_has(l, n, 0x01892400u) && rsx_wbs_has(l, n, 0x16D0400u));
    CHECK(!rsx_wbs_has(l, n, 0x01310400u) && !rsx_wbs_has(l, 0, 0x01892400u));   /* idx0 must never be skipped by an idx3 list */
}

static rsx_wbp P;
static void test_slack(void)
{
    rsx_wbp_init(&P);
    rsx_wbp_cycle c;
    const uint32_t S0 = 0x01310400u, S3 = 0x01892400u, SZ3 = 640u * 360u * 4u;
    /* a GET before any store is ignored */
    rsx_wbp_get(&P, S0, 256, MS(1), 7, 0x100);
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 10, MS(10), MS(0), &c) && !c.valid);
    /* GET of a neighbour range does not count; an overlapping one does */
    rsx_wbp_get(&P, S0 + 0x384000u, 64, MS(12), 7, 0x100);
    rsx_wbp_get(&P, S0 + 0x1000u, 256, MS(25), 5, 0x2A0);
    rsx_wbp_get(&P, S0 + 0x2000u, 256, MS(30), 5, 0x2B0);
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 11, MS(50), MS(40), &c) && c.valid);   /* closes cycle 1 */
    CHECK(c.frame == 10 && c.gets == 2 && c.dirty_gets == 2 && c.has_get && !c.has_put);
    CHECK(near(c.store_ms, 10.0) && near(c.first_get_ms, 25.0) && near(c.slack_ms, 15.0));
    CHECK(c.img == 5 && c.pc == 0x2A0);                       /* the FIRST get's job, not the last */
    /* cycle 2: never read -> slack not sampled */
    rsx_wbp_get(&P, S0 + 8, 16, MS(52), 1, 0x10);             /* cycle 2 first get at 2 ms after the store */
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 12, MS(90), MS(80), &c) && c.valid && near(c.slack_ms, 2.0));
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 13, MS(130), MS(120), &c) && c.valid && !c.has_get && c.gets == 0);
    uint32_t off, cyc, rd; double med, mn, mx;
    CHECK(rsx_wbp_summary(&P, 0, &off, &cyc, &rd, &med, &mn, &mx));
    CHECK(off == S0 && cyc == 3 && rd == 2 && near(mn, 2.0) && near(mx, 15.0) && near(med, 8.5));
    CHECK(rsx_wbp_summary(&P, 0, &off, &cyc, &rd, &med, &mn, &mx) == 0);   /* reset by the first call */
    CHECK(!rsx_wbp_summary(&P, 3, &off, &cyc, &rd, &med, &mn, &mx));       /* unused slot */

    /* idx3 zero recognition: GETs before the zero PUTs are dirty, GETs after are not */
    CHECK(rsx_wbp_store(&P, S3, SZ3, 20, MS(200), MS(190), &c));
    rsx_wbp_get(&P, S3 + 640, 128, MS(201), 2, 0x40);         /* before the zero: dirty */
    rsx_wbp_put(&P, S3, 0x10000u, MS(205));                   /* the Zero job writes 64 KB per job in line-sized PUTs */
    rsx_wbp_get(&P, S3 + 640, 128, MS(206), 2, 0x40);         /* inside the zeroed span: clean */
    rsx_wbp_get(&P, S3 + 0x10000u - 128u, 128, MS(206), 2, 0x40);   /* last zeroed line: clean */
    rsx_wbp_get(&P, S3 + 0x10000u - 64u, 128, MS(207), 2, 0x40);    /* straddles into unzeroed bytes: dirty */
    rsx_wbp_get(&P, S3 + 64u, 128, MS(207), 2, 0x40);               /* starts mid-line: line 0 covered, line 1 covered: clean */
    rsx_wbp_put(&P, S3 + 0x10000u + 64u, 0x10000u, MS(208));        /* unaligned PUT covers whole lines only from +0x10080 on */
    rsx_wbp_get(&P, S3 + 0x10000u + 64u, 64u, MS(209), 2, 0x40);    /* the half line at +0x10040 is not covered: dirty */
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 14, MS(250), MS(240), &c));
    CHECK(rsx_wbp_store(&P, S3, SZ3, 21, MS(251), MS(240), &c) && c.valid);
    CHECK(c.gets == 6 && c.dirty_gets == 3 && c.puts == 2 && c.has_put && near(c.first_put_ms - c.store_ms, 5.0));
    /* walker binds: first one is recorded, neighbours and unstored surfaces do not count */
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 30, MS(300), MS(290), &c));
    rsx_wbp_walker(&P, S0 + 0x384000u, 1, MS(301));
    rsx_wbp_walker(&P, S0, 1, MS(310)); rsx_wbp_walker(&P, S0, 1, MS(320));
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 31, MS(350), MS(340), &c) && c.has_walker && c.walker_binds == 2 && near(c.first_walker_ms, 20.0));
    CHECK(rsx_wbp_store(&P, S0, 0x384000u, 32, MS(390), MS(380), &c) && !c.has_walker && c.walker_binds == 0);
    /* submit / 0xEB00 gap: store done 35 ms, batch started 20 ms (wait 15 ms), user command at 60 ms -> gap 40 ms; the first user command wins */
    CHECK(rsx_wbp_store(&P, S3, SZ3, 40, MS(435), MS(400), &c));
    rsx_wbp_set_submit(&P, S3, MS(420));
    rsx_wbp_usercmd(&P, MS(460)); rsx_wbp_usercmd(&P, MS(480));
    CHECK(rsx_wbp_store(&P, S3, SZ3, 41, MS(500), MS(480), &c) && c.has_submit && c.has_usercmd);
    CHECK(near(c.wait_ms, 15.0) && near(c.gap_ms, 40.0) && near(c.usercmd_ms, 60.0) && near(c.submit_ms, 20.0));
    CHECK(rsx_wbp_store(&P, S3, SZ3, 42, MS(540), MS(520), &c) && !c.has_usercmd && !c.has_submit);   /* a cycle with neither reports neither */
    /* a surface too large for the table, a zero offset and a full table are refused */
    CHECK(!rsx_wbp_store(&P, 0x02000000u, 0x400000u, 1, MS(1), 0, &c));
    CHECK(!rsx_wbp_store(&P, 0, 16, 1, MS(1), 0, &c));
    for (uint32_t i = 0; i < RSX_WBP_SLOTS; i++) rsx_wbp_store(&P, 0x100000u + i * 0x10000u, 256, 1, MS(1), 0, &c);
    CHECK(!rsx_wbp_store(&P, 0x300000u, 256, 1, MS(1), 0, &c));
}

static void test_hash(void)
{
    static uint8_t b[0x384000u], d[0x384000u];
    for (uint32_t i = 0; i < sizeof b; i++) b[i] = (uint8_t)(i * 2654435761u >> 24);
    memcpy(d, b, sizeof d);
    const uint64_t h = rsx_wbp_hash64(b, sizeof b);
    CHECK(h == rsx_wbp_hash64(d, sizeof d));
    static const uint32_t pos[] = { 0, 1, 7, 8, 4095, 0x1C0000u, 0x383FF8u, 0x383FFFu };   /* head, word edges, mid, last word */
    for (unsigned k = 0; k < sizeof pos / sizeof pos[0]; k++) {
        d[pos[k]] ^= 0x01; CHECK(rsx_wbp_hash64(d, sizeof d) != h); d[pos[k]] ^= 0x01;
    }
    CHECK(rsx_wbp_hash64(b, 57600) != rsx_wbp_hash64(b, 57601));   /* size and an odd tail both count */
    { const uint32_t n = 57603u; const uint64_t t0 = rsx_wbp_hash64(b, n); d[n - 1u] ^= 0x80; CHECK(rsx_wbp_hash64(d, n) != t0); d[n - 1u] ^= 0x80; }   /* tail bytes count */
    CHECK(rsx_wbp_hash64(b, 0) == rsx_wbp_hash64(d, 0));
}

static rsx_wbp R;
static void* getter(void* a)
{
    (void)a;
    for (uint32_t i = 1; i < 200000; i++) { rsx_wbp_get(&R, 0x1000u + (i & 0x3FFu) * 64u, 128, (uint64_t)i * 10u, 1, i); rsx_wbp_put(&R, 0x1000u + (i & 0x3FFu) * 128u, 128, (uint64_t)i * 10u); }
    return NULL;
}
static void test_race(void)
{
    rsx_wbp_init(&R);
    pthread_t t[2]; rsx_wbp_cycle c; uint32_t closed = 0;
    for (int k = 0; k < 2; k++) pthread_create(&t[k], NULL, getter, NULL);
    for (uint32_t i = 0; i < 20000; i++) { rsx_wbp_store(&R, 0x1000u, 0x10000u, i, (uint64_t)i * 100u, 0, &c); closed += c.valid; }
    for (int k = 0; k < 2; k++) pthread_join(t[k], NULL);
    CHECK(closed == 19999);
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "race")) { test_race(); }
    else { test_skip(); test_slack(); test_hash(); }
    printf("[test_wb_probe] %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
