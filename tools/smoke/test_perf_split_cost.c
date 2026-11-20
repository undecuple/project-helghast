/* Host test for KZ3 patch 2203: the sampled decode_d of the perf split (libs/video/rsx_o0.h: rsx_split_dsel/dpush/dpop/scale_d).
 * A word-sized spin of known duration (virtual clock, see vclk) is run inside the sampled region; the scaled decode_d must
 * come back EXACTLY words x spin (words is a multiple of RSX_SPLIT_DSAMPLE, so timed x scale x spin == words x spin), and the buckets must still add up to the walk. `--negctl` drops the scale factor (scale 1): the recovered
 * value is then about 1/32 of the truth, so the same checks must FAIL (the .sh requires the non-zero exit).
 * Pure C, no game data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_o0.h"
rsx_o0 g_rsx_o0;
static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("[FAIL] %s:%d ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)
/* Virtual clock: the code under test takes every timestamp as an argument, so the spin is advanced by hand instead of busy
 * waiting on CLOCK_MONOTONIC. Only 1 word in RSX_SPLIT_DSAMPLE is timed and scaled up, so a real clock lets one preemption of
 * a sampled word (or a spin overshoot) be magnified 32x: that measured the host scheduler, not patch 2203's arithmetic. */
static long long vclk;
static long long now_ns(void) { return vclk; }
static void spin_ns(long long ns) { vclk += ns; }

/* the walker: per word, own loop work (decode_f, the base bucket) then the dispatch (decode_d) */
static long long no_owed[RSX_SB_N];
static void run(int scale, int words_n, long long loop_ns, long long disp_ns, long long sink_ns, double* rec_d,
                double* rec_f, double* walk_err)
{
    rsx_split s;
    unsigned long long phase = 0, words = 0, timed = 0;
    const long long w0 = now_ns();
    rsx_split_begin(&s, w0);
    rsx_split_base(&s, RSX_SB_DECODE_F, now_ns());
    for (int i = 0; i < words_n; i++) {
        spin_ns(loop_ns); /* loop work: stays in decode_f */
        long long d0 = 0;
        int tok = 0;
        if (rsx_split_dsel(&phase, &words, &timed)) tok = rsx_split_dpush(&s, &d0, now_ns());
        spin_ns(disp_ns);            /* the dispatch: decode_d self time */
        if (i % 7 == 0 && sink_ns) { /* a sink with its own bucket inside the dispatch */
            const int k = rsx_split_push(&s, RSX_SB_TEX, now_ns());
            spin_ns(sink_ns);
            rsx_split_pop(&s, k, now_ns());
        }
        if (tok) rsx_split_dpop(&s, tok, d0, scale, no_owed, now_ns());
    }
    const long long w1 = now_ns();
    rsx_split_end(&s, w1);
    rsx_split_settle(&s, no_owed);
    CHECK(timed == (unsigned long long)words_n / RSX_SPLIT_DSAMPLE, "timed %llu", timed);
    *rec_d = (double)s.acc[RSX_SB_DECODE_D];
    *rec_f = (double)s.acc[RSX_SB_DECODE_F];
    *walk_err = (double)(rsx_split_total(&s) - (w1 - w0)) / (double)(w1 - w0);
}

/* Many walk passes on an exact fake clock, as the walker runs them: rsx_split_begin/end per pass, the sampler phase and the owed
 * debits persisting across passes, buckets flushed into tot[] per pass. cost(i) = the dispatch cost of word i of the pass.
 * fault: 0 = the patched logic; 1 = the phase is reset every pass (the old code); 2 = the debit is taken at once, unclamped
 * (the old scale_d, so a short pass drives decode_f below zero). */
static long long cost_const(int i)
{
    (void)i;
    return 100;
}
static long long cost_idx31(int i)
{
    return i == 31 ? 3200 : 100;
} /* a pass whose word 31 is expensive */
static void passes(int fault, int npass, int wpp, long long (*cost)(int), long long loop_ns, int check_nonneg,
                   long long* decode_d, long long* total, long long* walk, long long* owed_left,
                   unsigned long long* timed_out)
{
    long long now = 0, tot[RSX_SB_N] = {0}, owed[RSX_SB_N] = {0};
    unsigned long long phase = 0, words = 0, timed = 0;
    for (int p = 0; p < npass; p++) {
        rsx_split s;
        if (fault == 1) phase = 0;
        rsx_split_begin(&s, now);
        rsx_split_base(&s, RSX_SB_DECODE_F, now);
        for (int i = 0; i < wpp; i++) {
            long long d0 = 0;
            int tok = 0;
            now += loop_ns;
            if (rsx_split_dsel(&phase, &words, &timed)) tok = rsx_split_dpush(&s, &d0, now);
            now += cost(i);
            if (tok) rsx_split_dpop(&s, tok, d0, RSX_SPLIT_DSAMPLE, owed, now);
        }
        rsx_split_end(&s, now);
        if (fault == 2) {
            for (int b = 0; b < RSX_SB_N; b++) {
                s.acc[b] -= owed[b];
                owed[b] = 0;
            }
        } else {
            rsx_split_settle(&s, owed);
        }
        for (int b = 0; b < RSX_SB_N; b++) {
            if (check_nonneg) CHECK(s.acc[b] >= 0, "pass %d bucket %d went negative: %lld", p, b, s.acc[b]);
            tot[b] += s.acc[b];
        }
    }
    long long sum = 0, ol = 0;
    for (int b = 0; b < RSX_SB_N; b++) {
        sum += tot[b];
        ol += owed[b];
    }
    *decode_d = tot[RSX_SB_DECODE_D];
    *total = sum;
    *walk = now;
    *owed_left = ol;
    *timed_out = timed;
}

