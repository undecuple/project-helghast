/* Host test for KZ3 patch 2211: libs/video/rsx_wait_split.h ([perf-split-w]). A walker pass is replayed on a virtual clock through the
 * real rsx_split stack (rsx_o0.h) with fence waits charged to X_WAITO / RBWAIT exactly as rsx_live_draw.c does, the per-site wait
 * counters are filled the way ld_wait_fence_s does, and the printed line must (1) have sum == walk (diff 0), (2) have no gap
 * between the site waits and the wait_other + rbwait buckets, (3) fit the 640-byte line buffer of cellGcmSys.c, (4) carry
 * wb_sync and gpuwait next to each other. Negative controls: --negctl-gap (a wait no bucket covers) and --negctl-window (the
 * buckets read from another window than the walk) must FAIL; --negctl-site (a site counter dropped) must FAIL. Pure C. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "rsx_wait_split.h"
rsx_o0 g_rsx_o0;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static long long vclk;   /* us */
int main(int argc, char** argv)
{
    const int gap_nc = argc > 1 && !strcmp(argv[1], "--negctl-gap"), win_nc = argc > 1 && !strcmp(argv[1], "--negctl-window"),
              site_nc = argc > 1 && !strcmp(argv[1], "--negctl-site");
    /* 20 flips; per flip (us): decode 3600, tex 10700, rec 4500, park 4650, draw_rest 880, wbdrain wait 6900 (in X_WAITO), a
     * present-slot wait 100 (X_WAITO), a readback flush wait 290 (RBWAIT), and the rest of the walk 300 (OTHER). */
    const int flips = 20;
    long long wait_us[RSX_WS_N] = {0}; long long wait_n[RSX_WS_N] = {0};
    rsx_split s; rsx_split_begin(&s, vclk);
    for (int f = 0; f < flips; f++) {
        int t;
        t = rsx_split_push(&s, RSX_SB_DECODE_D, vclk); vclk += 3600; rsx_split_pop(&s, t, vclk);
        t = rsx_split_push(&s, RSX_SB_TEX, vclk); vclk += 10700; rsx_split_pop(&s, t, vclk);
        t = rsx_split_push(&s, RSX_SB_REC, vclk); vclk += 4500; rsx_split_pop(&s, t, vclk);
        t = rsx_split_push(&s, RSX_SB_X_DRAW, vclk); vclk += 880; rsx_split_pop(&s, t, vclk);
        /* the write-back sync: WB bucket, a fence wait inside it goes to X_WAITO (innermost wins), as in the game */
        t = rsx_split_push(&s, RSX_SB_WB, vclk); vclk += 400;
        { int w = rsx_split_push(&s, RSX_SB_X_WAITO, vclk); vclk += 6900; rsx_split_pop(&s, w, vclk); wait_us[RSX_WS_WBD_DRAIN] += 6900; wait_n[RSX_WS_WBD_DRAIN]++; }
        rsx_split_pop(&s, t, vclk);
        t = rsx_split_push(&s, RSX_SB_X_WAITO, vclk); vclk += 100; rsx_split_pop(&s, t, vclk); wait_us[RSX_WS_PRESENT_SLOT] += 100; wait_n[RSX_WS_PRESENT_SLOT]++;
        t = rsx_split_push(&s, RSX_SB_RBWAIT, vclk); vclk += 290; rsx_split_pop(&s, t, vclk); wait_us[RSX_WS_FLUSH_RB] += 290; wait_n[RSX_WS_FLUSH_RB]++;
        if (gap_nc) { vclk += 5000; wait_us[RSX_WS_IDLE] += 5000; wait_n[RSX_WS_IDLE]++; }   /* a wait counted by site but charged to no wait bucket */
        t = rsx_split_push(&s, RSX_SB_PARK, vclk); vclk += 4650; rsx_split_pop(&s, t, vclk);
        vclk += 300;   /* OTHER */
    }
    rsx_split_end(&s, vclk);
    double b[RSX_SB_N], ws_ms[RSX_WS_N], ws_cnt[RSX_WS_N];
    const double us2ms = 1.0 / 1000.0 / flips;
    for (int i = 0; i < RSX_SB_N; i++) b[i] = (double)s.acc[i] * us2ms;
    if (win_nc) b[RSX_SB_X_WAITO] += 3.0;   /* read from another window */
    for (int i = 0; i < RSX_WS_N; i++) { ws_ms[i] = (double)wait_us[i] * us2ms; ws_cnt[i] = (double)wait_n[i] / flips; }
    if (site_nc) ws_ms[RSX_WS_WBD_DRAIN] = 0;   /* the drain site was not counted */
    const double walk = (double)rsx_split_total(&s) * us2ms;
    char ln[640];
    const int o = rsx_ws_fmt(ln, sizeof ln, 160, flips, ws_ms, ws_cnt, b, walk, 7.4, 8.1, 26.6, 9.7);
    printf("%s\n", ln);
    CHECK(o > 0 && o < (int)sizeof ln - 1, "line length %d does not fit 640", o);
    double sites = 0, wo = b[RSX_SB_X_WAITO], rb = b[RSX_SB_RBWAIT];
    for (int i = 0; i < RSX_WS_N; i++) sites += ws_ms[i];
    CHECK(fabs(walk - (rsx_split_named_sum(b) + (walk - rsx_split_named_sum(b)))) < 1e-9, "arith");
    /* parse sum/walk/diff/gap back out of the printed line */
    double sum = -1, w2 = -1, diff = 99, gap = 99;
    const char* p;
    CHECK((p = strstr(ln, " sum=")) && sscanf(p, " sum=%lf walk=%lf diff=%lf", &sum, &w2, &diff) == 3, "sum/walk/diff missing");
    CHECK(fabs(diff) < 0.01 && fabs(sum - w2) < 0.01, "[perf-split-w] buckets do not add up to the walk: sum %.2f walk %.2f diff %.2f", sum, w2, diff);
    CHECK((p = strstr(ln, " gap=")) && sscanf(p, " gap=%lf", &gap) == 1, "gap missing");
    CHECK(fabs(gap) < 0.01, "a fence wait outside the wait buckets: gap %.2f (sites %.2f, wait_other %.2f, rbwait %.2f)", gap, sites, wo, rb);
    CHECK(strstr(ln, " wbdrain=6.90/1.00") != NULL, "wbdrain site missing or wrong (want 6.90 ms in 1.00 waits per flip)");
    CHECK(strstr(ln, " gpuwait=7.40 wb_sync=8.10") != NULL, "gpuwait and wb_sync not printed side by side");
    CHECK(strstr(ln, " unwait=1.") != NULL || strstr(ln, " unwait=0.") != NULL, "unwait missing");
    CHECK(strstr(ln, " gpu=26.60 lists=9.70") != NULL, "gpu/lists missing");
    if (fails) { printf("[test_wait_split] %d check(s) failed\n", fails); return 1; }
    printf("[test_wait_split] ok\n");
    return 0;
}
