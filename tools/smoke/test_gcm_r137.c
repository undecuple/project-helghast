/* Host test for KZ3 patch 2147 (RSX pass 137): the [perf-flush] line with the ring-wrap counts (gcm_perf_flush_line).
 * Reuses the stubs of the toolkit's test_gcm_legacy_state_filter.c (its main is renamed away), no game data, no GPU.
 * Built by tools/smoke/test_gcm_r137.sh with -DLEGACY_TEST_C=<path of that file>. */
#define main legacy_state_filter_main
#include LEGACY_TEST_C
#undef main

static int has(const char* s, const char* sub) { return strstr(s, sub) != NULL; }

int main(void)
{
    long long d[RSX_PERF_COUNT];
    memset(d, 0, sizeof d);
    d[RSX_PERF_W_FLUSH_N0 + 2] = 3;    d[RSX_PERF_W_FLUSH_T0 + 2] = 3000;   /* vring */
    d[RSX_PERF_W_FLUSH_N0 + 9] = 5;    d[RSX_PERF_W_FLUSH_T0 + 9] = 1000;   /* descring */
    d[RSX_PERF_W_FLUSH_N0 + 11] = 2;   /* upring */
    d[RSX_PERF_W_WRAP_SRV] = 4; d[RSX_PERF_W_WRAP_SMP] = 1;
    char line[512];
    gcm_perf_flush_line(line, sizeof line, 160.0, d, 0.001);
    puts(line);
    check(strncmp(line, "[perf-flush] t=160s tex-up 0.0ms |", 34) == 0, "the line starts as before");
    check(has(line, " vring 3/3.0ms") && has(line, " descring 5/1.0ms"), "per-reason flush counts and ms are kept");
    check(has(line, " | wraps srv 4 smp 1 up 2 vtx 3 cb 0 pscb 0"), "wraps segment: srv/smp counters, upring, vring, cbring, pscbring");
    /* a zero period still prints the segment */
    memset(d, 0, sizeof d);
    gcm_perf_flush_line(line, sizeof line, 1.0, d, 1.0);
    check(has(line, " | wraps srv 0 smp 0 up 0 vtx 0 cb 0 pscb 0"), "an idle period prints zeros");
    /* a short buffer is cut, always NUL-terminated and never overrun */
    char small[80 + 8]; memset(small, 0x5A, sizeof small);
    d[RSX_PERF_W_WRAP_SRV] = 123456789;
    gcm_perf_flush_line(small, 80, 1.0, d, 1.0);
    check(strlen(small) < 80 && (unsigned char)small[80] == 0x5A && (unsigned char)small[87] == 0x5A, "a short buffer is not overrun");
    printf("[test_gcm_r137] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
