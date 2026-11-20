/* Host test for KZ3 patch 2172: the LD_FRONT_PROBE unit (libs/video/rsx_front_probe.h), GPU-free, with a fake clock.
 * Known inputs -> known counters and T; negative controls (zero spans, a bump counted once not per draw, a naive per-draw
 * count that must differ, gates at their thresholds, SYNC_BGS split by the bit). */
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t g_fake;
#define RSX_FP_CLOCK() (g_fake += 100u)   /* every read advances 100 ns: copies and scans get a nonzero, deterministic time */
#include "rsx_front_probe.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MS(x) ((uint64_t)((x) * 1e6))

static void draw(rsx_fp* p, uint64_t t_begin, uint64_t t_end) { rsx_fp_draw_begin(p, t_begin); rsx_fp_draw_end(p, t_end); }

/* 1. zero spans: a draw with nothing to snapshot gives zero everywhere */
static void t_zero(void)
{
    rsx_fp* p = rsx_fp_new(2, 0);
    draw(p, 0, 1000);
    rsx_fp_snap_tex(p, 1, 0x1000, 0, NULL);   /* len 0 = no span */
    rsx_fp_event(p, RSX_FP_EV_FLIP, 2000, 0);
    CHECK(p->tot.c.draws == 1 && p->tot.c.tex_b == 0 && p->tot.c.tex_n == 0 && p->tot.c.texb_n == 0 && p->tot.c.vtx_b == 0 &&
          p->tot.c.idx_b == 0 && p->tot.c.fp_b == 0 && p->tot.c.rep == 0, "zero spans must count nothing (draws %llu tex_b %llu)",
          (unsigned long long)p->tot.c.draws, (unsigned long long)p->tot.c.tex_b);
    rsx_fp_free(p);
}

/* 2. generation rule: a planted bump is counted once, not per draw; a naive per-draw count differs */
static void t_gen(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    const uint32_t off = 0x40000, len = 0x10000;                      /* 64 KB = 16 pages */
    for (int d = 0; d < 3; d++) { rsx_fp_snap_tex(p, 1, off, len, NULL); }   /* first sight: one snapshot */
    CHECK(p->iv.tex_b == len && p->iv.texb_b == 3u * len, "first sight: tex_b %llu (want %u) bound %llu", (unsigned long long)p->iv.tex_b, len, (unsigned long long)p->iv.texb_b);
    for (int d = 0; d < 3; d++) rsx_fp_snap_tex(p, 1, off, len, NULL);
    CHECK(p->iv.tex_b == len, "no write: still %u, got %llu", len, (unsigned long long)p->iv.tex_b);
    rsx_fp_note_write(p, 1, off + 0x8000, 16);                         /* one 4 KB page inside the span */
    for (int d = 0; d < 4; d++) rsx_fp_snap_tex(p, 1, off, len, NULL);
    CHECK(p->iv.tex_b == 2u * len, "one bump, 4 draws: moved once (got %llu, want %u)", (unsigned long long)p->iv.tex_b, 2u * len);
    CHECK(p->iv.texb_n == 10, "bound count is per draw: %llu", (unsigned long long)p->iv.texb_n);
    CHECK(p->iv.tex_b / len != p->iv.texb_n, "naive per-draw count (10) must differ from the generation rule (2)");
    rsx_fp_note_write(p, 1, off + len, 64);                            /* the page after the span: no move */
    rsx_fp_note_write(p, 0, off, len);                                 /* other location: no move */
    rsx_fp_snap_tex(p, 1, off, len, NULL);
    CHECK(p->iv.tex_b == 2u * len, "writes outside the span / other location must not move it (got %llu)", (unsigned long long)p->iv.tex_b);
    rsx_fp_note_write(p, 1, off + len - 1u, 2);                        /* straddles the last byte of the span: moves it */
    rsx_fp_snap_tex(p, 1, off, len, NULL);
    CHECK(p->iv.tex_b == 3u * len, "a write over the span's last byte moves it (got %llu)", (unsigned long long)p->iv.tex_b);
    rsx_fp_note_write(p, 1, off - 1u, 2);                              /* ... and over the first byte, from below */
    rsx_fp_snap_tex(p, 1, off, len, NULL);
    CHECK(p->iv.tex_b == 4u * len, "a write over the span's first byte moves it (got %llu)", (unsigned long long)p->iv.tex_b);
    CHECK(p->iv.tex_n == 1, "distinct spans per flip counts the span once (%llu)", (unsigned long long)p->iv.tex_n);
    /* a span written before its first snapshot is still one first-sight snapshot */
    rsx_fp_note_write(p, 1, 0x900000, 0x2000);
    const uint64_t b0 = p->iv.tex_b;
    rsx_fp_snap_tex(p, 1, 0x900000, 0x2000, NULL); rsx_fp_snap_tex(p, 1, 0x900000, 0x2000, NULL);
    CHECK(p->iv.tex_b == b0 + 0x2000, "pre-written span: one snapshot (got +%llu)", (unsigned long long)(p->iv.tex_b - b0));
    rsx_fp_free(p);
}

