/* Host test for KZ3 patch 2205: libs/video/rsx_texloop_split.h (the sampled texture-loop split of RSX_PERF_DRAW=1).
 * A synthetic draw loop with a fake clock: every draw spends known ticks in each stage, the timer sites are written the way
 * rsx_live_draw.c writes them (T0 reads the clock only when the draw is sampled, ADD adds the span), and the report rows
 * must give the true per-draw cost over ALL draws. Built and mutated by test_texloop_split.sh. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rsx_texloop_split.h"

static int fails;
#define CHECK(name, cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s (line %d)\n", name, __LINE__);                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static long long g_clk = 1000;
static unsigned long long g_reads;
static long long now(void)
{
    g_reads++;
    return g_clk;
}
static rsx_tls T;
#define T0(name) const long long name = T.act ? now() : 0
#define ADD(b, t0)                                                                                                     \
    do {                                                                                                               \
        if (t0) rsx_tls_add(&T, (b), now() - (t0));                                                                    \
    } while (0)

/* per-draw cost in ticks of each stage; `vary` makes a stage's cost depend on the draw index */
typedef struct {
    long long l1, get, smp, plain, zsnap, swin, rg16, desc, other;
    int units;
} cost;
static unsigned g_alias_period = 16,
                g_alias_phase; /* vary == 2: the plain bind is dear on one draw of every g_alias_period */
static cost cost_of(uint64_t i, int vary)
{
    cost c = {40, 30, 20, 100, 60, 50, 10, 70, 25, 4};
    if (vary == 2) {
        if (i % g_alias_period == g_alias_phase) c.plain += 1600;
    } else if (vary) {
        c.plain += (long long)(i % 7) * 20;
        c.zsnap += (long long)(i % 5) * 15;
        c.swin += (long long)(i % 3) * 10;
        c.units = 2 + (int)(i % 5);
    }
    return c;
}
/* the draw loop, in the order of sink_end_impl: loop { row check, decode, sampler, plain bind, zdepth_snapshot,
 * surface_window, rg16, other work }, then the descriptor writes */
static void draw(uint64_t i, int vary)
{
    const cost c = cost_of(i, vary);
    rsx_tls_begin(&T);
    T0(loop_t0);
    {
        T0(a);
        g_clk += c.l1;
        ADD(RSX_TLS_L1, a);
    }
    {
        T0(a);
        g_clk += c.get;
        ADD(RSX_TLS_GET, a);
    }
    {
        T0(a);
        g_clk += c.smp;
        ADD(RSX_TLS_SMP, a);
    }
    {
        T0(a);
        g_clk += c.plain;
        ADD(RSX_TLS_PLAIN, a);
    }
    {
        T0(a);
        g_clk += c.zsnap;
        ADD(RSX_TLS_ZSNAP, a);
    }
    {
        T0(a);
        g_clk += c.swin;
        ADD(RSX_TLS_SWIN, a);
    }
    {
        T0(a);
        g_clk += c.rg16;
        ADD(RSX_TLS_RG16, a);
    }
    g_clk += c.other; /* vertex textures, bookkeeping: inside the loop, not in a leaf */
    ADD(RSX_TLS_LOOP, loop_t0);
    if (T.act) rsx_tls_count(&T, RSX_TLS_EN, (uint64_t)c.units);
    {
        T0(a);
        g_clk += c.desc;
        ADD(RSX_TLS_DESC, a);
    }
    g_clk += 500; /* the rest of the draw */
    rsx_tls_end(&T);
}
static double near(double a, double b, double rel)
{
    return fabs(a - b) <= rel * (fabs(b) > 1e-12 ? fabs(b) : 1.0);
}

static double rowv(int b, double* calls)
{
    double us;
    rsx_tls_row(&T, b, 1.0, &us, calls);
    return us;
}

