/* Host test for KZ3 patch 2210: libs/video/rsx_diag_delay.h (KZ3_DIAG_DELAY core), with a fake clock that advances 1 tick per
 * read (freq 1000 ticks/s, so one tick = 1 ms). Built and mutated by test_diag_probes.sh. */
#include <stdio.h>
#include <string.h>
#include "rsx_diag_delay.h"
static int fails;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s (line %d)\n", name, __LINE__); fails++; } } while (0)
static long long g_clock, g_reads;
static long long fake_now(void) { g_reads++; return g_clock++; }
int main(void)
{
    double v[2]; char e[96], m[600], b[600];
    CHECK("parse-main", rsx_dd_parse("main:2", v, e, sizeof e) == 0 && v[0] == 2.0 && v[1] == 0.0);
    CHECK("parse-walk-frac", rsx_dd_parse("walk:0.5", v, e, sizeof e) == 0 && v[0] == 0.0 && v[1] == 0.5);
    CHECK("parse-both", rsx_dd_parse("main:2,walk:3", v, e, sizeof e) == 0 && v[0] == 2.0 && v[1] == 3.0);
    CHECK("parse-unset", rsx_dd_parse(NULL, v, e, sizeof e) == -2 && rsx_dd_parse("", v, e, sizeof e) == -2);
    CHECK("parse-bad-side", rsx_dd_parse("gpu:2", v, e, sizeof e) == -1 && strstr(e, "expected main:N or walk:N"));
    CHECK("parse-no-number", rsx_dd_parse("main:", v, e, sizeof e) == -1);
    CHECK("parse-negative", rsx_dd_parse("main:-1", v, e, sizeof e) == -1);
    CHECK("parse-zero", rsx_dd_parse("main:0", v, e, sizeof e) == -1);
    CHECK("parse-huge", rsx_dd_parse("walk:5000", v, e, sizeof e) == -1);
    CHECK("parse-trailing", rsx_dd_parse("main:2x", v, e, sizeof e) == -1);
    rsx_dd d;
    CHECK("init-bad-off", rsx_dd_init(&d, "bogus", 1000, fake_now, m, sizeof m) == 0 && strstr(m, "ignored") && !d.on);
    CHECK("inject-when-off", rsx_dd_inject(&d, RSX_DD_MAIN, 1) == 0 && d.count[0] == 0);
    CHECK("init-ok", rsx_dd_init(&d, "main:5", 1000, fake_now, m, sizeof m) == 1 && d.on && strstr(m, "[diag-delay] on"));
    g_clock = 100;
    const long long el = rsx_dd_inject(&d, RSX_DD_MAIN, 77);
    CHECK("spins-asked-ticks", el == 5);
    CHECK("busy-waits-reads-clock", g_reads >= 5);
    CHECK("walk-side-off", rsx_dd_inject(&d, RSX_DD_WALK, 78) == 0 && d.count[RSX_DD_WALK] == 0);
    CHECK("count", d.count[RSX_DD_MAIN] == 1);
    CHECK("tid-first-only", d.first_tid[RSX_DD_MAIN] == 77);
    rsx_dd_inject(&d, RSX_DD_MAIN, 99);
    CHECK("tid-kept", d.first_tid[RSX_DD_MAIN] == 77 && d.count[RSX_DD_MAIN] == 2);
    CHECK("ticks-summed", d.ticks[RSX_DD_MAIN] == 10);
    /* report: first tick arms the clock, then one line per period with the delta mean */
    rsx_dd_init(&d, "main:4,walk:2", 1000, fake_now, m, sizeof m); g_clock = 1000;   /* a real QPC is never 0, 0 means unarmed */
    CHECK("tick-first-silent", rsx_dd_tick(&d, b, sizeof b) == 0);
    for (int i = 0; i < 10; i++) rsx_dd_inject(&d, RSX_DD_MAIN, 1);
    for (int i = 0; i < 5; i++) rsx_dd_inject(&d, RSX_DD_WALK, 2);
    g_clock += 2000;
    CHECK("tick-prints", rsx_dd_tick(&d, b, sizeof b) > 0);
    CHECK("line-tag", !strncmp(b, "[diag-delay] ", 13));
    CHECK("line-main-count", strstr(b, "main: count=10 mean injected=4.000 ms (asked 4.000"));
    CHECK("line-walk-count", strstr(b, "walk: count=5 mean injected=2.000 ms (asked 2.000"));
    CHECK("tick-once-per-period", rsx_dd_tick(&d, b, sizeof b) == 0);
    for (int i = 0; i < 3; i++) rsx_dd_inject(&d, RSX_DD_MAIN, 1);
    g_clock += 2000;
    CHECK("second-tick-prints", rsx_dd_tick(&d, b, sizeof b) > 0);
    CHECK("second-delta-count", strstr(b, "main: count=3 mean injected=4.000 ms") && strstr(b, "total 13"));
    printf("%s\n", fails ? "diag_delay: FAILED" : "diag_delay: ok");
    return fails ? 1 : 0;
}
