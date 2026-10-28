/* Patch 1054 host test: the periodic-timer schedule arithmetic (runtime/syscalls/sys_timer_sched.h).
 * Checks: (1) deadlines are absolute and exact at a non-integer ms period (5333 us at 10 MHz, 3 MHz, 24 MHz and an
 * odd 14.318 MHz counter): deadline(n) = base + n * period with no accumulated rounding, over 10 h of ticks;
 * (2) an on-time fire never skips; (3) a wake up to `catchup` periods late keeps every deadline (fired back to back);
 * (4) a later wake skips to the first deadline after now, keeping the phase; (5) catchup = 0 never bursts;
 * (6) the old loop's arithmetic (5 ms relative) drifts 333 us per tick against it (negative control on the formula).
 * No game data, no OS calls.  Build/run: tools/spu/test_timer_sched.sh */
#include <stdint.h>
#include <stdio.h>
#include "sys_timer_sched.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

int main(void)
{
    const int64_t freqs[] = {10000000, 3000000, 24000000, 14318180};
    for (unsigned fi = 0; fi < sizeof freqs / sizeof freqs[0]; fi++) {
        const int64_t f = freqs[fi];
        lv2_tsched s;
        const int64_t base = 123456789012LL;
        lv2_tsched_init(&s, base, f, 5333);
        /* (1) exact absolute deadlines: compare with a 128-bit reference, 10 h of ticks */
        for (uint64_t n = 1; n < 6750000ULL; n += 997) {
            unsigned __int128 ref = (unsigned __int128)n * 5333u * (uint64_t)f / 1000000u;
            int64_t d = lv2_tsched_deadline_n(&s, n);
            CHECK(d == base + (int64_t)ref, "f=%lld n=%llu deadline %lld != %lld", (long long)f, (unsigned long long)n,
                  (long long)d, (long long)(base + (int64_t)ref));
        }
        /* (2) on-time fires: n advances by one, nothing skipped */
        for (int i = 0; i < 1000; i++) {
            int64_t dl = lv2_tsched_deadline(&s);
            uint64_t n0 = s.n;
            uint64_t sk = lv2_tsched_fire(&s, dl + f / 20000 /* 50 us late */, 2);
            CHECK(sk == 0 && s.n == n0 + 1, "on-time fire skipped %llu", (unsigned long long)sk);
        }
        /* (3) 2 periods late with catchup 2: no skip, the two missed deadlines are due at once */
        {
            uint64_t n0 = s.n;
            int64_t now = lv2_tsched_deadline_n(&s, n0 + 2) + 1; /* woke after deadline n0+2 */
            uint64_t sk = lv2_tsched_fire(&s, now, 2);
            CHECK(sk == 0 && s.n == n0 + 1, "2-late catchup=2: skipped %llu n=%llu", (unsigned long long)sk,
                  (unsigned long long)(s.n - n0));
            CHECK(lv2_tsched_deadline(&s) <= now, "missed deadline not due");
            sk = lv2_tsched_fire(&s, now, 2);
            sk += lv2_tsched_fire(&s, now, 2);
            CHECK(sk == 0 && s.n == n0 + 3 && lv2_tsched_deadline(&s) > now,
                  "catch-up did not end at the next future deadline");
        }
        /* (4) 50 periods late: skip to the first deadline after now, phase kept */
        {
            uint64_t n0 = s.n;
            int64_t now = lv2_tsched_deadline_n(&s, n0 + 50) + 7;
            uint64_t sk = lv2_tsched_fire(&s, now, 2);
            CHECK(s.n == n0 + 51 && sk == 50, "50-late: n advanced %llu skipped %llu", (unsigned long long)(s.n - n0),
                  (unsigned long long)sk);
            CHECK(lv2_tsched_deadline(&s) > now && lv2_tsched_deadline_n(&s, s.n - 1) <= now, "phase not kept");
            /* exactly on a deadline: that deadline counts as passed */
            n0 = s.n;
            now = lv2_tsched_deadline_n(&s, n0 + 10);
            sk = lv2_tsched_fire(&s, now, 2);
            CHECK(s.n == n0 + 11 && lv2_tsched_deadline(&s) > now, "on-deadline skip wrong (n+%llu)",
                  (unsigned long long)(s.n - n0));
        }
        /* (5) catchup 0: a wake past the next deadline never bursts */
        {
            uint64_t n0 = s.n;
            int64_t now = lv2_tsched_deadline_n(&s, n0 + 1) + 1;
            uint64_t sk = lv2_tsched_fire(&s, now, 0);
            CHECK(sk == 1 && s.n == n0 + 2 && lv2_tsched_deadline(&s) > now, "catchup 0 burst (n+%llu sk %llu)",
                  (unsigned long long)(s.n - n0), (unsigned long long)sk);
        }
    }
    /* (6) negative control: the old loop waited period/1000 ms after each send (5 ms for 5333 us), so even with zero
     * wake latency its n-th tick is 333 us x n early against the absolute schedule: 1 s of ticks is 62 ms off. */
    {
        lv2_tsched s;
        lv2_tsched_init(&s, 0, 10000000, 5333);
        int64_t old_t = 0;
        for (int i = 0; i < 187; i++) old_t += (5333 / 1000) * 10000; /* 5 ms in 100 ns ticks */
        int64_t drift_us = (lv2_tsched_deadline_n(&s, 187) - old_t) / 10;
        CHECK(drift_us > 60000 && drift_us < 63000, "old-loop drift %lld us", (long long)drift_us);
        printf("old relative 5 ms loop vs absolute schedule after 187 ticks: %lld us\n", (long long)drift_us);
    }
    /* ticks <-> us round trip */
    CHECK(lv2_tsched_ticks_to_us(lv2_tsched_us_to_ticks(5333, 10000000), 10000000) == 5333, "round trip");
    printf("%s: test_timer_sched (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