/* 3. FP memcmp model */
static void t_fp(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    uint8_t a[64], b[64]; memset(a, 1, sizeof a); memcpy(b, a, sizeof b); b[63] = 9;
    rsx_fp_snap_fp(p, 1, 0x100, 64, a); rsx_fp_snap_fp(p, 1, 0x100, 64, a);
    CHECK(p->iv.fp_n == 2 && p->iv.fp_miss == 1 && p->iv.fp_b == 64, "FP: first sight misses, repeat hits (miss %llu)", (unsigned long long)p->iv.fp_miss);
    rsx_fp_snap_fp(p, 1, 0x100, 64, b);
    CHECK(p->iv.fp_miss == 2, "FP: one changed byte at the end misses (miss %llu)", (unsigned long long)p->iv.fp_miss);
    rsx_fp_snap_fp(p, 1, 0x100, 32, b);
    CHECK(p->iv.fp_miss == 3, "FP: same offset, shorter size misses");
    rsx_fp_snap_fp(p, 0, 0x100, 64, b);
    CHECK(p->iv.fp_miss == 4, "FP: same offset in the other location is another key");
    rsx_fp_free(p);
}

/* 4. SYNC_BGS split by the drawn_since_wb bit, SYNC_WB, FLIP */
static void t_bgs(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    rsx_fp_event(p, RSX_FP_EV_BGS, 100, 0);                           /* bit clear: ordering only */
    rsx_fp_event(p, RSX_FP_EV_BGS, 200, 1);                           /* bit clear but the recorder found dirty: the bit was not conservative */
    CHECK(p->iv.bgs_ord == 2 && p->iv.bgs_hard == 0 && p->iv.bgs_missed == 1 && p->fl_c.ivs == 0, "bit clear: ordering only, no interval closed");
    rsx_fp_draw_rt(p, 0); draw(p, 300, 400);                           /* a draw on a non-main RT leaves the bit clear */
    rsx_fp_event(p, RSX_FP_EV_BGS, 500, 0);
    CHECK(p->iv.bgs_ord == 3 && p->fl_c.ivs == 0, "non-main draw must not set the bit");
    rsx_fp_draw_rt(p, 1); draw(p, 600, 700);
    rsx_fp_event(p, RSX_FP_EV_BGS, 800, 1);                           /* bit set: hard, closes the interval, clears the bit */
    CHECK(p->fl_c.bgs_hard == 1 && p->fl_c.ivs == 1 && p->drawn_since_wb == 0, "bit set: hard wait, bit cleared after");
    rsx_fp_event(p, RSX_FP_EV_BGS, 900, 0);
    CHECK(p->iv.bgs_ord == 1 && p->fl_c.ivs == 1, "bit cleared after the wait: next BGS is ordering again");
    rsx_fp_draw_rt(p, 1);
    rsx_fp_event(p, RSX_FP_EV_WB, 1000, 0);                           /* SYNC_WB is always hard and clears the bit */
    CHECK(p->fl_c.wb == 1 && p->fl_c.ivs == 2 && p->drawn_since_wb == 0, "SYNC_WB: hard, clears the bit");
    rsx_fp_draw_rt(p, 1);
    rsx_fp_event(p, RSX_FP_EV_FLIP, 1100, 0);
    CHECK(p->tot.c.flips == 1 && p->tot.c.ivs == 3 && p->tot.c.bgs_hard == 1 && p->tot.c.bgs_ord == 4 && p->tot.c.wb == 1,
          "totals: flips %llu ivs %llu hard %llu ord %llu wb %llu", (unsigned long long)p->tot.c.flips, (unsigned long long)p->tot.c.ivs,
          (unsigned long long)p->tot.c.bgs_hard, (unsigned long long)p->tot.c.bgs_ord, (unsigned long long)p->tot.c.wb);
    rsx_fp_free(p);
}