static void constant_costs(uint64_t n, const char* tag)
{
    rsx_tls_reset(&T);
    g_reads = 0;
    for (uint64_t i = 0; i < n; i++) draw(i, 0);
    const cost c = cost_of(0, 0);
    char nm[96];
    /* one sampled draw per block of 16, the first draw of the window always; a last partial block is sampled or not */
    const uint64_t want = (n + RSX_TLS_PERIOD - 1) / RSX_TLS_PERIOD,
                   lo = n / RSX_TLS_PERIOD > 0 ? n / RSX_TLS_PERIOD : 1;
    snprintf(nm, sizeof nm, "sampled-count-%s", tag);
    CHECK(nm, T.draws == n && T.sampled >= lo && T.sampled <= want);
    snprintf(nm, sizeof nm, "act-cleared-%s", tag);
    CHECK(nm, T.act == 0);
    /* a non-sampled draw reads no clock: loop + 7 leaves + desc = 9 spans = 18 reads per sampled draw */
    snprintf(nm, sizeof nm, "clock-reads-%s", tag);
    CHECK(nm, g_reads == T.sampled * 18ull && g_reads >= lo * 18ull && g_reads <= want * 18ull);
    const long long leaf[] = {0, c.l1, c.get, c.smp, c.plain, c.zsnap, c.swin, c.rg16, c.desc};
    for (int b = RSX_TLS_L1; b <= RSX_TLS_DESC; b++) {
        double calls, us = rowv(b, &calls);
        snprintf(nm, sizeof nm, "row-%d-us-%s", b, tag);
        CHECK(nm, near(us, (double)leaf[b], 1e-9));
        snprintf(nm, sizeof nm, "row-%d-calls-%s", b, tag);
        CHECK(nm, near(calls, 1.0, 1e-9));
    }
    double calls, us = rowv(RSX_TLS_LOOP, &calls);
    long long loop = c.l1 + c.get + c.smp + c.plain + c.zsnap + c.swin + c.rg16 + c.other;
    snprintf(nm, sizeof nm, "loop-us-%s", tag);
    CHECK(nm, near(us, (double)loop, 1e-9));
    snprintf(nm, sizeof nm, "other-us-%s", tag);
    CHECK(nm, near(rsx_tls_other_us(&T, 1.0), (double)c.other, 1e-9));
    rowv(RSX_TLS_EN, &calls);
    snprintf(nm, sizeof nm, "units-per-draw-%s", tag);
    CHECK(nm, near(calls, (double)c.units, 1e-9));
    /* us_per_tick is applied once */
    double us2, c2;
    rsx_tls_row(&T, RSX_TLS_PLAIN, 0.5, &us2, &c2);
    snprintf(nm, sizeof nm, "tick-factor-%s", tag);
    CHECK(nm, near(us2, c.plain * 0.5, 1e-9));
}

