/* Host test for KZ3 patch 2072 (frame-pacing metric): libs/video/rsx_pace.h with synthetic present times. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_pace.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("[FAIL] ");                                                                                         \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        } else                                                                                                         \
            printf("[PASS] %s\n", #c);                                                                                 \
    } while (0)

typedef struct {
    unsigned n;
    double p50, p99, max, sd, low1;
    long t;
} line_t;
static int parse(const char* s, line_t* l)
{
    return sscanf(s, "[pace] t=%ld n=%u p50=%lf p99=%lf max=%lf sd=%lf low1=%lf", &l->t, &l->n, &l->p50, &l->p99,
                  &l->max, &l->sd, &l->low1) == 7;
}
static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

int main(void)
{
    char buf[256];
    line_t l;
    /* 1: 99 frames of 33.33 ms and one of 100 ms (3.4 s: no window boundary, so format it by hand). */
    static rsx_pace p;
    memset(&p, 0, sizeof p);
    double t = 1000.0;
    rsx_pace_frame(&p, t);
    for (int i = 0; i < 99; i++) {
        t += 33.33;
        rsx_pace_frame(&p, t);
    }
    t += 100.0;
    rsx_pace_frame(&p, t);
    rsx_pace_format(&p, t - p.start_ms, buf, sizeof buf);
    printf("%s\n", buf);
    CHECK(parse(buf, &l), "line format parses");
    CHECK(l.n == 100, "n=%u", l.n);
    CHECK(near(l.p50, 33.33, 0.13), "p50=%.2f", l.p50);
    CHECK(near(l.p99, 33.33, 0.13), "p99=%.2f", l.p99);
    CHECK(near(l.max, 100.0, 0.005), "max=%.2f", l.max);
    {
        const double mean = (99 * 33.33 + 100.0) / 100, var = (99 * 33.33 * 33.33 + 1e4) / 100 - mean * mean;
        CHECK(near(l.sd, sqrt(var), 0.01), "sd=%.2f want %.2f", l.sd, sqrt(var));
    }
    CHECK(near(l.low1, 10.0, 0.05), "low1=%.2f", l.low1);

    /* 2: steady 60 fps for 25 s -> two lines (10 s and 20 s), windows aligned to the first present. */
    FILE* f = freopen("test_pace.err", "w", stderr);
    if (!f) return 2;
    memset(&p, 0, sizeof p);
    t = 5000.0;
    rsx_pace_frame(&p, t);
    const double dt = 1000.0 / 60.0;
    for (int i = 0; i < 25 * 60; i++) {
        t += dt;
        rsx_pace_frame(&p, t);
    }
    fflush(stderr);
    f = freopen("test_pace.err", "r", stderr);
    char ln[256];
    int lines = 0;
    line_t w[4];
    while (f && fgets(ln, sizeof ln, f) && lines < 4)
        if (parse(ln, &w[lines])) lines++;
    CHECK(lines == 2, "windows=%d", lines);
    if (lines == 2) {
        CHECK(w[0].t >= 10000 && w[0].t < 10020 && w[1].t >= 20000 && w[1].t < 20020, "t=%ld,%ld", w[0].t, w[1].t);
        CHECK(w[0].n >= 598 && w[0].n <= 602 && w[1].n >= 598 && w[1].n <= 602, "n=%u,%u", w[0].n, w[1].n);
        for (int i = 0; i < 2; i++) {
            CHECK(near(w[i].p50, dt, 0.13) && near(w[i].p99, dt, 0.13), "p50/p99=%.2f/%.2f", w[i].p50, w[i].p99);
            CHECK(near(w[i].max, dt, 0.01) && w[i].sd < 0.01, "max=%.2f sd=%.2f", w[i].max, w[i].sd);
            CHECK(near(w[i].low1, 60.0, 0.5), "low1=%.2f", w[i].low1);
        }
    }
    /* 3: an interval beyond 250 ms lands in the overflow bucket and keeps its exact value. */
    memset(&p, 0, sizeof p);
    rsx_pace_frame(&p, 0);
    rsx_pace_frame(&p, 16);
    rsx_pace_frame(&p, 16 + 400.0);
    rsx_pace_format(&p, 416, buf, sizeof buf);
    parse(buf, &l);
    CHECK(near(l.max, 400.0, 0.005) && near(l.low1, 2.5, 0.01), "overflow max=%.2f low1=%.2f", l.max, l.low1);
    remove("test_pace.err");
    printf(fails ? "[test_pace] FAILED\n" : "[test_pace] OK\n");
    return fails != 0;
}