int main(int argc, char** argv)
{
    const int neg = argc > 1 && !strcmp(argv[1], "--negctl");
    const int neg_phase = argc > 1 && !strcmp(argv[1], "--negctl-phase");
    const int neg_clamp = argc > 1 && !strcmp(argv[1], "--negctl-clamp");
    const int scale = neg ? 1 : RSX_SPLIT_DSAMPLE;
    /* deterministic core: exact fake clock, the dispatch costs 100 and the loop work 10 on every word */
    {
        rsx_split s;
        unsigned long long ph = 0, w = 0, t = 0;
        long long now = 0, owed[RSX_SB_N] = {0};
        rsx_split_begin(&s, 0);
        rsx_split_base(&s, RSX_SB_DECODE_F, 0);
        for (int i = 0; i < 3200; i++) {
            long long d0 = 0;
            int tok = 0;
            now += 10;
            if (rsx_split_dsel(&ph, &w, &t)) tok = rsx_split_dpush(&s, &d0, now);
            now += 100;
            if (tok) rsx_split_dpop(&s, tok, d0, scale, owed, now);
        }
        rsx_split_end(&s, now);
        rsx_split_settle(&s, owed);
        CHECK(w == 3200 && t == 100, "words %llu timed %llu", w, t);
        CHECK(rsx_split_total(&s) == now, "buckets %lld vs walk %lld", rsx_split_total(&s), now);
        CHECK(s.acc[RSX_SB_DECODE_D] == 3200 * 100, "exact decode_d %lld (want 320000)", s.acc[RSX_SB_DECODE_D]);
        CHECK(s.acc[RSX_SB_DECODE_F] == 3200 * 10, "exact decode_f %lld (want 32000)", s.acc[RSX_SB_DECODE_F]);
    }
    /* virtual spin: 20000 words x 5 us in the sampled region, 1 us loop work, a 3 us sink every 7th word */
    {
        const int n = 20000;
        const long long disp = 5000, loop = 1000, sink = 3000;
        double d, f, err;
        run(scale, n, loop, disp, sink, &d, &f, &err);
        const double want = (double)n * (double)disp, got = d, rel = (got - want) / want;
        /* exact: n is a multiple of RSX_SPLIT_DSAMPLE, so n/32 timed words x scale 32 x disp == n x disp, and the sinks sit in
         * their own bucket inside the dispatch, so they never enter the sampled word's decode_d. No tolerance is needed. */
        printf("[test_perf_split_cost] scale=%d decode_d got %.0f ns want %.0f ns (%+.2f%%), decode_f %.0f ns, walk "
               "error %+.4f%%\n",
               scale, got, want, rel * 100.0, f, err * 100.0);
        CHECK(n % RSX_SPLIT_DSAMPLE == 0 && got == want, "decode_d not exactly the known spin: %+.2f%%", rel * 100.0);
        CHECK(err == 0.0, "buckets do not add up to the walk: %+.4f%%", err * 100.0);
    }
    /* the sampler across passes (review of 2203): the phase persists, the per-pass word count is only the flushed delta */
    {
        long long d, tot, walk, ol;
        unsigned long long timed;
        const int fault = neg_phase ? 1 : 0;
        /* A: 1000 passes of 20 words (fewer than 32): the old per-pass phase never sampled, decode_d was 0 */
        passes(fault, 1000, 20, cost_const, 10, 0, &d, &tot, &walk, &ol, &timed);
        CHECK(timed == 20000 / RSX_SPLIT_DSAMPLE, "short passes: timed %llu (want 625)", timed);
        CHECK(d == 20000ll * 100, "short passes: decode_d %lld (want 2000000)", d);
        /* B: 3200 passes of 37 words, word 31 of each pass 32x dearer: a per-pass phase samples exactly that word every time */
        passes(fault, 3200, 37, cost_idx31, 10, 0, &d, &tot, &walk, &ol, &timed);
        const long long truth = 3200ll * (36 * 100 + 3200);
        CHECK(d == truth, "aliased pass pattern: decode_d %lld (want %lld, %+.1f%%)", d, truth,
              100.0 * (double)(d - truth) / (double)truth);
        CHECK(tot - ol == walk, "buckets %lld - owed %lld != walk %lld", tot, ol, walk);
        CHECK(ol * 100 < walk, "owed debit left over: %lld of %lld", ol, walk);
    }
    /* decode_f never negative on short passes, and the total is kept: the debit is clamped per pass, the rest stays owed */
    {
        long long d, tot, walk, ol;
        unsigned long long timed;
        passes(neg_clamp ? 2 : 0, 1000, 20, cost_const, 10, 1, &d, &tot, &walk, &ol,
               &timed); /* fault 2 = the old unclamped debit */
        CHECK(tot - ol == walk, "short passes: buckets %lld - owed %lld != walk %lld", tot, ol, walk);
        CHECK(ol * 100 < walk, "short passes: owed left over %lld of %lld", ol, walk);
    }
    {
        char ln[200];
        rsx_split_fmt_cost(ln, sizeof ln, 12, 2, 6400, 640, 200);
        CHECK(!strcmp(ln, "[perf-split-cost] t=12 words=3200 commands=320 timed_words=100 sample=1/32 per_flip"),
              "cost line: %s", ln);
    }
    if (fails) {
        printf("[test_perf_split_cost] %d FAILED\n", fails);
        return 1;
    }
    printf("[test_perf_split_cost] ok\n");
    return 0;
}