int main(void)
{
    constant_costs(1600, "multiple"); /* 100 sampled of 1600: scale 16 */
    constant_costs(1605, "ragged");   /* 100 or 101 sampled of 1605: scale 16.05 or 15.89, not 16 */
    constant_costs(3, "tiny");        /* 1 sampled of 3: scale 3 */
    /* costs that vary with the draw index: the sampled mean tracks the true mean (index period 7 is coprime with 16) */
    rsx_tls_reset(&T);
    const uint64_t n = 7ull * 16ull * 200ull;
    double tp = 0, tz = 0, tw = 0, tu = 0;
    for (uint64_t i = 0; i < n; i++) {
        draw(i, 1);
        const cost c = cost_of(i, 1);
        tp += (double)c.plain;
        tz += (double)c.zsnap;
        tw += (double)c.swin;
        tu += c.units;
    }
    double calls;
    CHECK("varied-plain", near(rowv(RSX_TLS_PLAIN, &calls), tp / (double)n, 0.03));
    CHECK("varied-zsnap", near(rowv(RSX_TLS_ZSNAP, &calls), tz / (double)n, 0.05));
    CHECK("varied-swin", near(rowv(RSX_TLS_SWIN, &calls), tw / (double)n, 0.05));
    rowv(RSX_TLS_EN, &calls);
    CHECK("varied-units", near(calls, tu / (double)n, 0.05));
    /* draw patterns whose period divides 16 (review of 2205): a spike on one draw of every 16 (at offset 0 and at offset 15) and of every 8.
     * A fixed sampling phase times the same offset every block, so it reads the spike on every sample or never; the true mean is
     * base + spike / period. One sample per block at a pseudo-random offset tracks it (the tolerance is about 5 sigma). */
    {
        static const unsigned per[] = {16, 16, 8}, ph[] = {0, 15, 0};
        for (int k = 0; k < 3; k++) {
            g_alias_period = per[k];
            g_alias_phase = ph[k];
            rsx_tls_reset(&T);
            const uint64_t na = 16ull * 4000ull;
            double tpl = 0;
            for (uint64_t i = 0; i < na; i++) {
                draw(i, 2);
                tpl += (double)cost_of(i, 2).plain;
            }
            const double got = rowv(RSX_TLS_PLAIN, &calls), want = tpl / (double)na;
            char nm[64];
            snprintf(nm, sizeof nm, "alias-period%u-phase%u", per[k], ph[k]);
            if (!near(got, want, 0.15)) printf("  %s: row %.1f us, true mean %.1f us\n", nm, got, want);
            CHECK(nm, near(got, want, 0.15));
        }
        g_alias_period = 16;
        g_alias_phase = 0;
    }
    /* nothing sampled: scale 0, rows 0, no NaN */
    rsx_tls_reset(&T);
    CHECK("empty-scale", rsx_tls_scale(&T) == 0.0);
    {
        double us = rowv(RSX_TLS_PLAIN, &calls);
        CHECK("empty-row", us == 0.0 && calls == 0.0 && !isnan(us));
    }
    /* a negative span (clock stepped back) is clamped, not subtracted */
    rsx_tls_reset(&T);
    rsx_tls_begin(&T);
    rsx_tls_add(&T, RSX_TLS_PLAIN, -50);
    rsx_tls_end(&T);
    CHECK("negative-span", T.ticks[RSX_TLS_PLAIN] == 0);
    /* the sampled draw (the 1st) leaves the flag clear at its end: a sink_end_impl call outside ld_pd_draw reads no clock */
    rsx_tls_reset(&T);
    g_reads = 0;
    rsx_tls_begin(&T);
    rsx_tls_end(&T);
    {
        T0(a);
        ADD(RSX_TLS_PLAIN, a);
    }
    CHECK("act-cleared-after-sampled", T.act == 0 && g_reads == 0 && T.cnt[RSX_TLS_PLAIN] == 0);
    /* the first draw of a window is sampled; every full block of 16 has exactly one sampled draw; the offset moves from block to block */
    rsx_tls_reset(&T);
    {
        int s[16 * 64], first_ok = 0, blocks_ok = 1;
        unsigned seen = 0;
        for (int i = 0; i < 16 * 64; i++) {
            s[i] = rsx_tls_begin(&T);
            rsx_tls_end(&T);
        }
        first_ok = s[0];
        for (int b = 0; b < 64; b++) {
            int n1 = 0;
            for (int i = 0; i < 16; i++)
                if (s[b * 16 + i]) {
                    n1++;
                    seen |= 1u << i;
                }
            if (n1 != 1) blocks_ok = 0;
        }
        CHECK("sampling-phase", first_ok && blocks_ok);
        CHECK("sampling-offsets-vary", __builtin_popcount(seen) >= 8);
    }
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("[test_texloop_split] ok: sampled split == true per-draw cost over all draws (1600, 1605, 3 draws), no "
           "aliasing with period-16 and period-8 draw patterns, 18 clock reads per sampled draw, 0 otherwise\n");
    return 0;
}
