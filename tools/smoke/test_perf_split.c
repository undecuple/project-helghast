/* Host test for KZ3 patch 2188: the exclusive split core, the [perf-split] formatter and the O0 counters (libs/video/rsx_o0.h).
 * Pure C, no game data. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_o0.h"
rsx_o0 g_rsx_o0;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_core(void)
{
    rsx_split s = {0}; rsx_split_begin(&s, 1000);
    rsx_split_base(&s, RSX_SB_DECODE_F, 1100);               /* 100 other */
    int a = rsx_split_push(&s, RSX_SB_X_DRAW, 1200);         /* 100 decode_f */
    int b = rsx_split_push(&s, RSX_SB_TEX, 1250);            /* 50 draw_rest */
    int c = rsx_split_push(&s, RSX_SB_WB, 1260);             /* 10 tex */
    int d = rsx_split_push(&s, RSX_SB_RBWAIT, 1300);         /* 40 wb */
    rsx_split_pop(&s, d, 1400);                              /* 100 rbwait */
    rsx_split_pop(&s, c, 1410);                              /* 10 wb */
    rsx_split_pop(&s, b, 1500);                              /* 90 tex */
    int e = rsx_split_push(&s, RSX_SB_PSO, 1510);            /* 10 draw_rest */
    (void)e;
    rsx_split_pop(&s, a, 1600);                              /* early return: pso never popped; closes it and the draw: 90 pso */
    CHECK(s.unwound == 1, "unwound %llu", s.unwound);
    CHECK(s.sp == 0 && s.cur == RSX_SB_DECODE_F, "back to base");
    rsx_split_pop(&s, a, 1700);                              /* a stale token is ignored */
    CHECK(s.acc[RSX_SB_DECODE_F] == 100, "decode_f charged by a stale pop: %lld", s.acc[RSX_SB_DECODE_F]);
    rsx_split_end(&s, 1800);                                 /* 200 decode_f */
    CHECK(s.acc[RSX_SB_OTHER] == 100 && s.acc[RSX_SB_DECODE_F] == 300, "other %lld f %lld", s.acc[RSX_SB_OTHER], s.acc[RSX_SB_DECODE_F]);
    CHECK(s.acc[RSX_SB_X_DRAW] == 60 && s.acc[RSX_SB_TEX] == 100 && s.acc[RSX_SB_WB] == 50 && s.acc[RSX_SB_RBWAIT] == 100 && s.acc[RSX_SB_PSO] == 90,
          "draw %lld tex %lld wb %lld rb %lld pso %lld", s.acc[RSX_SB_X_DRAW], s.acc[RSX_SB_TEX], s.acc[RSX_SB_WB], s.acc[RSX_SB_RBWAIT], s.acc[RSX_SB_PSO]);
    CHECK(rsx_split_total(&s) == 800, "buckets must add up to walk (800), got %lld", rsx_split_total(&s));
    /* overflow is counted and the token is 0 */
    rsx_split_begin(&s, 0);
    int last = 0; for (int i = 0; i < RSX_SPLIT_DEPTH + 3; i++) last = rsx_split_push(&s, RSX_SB_TEX, i);
    CHECK(last == 0 && s.overflow == 3, "overflow %llu", s.overflow);
}