/* 5. report classes and the state-lag model */
static void t_report(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    rsx_fp_pub_begin(p);
    rsx_fp_pub_set(p, 5, 0x40, 3 /* PENDING */, 0);                    /* mirror published at the last hard point */
    rsx_fp_pub_set(p, 6, 0x50, 2 /* NEVER */, 0);
    draw(p, 0, 10);
    rsx_fp_report(p, RSX_FP_RC_NOVAL, 5, 0x40, 3, 0);                  /* same state, same has_val: no lag */
    rsx_fp_report(p, RSX_FP_RC_VAL, 5, 0x40, 3, 1);                    /* has_val flipped since: val lag only */
    rsx_fp_report(p, RSX_FP_RC_NEVER, 6, 0x50, 3, 0);                  /* NEVER -> PENDING since: state lag */
    rsx_fp_report(p, RSX_FP_RC_VAL, 7, 0x60, 3, 1);                    /* slot created since the hard point: state lag (mirror has none) */
    rsx_fp_report(p, RSX_FP_RC_DFLT, -1, 0x70, 0, 0);                  /* no slot */
    CHECK(p->iv.rep == 5 && p->iv.rep_noval == 1 && p->iv.rep_val == 2 && p->iv.rep_never == 1 && p->iv.rep_dflt == 1, "classes");
    CHECK(p->iv.rep_lag_state == 2 && p->iv.rep_lag_val == 1, "lag state %llu (want 2) val %llu (want 1)", (unsigned long long)p->iv.rep_lag_state, (unsigned long long)p->iv.rep_lag_val);
    CHECK(p->iv_rep_first == 1 && p->iv_rep_last == 1, "report position = draws before it in the interval");
    rsx_fp_free(p);
    p = rsx_fp_new(100, 0);                                            /* control: nothing changed => lag 0 */
    rsx_fp_pub_begin(p); rsx_fp_pub_set(p, 1, 0x10, 3, 1);
    for (int i = 0; i < 100; i++) rsx_fp_report(p, RSX_FP_RC_VAL, 1, 0x10, 3, 1);
    CHECK(p->iv.rep == 100 && p->iv.rep_lag_state == 0 && p->iv.rep_lag_val == 0, "control: unchanged mirror gives lag 0");
    rsx_fp_free(p);
}

