/* Host test for KZ3 patch 2209: libs/video/rsx_walk_trivial.h (RSX_WALK_TRIVIAL=1 core). Built and mutated by test_diag_probes.sh.
 * Prints "FAIL name" per failed check (the mutation controls grep for the names). */
#include <stdio.h>
#include <string.h>
#include "rsx_walk_trivial.h"
static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s (line %d)\n", name, __LINE__); fails++; } } while (0)
int main(void)
{
    rsx_wt w; char b[1024];
    rsx_wt_init(&w, NULL); CHECK("unset-off", !w.on);
    rsx_wt_init(&w, ""); CHECK("empty-off", !w.on);
    rsx_wt_init(&w, "0"); CHECK("zero-off", !w.on);
    rsx_wt_init(&w, "1"); CHECK("one-on", w.on);
    rsx_wt_init(&w, "0"); CHECK("tick-silent-when-off", rsx_wt_tick(&w, 10000, 1000, 0, 0, 0, 0, b, sizeof b) == 0 && rsx_wt_tick(&w, 99999, 1000, 0, 0, 0, 0, b, sizeof b) == 0);
    rsx_wt_init(&w, "1");
    /* arm line names the write-back policy */
    rsx_wt_arm_line(b, sizeof b);
    CHECK("arm-names-writebacks", strstr(b, "surf_wb") && strstr(b, "wbd_drain") && strstr(b, "report_rb"));
    CHECK("arm-says-not-zero-filled", strstr(b, "NOT zero-filled"));
    CHECK("arm-tag", !strncmp(b, "[walk-trivial] on", 17));
    /* first tick only starts the clock; one report per period of freq ticks; deltas, not totals */
    CHECK("first-tick-silent", rsx_wt_tick(&w, 1000, 1000, 0, 0, 0, 0, b, sizeof b) == 0);
    RSX_WT_ADD(&w, flips, 30); RSX_WT_ADD(&w, methods, 4000); RSX_WT_ADD(&w, draws, 900); RSX_WT_ADD(&w, clears, 20); RSX_WT_ADD(&w, wb_req, 7);
    CHECK("early-tick-silent", rsx_wt_tick(&w, 1999, 1000, 50, 5, 0, 12, b, sizeof b) == 0);
    CHECK("period-tick-prints", rsx_wt_tick(&w, 2000, 1000, 50, 5, 0, 12, b, sizeof b) > 0);
    CHECK("line-flips", strstr(b, "flips=30 "));
    CHECK("line-methods", strstr(b, "methods=4000 "));
    CHECK("line-draws", strstr(b, "draws=900 "));
    CHECK("line-clears", strstr(b, "clears=20 "));
    CHECK("line-labels", strstr(b, "labels/sems=50 "));
    CHECK("line-reports", strstr(b, "reports=5 "));
    CHECK("line-copies", strstr(b, "(on guest memory)=12 "));
    CHECK("line-gpuwb-zero", strstr(b, "GPU write-backs run=0 (expected 0; requests seen=7"));
    CHECK("line-no-zero-fill", strstr(b, "no zero-fill"));
    CHECK("line-secs", strstr(b, "1.00s"));
    /* second period: only the new activity */
    RSX_WT_ADD(&w, flips, 5); RSX_WT_ADD(&w, draws, 3);
    CHECK("second-period-prints", rsx_wt_tick(&w, 3500, 1000, 60, 5, 2, 12, b, sizeof b) > 0);
    CHECK("second-delta-flips", strstr(b, "flips=5 "));
    CHECK("second-delta-draws", strstr(b, "draws=3 "));
    CHECK("second-delta-labels", strstr(b, "labels/sems=10 "));
    CHECK("second-gpuwb-reported", strstr(b, "GPU write-backs run=2 "));
    CHECK("second-secs", strstr(b, "1.50s"));
    printf("%s\n", fails ? "walk_trivial: FAILED" : "walk_trivial: ok");
    return fails ? 1 : 0;
}