static void t_line(void)
{
    /* random passes: sum of the printed buckets equals walk within rounding */
    srand(7);
    for (int trial = 0; trial < 200; trial++) {
        rsx_split s = {0}; long long now = 5000; rsx_split_begin(&s, now);
        rsx_split_base(&s, RSX_SB_DECODE_F, now += rand() % 50);
        int tok[8], n = 0;
        for (int i = 0; i < 300; i++) {
            now += rand() % 500;
            if (n && rand() % 2) rsx_split_pop(&s, tok[--n], now);
            else if (n < 7) tok[n++] = rsx_split_push(&s, 1 + rand() % (RSX_SB_N - 1), now);
        }
        rsx_split_end(&s, now += 17);
        const long long flips = 1 + rand() % 3; const double tk = 1000.0 / 10000000.0;
        double b[RSX_SB_N]; for (int i = 0; i < RSX_SB_N; i++) b[i] = (double)s.acc[i] * tk / (double)flips;
        const double walk = (double)rsx_split_total(&s) * tk / (double)flips;
        char ln[800]; rsx_split_fmt(ln, sizeof ln, "perf-split", 12, flips, 33.3, walk, b, -1.0, -1.0);
        double v[16]; const char* keys[] = {"decode=", "tex=", "pso=", "desc=", "vplan=", "fill=", "rec=", "wb=", "rbwait=", "submit=", "park=", "other="};
        double sum = 0, w = -1;
        for (int k = 0; k < 12; k++) { const char* p = strstr(ln, keys[k]); CHECK(p, "key %s missing in %s", keys[k], ln); if (!p) return; v[k] = atof(p + strlen(keys[k])); sum += v[k]; }
        w = atof(strstr(ln, "walk=") + 5);
        CHECK(fabs(sum - w) < 0.07, "trial %d: sum %.3f != walk %.3f\n%s", trial, sum, w, ln);
        const double df = b[RSX_SB_DECODE_F] + b[RSX_SB_DECODE_D];
        CHECK(fabs(v[0] - df) < 0.006, "decode = decode_f + decode_d");
        CHECK(fabs(v[11] - (walk - rsx_split_named_sum(b))) < 0.006, "other = walk - named");
    }
    double b[RSX_SB_N] = {0}; b[RSX_SB_DECODE_F] = 1.5; b[RSX_SB_DECODE_D] = 2.25; b[RSX_SB_TEX] = 3; b[RSX_SB_X_DRAW] = 4;
    char ln[800];
    rsx_split_fmt(ln, sizeof ln, "perf-split", 120, 30, 42.8, 11.0, b, -1.0, -1.0);
    const char* want = "[perf-split] t=120 flips=30 frame=42.80 walk=11.00 decode=3.75 tex=3.00 pso=0.00 desc=0.00 vplan=0.00 fill=0.00 rec=0.00"
                       " wb=0.00 rbwait=0.00 submit=0.00 park=0.00 other=4.25 gpu=-1.00";
    CHECK(!strcmp(ln, want), "line format changed:\n%s\n%s", ln, want);
    rsx_split_fmt(ln, sizeof ln, "perf-split-w", 120, 30, 42.8, 11.0, b, 6.5, 0.75);
    CHECK(strstr(ln, "[perf-split-w] ") == ln && strstr(ln, " gpu=6.50 bar=0.75") && !strchr(strstr(ln, "bar="), ' '), "worker line: %s", ln);
    rsx_split_fmt_d(ln, sizeof ln, 120, b); CHECK(!strcmp(ln, "[perf-split-d] t=120 decode_f=1.50 decode_d=2.25"), "%s", ln);
    char tiny[20]; int r = rsx_split_fmt(tiny, sizeof tiny, "perf-split", 1, 1, 1, 1, b, 0, -1); CHECK(r > 20 && tiny[19] == 0, "truncation is safe");
}