/* 6. T model with hand-worked numbers */
static void t_model(void)
{
    /* recorder is the bottleneck: draw 2..11 ms, snapshot 1 MB at 1 ns/B (1 ms) first, flip at 14 ms.
     * front_raw = 14 - 9 = 5; F = 6; R = 9; sim: ready 2+1 = 3, recorder 3..12 -> C = 12; drain = 12 - 9 = 3; T = 12. */
    rsx_fp* p = rsx_fp_new(2, 0);
    p->rates_known = 1; p->copy_rate = 1.0; p->scan_rate = 0.0;
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(1), 0);                         /* the sampled flip: excluded from the model */
    CHECK(p->win.tflips == 0, "sampled flip is not a model flip");
    p->t_start = 0;
    rsx_fp_draw_begin(p, MS(2));
    rsx_fp_snap_vtx(p, 1000000, NULL);
    rsx_fp_draw_end(p, MS(11));
    CHECK(rsx_fp_event(p, RSX_FP_EV_FLIP, MS(14), 0) == 1, "window due after 2 flips");
    rsx_fp_gates g = rsx_fp_gate(&p->win);
    CHECK(g.T_ms > 11.999 && g.T_ms < 12.001 && g.F_ms > 5.999 && g.F_ms < 6.001 && g.R_ms > 8.999 && g.R_ms < 9.001 && g.drain_ms > 2.999 && g.drain_ms < 3.001,
          "T %.3f F %.3f R %.3f drain %.3f (want 12 6 9 3)", g.T_ms, g.F_ms, g.R_ms, g.drain_ms);
    char line[3072]; rsx_fp_window_line(p, &p->win, "window", line, sizeof line);
    CHECK(strstr(line, "[front-probe] window flips 2 (model flips 1, rates measured") != NULL, "line prefix: %.120s", line);
    rsx_fp_free(p);
    /* front is the bottleneck: draw 5..6 ms (rec 1), flip at 14: front_raw 13, snap 1 ms -> F 14, R 1, drain 0, T 14 */
    p = rsx_fp_new(2, 0); p->rates_known = 1; p->copy_rate = 1.0;
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(1), 0); p->t_start = 0;
    rsx_fp_draw_begin(p, MS(5)); rsx_fp_snap_vtx(p, 1000000, NULL); rsx_fp_draw_end(p, MS(6));
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(14), 0);
    g = rsx_fp_gate(&p->win);
    CHECK(g.T_ms > 13.999 && g.T_ms < 14.001 && g.drain_ms < 0.001, "front-bound: T %.3f drain %.3f (want 14, 0)", g.T_ms, g.drain_ms);
    rsx_fp_free(p);
    /* a sync is recorder work after the draws: draw 0..2, sync 4 ms, flip at 7 -> front_raw = 7-2-4 = 1, R = 2+4 = 6, C = max(2,1)+4 = 6, T 6 */
    p = rsx_fp_new(2, 0);
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(1), 0); p->t_start = 0;
    rsx_fp_draw_begin(p, 0); rsx_fp_draw_end(p, MS(2)); rsx_fp_sync_time(p, MS(4));
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(7), 0);
    g = rsx_fp_gate(&p->win);
    CHECK(g.T_ms > 5.999 && g.T_ms < 6.001 && g.F_ms > 0.999 && g.F_ms < 1.001 && g.R_ms > 5.999 && g.R_ms < 6.001, "sync: T %.3f F %.3f R %.3f", g.T_ms, g.F_ms, g.R_ms);
    rsx_fp_free(p);
    /* present carry: the recorder starts the next interval busy with the previous present */
    p = rsx_fp_new(3, 0);
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(1), 0);                         /* sampled */
    rsx_fp_present_begin(p, MS(10)); rsx_fp_event(p, RSX_FP_EV_FLIP, MS(12), 0); rsx_fp_present_done(p, MS(14));   /* flip 2: interval closes at 10, present 10..14 */
    CHECK(p->carry_ns == MS(4) && p->t_start == MS(14), "present carry %llu t_start %llu", (unsigned long long)p->carry_ns, (unsigned long long)p->t_start);
    rsx_fp_event(p, RSX_FP_EV_FLIP, MS(16), 0);                        /* flip 3: front 2 ms, R = carry 4 -> T = 4, drain: C = max(4,2) = 4 -> 0 */
    g = rsx_fp_gate(&p->win);
    CHECK(g.T_ms > 6.499 && g.T_ms < 6.501, "T over flips 2 (9 ms, interval closed where the present starts) and 3 (4 ms = the carried present) averages %.3f (want 6.5)", g.T_ms);
    rsx_fp_free(p);
}

/* 7. gates flip at their thresholds (T > 35.2 ms; front+snap >= 22 ms; report lag > 0; TEX > 10 MiB per flip) */
static int gate_run(uint64_t wall_ns, uint64_t tex_bytes, int lag, rsx_fp_gates* out)
{
    rsx_fp* p = rsx_fp_new(3, 0);
    uint64_t t = 0;
    for (int f = 0; f < 3; f++) {
        if (tex_bytes) rsx_fp_snap_tex(p, 1, 0x1000000u * (uint32_t)(f + 1), (uint32_t)tex_bytes, NULL);
        if (f == 1 && lag) { rsx_fp_pub_begin(p); rsx_fp_report(p, RSX_FP_RC_VAL, 3, 0x10, 3, 1); }
        t += wall_ns;
        const int due = rsx_fp_event(p, RSX_FP_EV_FLIP, t, 0);
        if (f == 2 && !due) return -1;
    }
    *out = rsx_fp_gate(&p->win);
    rsx_fp_free(p);
    return 0;
}
static void t_gates(void)
{
    rsx_fp_gates g;
    gate_run(35199999ull, 0, 0, &g); CHECK(!g.t, "T 35.199999 ms: gate off");
    gate_run(35200000ull, 0, 0, &g); CHECK(!g.t, "T == 35.2 ms: gate off (strictly greater)");
    gate_run(35200001ull, 0, 0, &g); CHECK(g.t, "T 35.200001 ms: gate on");
    gate_run(21999999ull, 0, 0, &g); CHECK(!g.front, "front+snap 21.999999 ms: gate off");
    gate_run(22000000ull, 0, 0, &g); CHECK(g.front, "front+snap == 22 ms: gate on (>=)");
    gate_run(1000000ull, 0, 0, &g);  CHECK(!g.lag, "no lag: gate off");
    gate_run(1000000ull, 0, 1, &g);  CHECK(g.lag, "one state lag: gate on");
    gate_run(1000000ull, RSX_FP_TEX_GATE_BYTES, 0, &g);      CHECK(!g.tex, "TEX == 10 MiB/flip: gate off");
    gate_run(1000000ull, RSX_FP_TEX_GATE_BYTES + 3u, 0, &g); CHECK(g.tex, "TEX 10 MiB + 3 B per flip: gate on (3 flips x (L)/3 = L)");
    gate_run(1000000ull, 0, 0, &g);  CHECK(!g.t && !g.front && !g.lag && !g.tex, "control: all four gates off");
}

/* 8. async ref publications: draws before the last publication in the interval, once per interval */
static void t_cross(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    for (int i = 0; i < 2; i++) draw(p, 0, 1);
    rsx_fp_pub_note(p);
    for (int i = 0; i < 2; i++) draw(p, 0, 1);
    rsx_fp_event(p, RSX_FP_EV_WB, 100, 0);
    CHECK(p->fl_c.cross_d == 2 && p->fl_c.cross_iv == 1 && p->fl_c.pubs == 1, "2 of 4 draws precede the publication (cross %llu)", (unsigned long long)p->fl_c.cross_d);
    for (int i = 0; i < 3; i++) draw(p, 0, 1);
    rsx_fp_event(p, RSX_FP_EV_WB, 200, 0);
    CHECK(p->fl_c.cross_d == 2 && p->fl_c.cross_iv == 1, "an interval without a publication adds nothing (cross %llu)", (unsigned long long)p->fl_c.cross_d);
    draw(p, 0, 1); rsx_fp_pub_note(p); rsx_fp_pub_note(p);
    rsx_fp_event(p, RSX_FP_EV_WB, 300, 0);
    CHECK(p->fl_c.cross_d == 3 && p->fl_c.pubs == 3 && p->fl_c.cross_iv == 2, "two publications in one interval count the draw once (cross %llu pubs %llu)", (unsigned long long)p->fl_c.cross_d, (unsigned long long)p->fl_c.pubs);
    rsx_fp_free(p);
}

/* 9. sampled copies and the scan: only moved spans are copied, only on the sampled flip */
static void t_sample(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    static uint8_t buf[8192]; for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;
    rsx_fp_snap_tex(p, 1, 0x20000, sizeof buf, buf); rsx_fp_snap_tex(p, 1, 0x20000, sizeof buf, buf);
    CHECK(p->samp_cp_b == sizeof buf && p->samp_cp_ns > 0, "sampled flip: the moved span is copied once (%llu B)", (unsigned long long)p->samp_cp_b);
    rsx_fp_note_write(p, 1, 0x20000, 4);
    rsx_fp_snap_tex(p, 1, 0x20000, sizeof buf, buf);
    CHECK(p->samp_cp_b == 2u * sizeof buf, "a bump copies again (%llu B)", (unsigned long long)p->samp_cp_b);
    uint8_t idx[8] = { 0, 3, 0, 9, 0, 1, 0, 7 };
    const uint32_t range = rsx_fp_sample_scan(p, idx, 8, 2);
    CHECK(range == 8, "u16 BE scan: max 9 - min 1 = 8, got %u", range);
    p->sampling = 0;
    const uint64_t b = p->samp_cp_b;
    rsx_fp_note_write(p, 1, 0x20000, 4); rsx_fp_snap_tex(p, 1, 0x20000, sizeof buf, buf);
    CHECK(p->samp_cp_b == b, "not sampling: no copy is made");
    rsx_fp_free(p);
}