static void t_o0(void)
{
    static rsx_o0 o, p0, p1; memset(&o, 0, sizeof o);
    /* 0x4B: pass starts at t=100 (put 0x200) and t=300 (put 0x900); the command at 0x500 is first covered by the pass at t=300 */
    rsx_o0_pass(&o, 0x200, 100); rsx_o0_pass(&o, 0x900, 300);
    rsx_o0_q4b(&o, 0x500, 0x508, 0x900, 350);   /* backlog 0x3F8, lag 50 */
    rsx_o0_q4b(&o, 0x600, 0x608, 0x608, 450);   /* idle, lag 150 */
    CHECK(o.q_n == 2 && o.q_idle == 1 && o.q_backlog_sum == 0x3F8 && o.q_lag_n == 2 && o.q_lag_sum == 200 && o.q_lag_max == 150, "q4b %lld %lld %lld %lld", o.q_n, o.q_idle, o.q_backlog_sum, o.q_lag_sum);
    CHECK(o.q_per_n == 1 && o.q_per_sum == 100, "period");
    rsx_o0_q4b(&o, 0x700, 0x708, 0x10, 500);    /* put wrapped behind get: counted, no backlog */
    CHECK(o.q_unk == 1, "put_behind");
    /* textures */
    for (int i = 0; i < 5; i++) { rsx_o0_tex_bind(&o, 1, 0x1000); rsx_o0_tex_hash(&o, 1, 0x1000, i == 0, i < 2); }
    rsx_o0_tex_bind(&o, 0, 0x40); rsx_o0_tex_hash(&o, 0, 0x40, 0, 0);
    CHECK(o.t_binds[1] == 5 && o.t_hashed[1] == 5 && o.t_changed[1] == 1 && o.t_written[1][1] == 1 && o.t_written[1][0] == 1, "tex totals");
    char ln[512]; rsx_o0_tsrc_top(&o, ln, sizeof ln, 5, 8);
    CHECK(strstr(ln, " 1:0x00001000=5/1/2") && strstr(ln, " 0:0x00000040=1/0/0"), "top: %s", ln);
    /* write-back overlap, idx3 */
    o.flip_n = 10; rsx_o0_wb_store(&o, 1, 0x01892000, 0x1000); rsx_o0_wb_store(&o, 1, 0x01300000, 0x1000);
    CHECK(o.wb_idx3 == 1, "idx3 store");
    o.flip_n = 11;
    rsx_o0_read(&o, RSX_O0_RK_RENDERER, 1, 0x01892800, 0x100);   /* overlaps idx3 store, age 1 */
    rsx_o0_read(&o, RSX_O0_RK_RENDERER, 0, 0x01892800, 0x100);   /* other location: no overlap */
    rsx_o0_read(&o, RSX_O0_RK_NV0039, 1, 0x01892000, 0x10);      /* overlaps the idx3 store too */
    rsx_o0_read(&o, RSX_O0_RK_NV0039, 1, 0x01500000, 0x10);
    CHECK(o.rd_n[0] == 2 && o.rd_ov[0] == 1 && o.rd_ov_age[0][1] == 1 && o.rd_idx3[0] == 1 && o.rd_ov[1] == 1 && o.rd_idx3[1] == 1, "overlap counts");
    for (unsigned i = 0; i < RSX_O0_WBR; i++) rsx_o0_wb_store(&o, 0, 0x10 * i, 4);   /* recycles the first slots */
    CHECK(o.wb_stores_read >= 1 && o.wb_stores_never >= 1, "slot reuse: read %lld never %lld", o.wb_stores_read, o.wb_stores_never);
    memset(&p0, 0, sizeof p0); memcpy(&p1, &o, sizeof p1);
    rsx_o0_fmt_4b(ln, sizeof ln, 9, 0.0001, &p1, &p0); CHECK(strstr(ln, "[o0-4b] t=9 writes=3 idle_at_put=1 idle_frac=0.33"), "%s", ln);
    rsx_o0_fmt_wbov(ln, sizeof ln, 9, &p1, &p0); CHECK(strstr(ln, "idx3_stores=1 idx3_read_by_renderer=1 idx3_read_by_nv0039=1"), "%s", ln);
    rsx_o0_fmt_state(ln, sizeof ln, 9, &p1, &p0); rsx_o0_fmt_ref(ln, sizeof ln, 9, &p1, &p0); rsx_o0_fmt_seg(ln, sizeof ln, 9, &p1, &p0);
    rsx_o0_fmt_tex(ln, sizeof ln, 9, &p1, &p0);
    CHECK(strstr(ln, "loc1 binds=5 hashed=5 changed=1 written_changed=1 written_same=1"), "%s", ln);
}

int main(void)
{
    t_core(); t_line(); t_o0();
    if (fails) { printf("[test_perf_split] %d FAILED\n", fails); return 1; }
    printf("[test_perf_split] ok\n");
    return 0;
}