/* 10. write marks from another thread (run under -fsanitize=thread by the script) */
static void* writer(void* a) { rsx_fp* p = (rsx_fp*)a; for (uint32_t i = 0; i < 20000; i++) rsx_fp_note_write(p, 1, 0x10000u * (i & 7u), 0x3000); for (int i = 0; i < 100; i++) rsx_fp_pub_note(p); return NULL; }
static void t_thread(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    pthread_t th; pthread_create(&th, NULL, writer, p);
    uint64_t moved = 0;
    for (int i = 0; i < 20000; i++) { const uint64_t b = p->iv.tex_b; rsx_fp_snap_tex(p, 1, 0x10000u * (uint32_t)(i & 7), 0x3000, NULL); moved += p->iv.tex_b - b; }
    pthread_join(th, NULL);
    CHECK(moved >= 8u * 0x3000u, "every span was snapshotted at least once under concurrent marks (%llu)", (unsigned long long)moved);
    CHECK(__atomic_load_n(&p->pub_count, __ATOMIC_RELAXED) == 100, "100 publications");
    rsx_fp_free(p);
}

/* 11. pass 160: tables that must be correct for any number of distinct spans / programs / draws (the K4 run filled the old
 * 8192-entry span table: tex-table full 3,097,107 and every later span counted as moved). Default build: exact answers and
 * zero overflow counters. -DRSX_FP_NOGROW (the old fixed sizes, bounded): must show the WRONG answer (the negative control). */
#define MANY 40000u
static int t_many(void)
{
    rsx_fp* p = rsx_fp_new(100, 0);
    const uint32_t len = 0x1000;
    for (uint32_t i = 0; i < MANY; i++) rsx_fp_snap_tex(p, 1, i * len, len, NULL);
    const uint64_t first = p->iv.tex_b;
    for (uint32_t i = 0; i < MANY; i++) rsx_fp_snap_tex(p, 1, i * len, len, NULL);        /* no write since: nothing may move */
    const uint64_t second = p->iv.tex_b - first;
    uint32_t nw = 0;
    for (uint32_t i = 0; i < MANY; i += 7u) { rsx_fp_note_write(p, 1, i * len + 8u, 4); nw++; }
    const uint64_t b2 = p->iv.tex_b;
    for (uint32_t i = 0; i < MANY; i++) rsx_fp_snap_tex(p, 1, i * len, len, NULL);
    const uint64_t third = p->iv.tex_b - b2;
    /* FP programs above 512, same pattern */
    uint8_t prog[64]; memset(prog, 5, sizeof prog);
    const uint32_t NP = 3000;
    for (uint32_t i = 0; i < NP; i++) rsx_fp_snap_fp(p, 1, 0x100000u + i * 0x100u, 64, prog);
    const uint64_t m1 = p->iv.fp_miss;
    for (uint32_t i = 0; i < NP; i++) rsx_fp_snap_fp(p, 1, 0x100000u + i * 0x100u, 64, prog);
    const uint64_t m2 = p->iv.fp_miss - m1;
    uint8_t prog2[64]; memcpy(prog2, prog, 64); prog2[7] = 9; uint32_t nc = 0;
    for (uint32_t i = 0; i < NP; i += 10u) { rsx_fp_snap_fp(p, 1, 0x100000u + i * 0x100u, 64, prog2); nc++; }
    const uint64_t m3 = p->iv.fp_miss - m1 - m2;
    /* more draws than the old 16384 rows */
    for (uint32_t i = 0; i < 40000u; i++) draw(p, 1000u + i * 10u, 1005u + i * 10u);
    const uint32_t exact = (uint64_t)MANY * len == first && second == 0 && third == (uint64_t)nw * len &&
                           m1 == NP && m2 == 0 && m3 == nc && p->n_dr == 40000u;
#ifndef RSX_FP_NOGROW
    CHECK(first == (uint64_t)MANY * len, "first pass: every span moves once (%llu want %llu)", (unsigned long long)first, (unsigned long long)MANY * len);
    CHECK(second == 0, "second pass, no writes: nothing moves (got %llu bytes)", (unsigned long long)second);
    CHECK(third == (uint64_t)nw * len, "after %u writes exactly those spans move (got %llu bytes, want %llu)", nw, (unsigned long long)third, (unsigned long long)nw * len);
    CHECK(m1 == NP && m2 == 0 && m3 == nc, "FP: first %llu (want %u), repeat %llu (want 0), changed %llu (want %u)", (unsigned long long)m1, NP, (unsigned long long)m2, (unsigned long long)m3, nc);
    CHECK(p->n_dr == 40000u && p->dr_over == 0 && p->dr_cap >= 40000u, "draw rows grow past 16384 (n %u cap %u over %llu)", p->n_dr, p->dr_cap, (unsigned long long)p->dr_over);
    CHECK(p->tab_full == 0 && p->fpt_full == 0 && p->tab_grows > 0 && p->fpt_grows > 0 && p->tab_n == MANY && p->fpt_n == NP,
          "no overflow, tables grew (tab full %llu grows %llu n %u; fpt full %llu grows %llu n %u)", (unsigned long long)p->tab_full,
          (unsigned long long)p->tab_grows, p->tab_n, (unsigned long long)p->fpt_full, (unsigned long long)p->fpt_grows, p->fpt_n);
    /* stamp coverage above 256 MiB per location: a write at 0x10005000 must not move the span at 0x5000 (the old 65536-page stamps aliased them) */
    rsx_fp_snap_tex(p, 1, 0x5000, 0x1000, NULL);
    const uint64_t b3 = p->iv.tex_b;
    rsx_fp_note_write(p, 1, 0x10005000u, 16);
    rsx_fp_snap_tex(p, 1, 0x5000, 0x1000, NULL);
    CHECK(p->iv.tex_b == b3, "a write 256 MiB above must not move the span (aliased stamp: +%llu)", (unsigned long long)(p->iv.tex_b - b3));
    rsx_fp_note_write(p, 1, 0xFFFFF000u, 0x2000);                       /* clamped at the top of the 32-bit space, no wrap onto page 0 */
    rsx_fp_snap_tex(p, 1, 0x5000, 0x1000, NULL);
    CHECK(p->iv.tex_b == b3, "a write at the top of the offset space must not move a low span");
    char cl[1200]; rsx_fp_cost_line(p, cl, sizeof cl);
    CHECK(strstr(cl, "overflow: tex-table full 0") && strstr(cl, "fp-table full 0") && strstr(cl, "draw-rows lost 0"), "cost line prints the overflow counters: %s", cl);
    (void)exact;
    rsx_fp_free(p);
    return 0;
#else
    printf("[test_front_probe] control (old fixed tables, RSX_FP_NOGROW): first %llu second %llu (want 0) third %llu (want %llu) fp miss %llu/%llu/%llu tab_full %llu fpt_full %llu\n",
           (unsigned long long)first, (unsigned long long)second, (unsigned long long)third, (unsigned long long)nw * len,
           (unsigned long long)m1, (unsigned long long)m2, (unsigned long long)m3, (unsigned long long)p->tab_full, (unsigned long long)p->fpt_full);
    CHECK(!exact && second != 0 && p->tab_full > 0 && p->fpt_full > 0, "negative control: the fixed tables must give the wrong answer (second pass moved %llu)", (unsigned long long)second);
    rsx_fp_free(p);
    return (int)exact;
#endif
}

int main(void)
{
#ifdef RSX_FP_NOGROW
    t_many();
    if (g_fail) { printf("[test_front_probe] control FAIL (%d)\n", g_fail); return 1; }
    printf("[test_front_probe] control PASS: the old fixed sizes give the wrong answer\n");
    return 0;
#endif
    t_zero(); t_gen(); t_fp(); t_bgs(); t_report(); t_model(); t_gates(); t_cross(); t_sample(); t_thread(); t_many();
    if (g_fail) { printf("[test_front_probe] FAIL (%d)\n", g_fail); return 1; }
    printf("[test_front_probe] unit PASS\n");
    return 0;
}
