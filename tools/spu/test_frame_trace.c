/* Host test for port/spu/kz3_frame_trace.c (KZ3_FRAME_TRACE, runtime patch 1085, SPU pass 66): built and run by
 * tools/spu/test_frame_trace.sh. Self-contained: the wait-accounting symbols are stubs here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void (*g_kz3_wtrace)(const char* thr, int kind, uint32_t site, uint64_t t0, uint64_t t1);
int g_kz3_wacct = -1;
static int s_wacct_env = 1000;
int kz3_wacct_init(void)
{
    return g_kz3_wacct = s_wacct_env;
}
uint64_t kz3_wacct_base(void)
{
    return 5000000000ull;
}
void kz3_frame_trace_install(void);

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "frametrace.bin";
    int fails = 0;
    unsetenv("KZ3_FRAME_TRACE");
    kz3_frame_trace_install();
    if (g_kz3_wtrace) {
        printf("FAIL: hook installed without KZ3_FRAME_TRACE\n");
        fails++;
    }
    s_wacct_env = 0;
    setenv("KZ3_FRAME_TRACE", "1,2", 1);
    kz3_frame_trace_install();
    if (g_kz3_wtrace) {
        printf("FAIL: hook installed with KZ3_WAIT_ACCT off\n");
        fails++;
    }
    s_wacct_env = 1000;
    g_kz3_wacct = -1;
    setenv("KZ3_FRAME_TRACE_OUT", out, 1);
    kz3_frame_trace_install();
    if (!g_kz3_wtrace) {
        printf("FAIL: hook not installed\n");
        return 1;
    }
    const uint64_t B = 5000000000ull, S = 1000000000ull, M = 1000000ull;
    static const char spu0[40] = "spurs-kern0:hi", walker[40] = "", mainth[40] = "ppu-main";
    g_kz3_wtrace(spu0, 33, 200, B + S / 2, B + S / 2 + M); /* before the window: dropped */
    /* 4 frames of 30 ms: a job of image 250, then the release job 244, the park ends 0.1 ms after it */
    for (int f = 0; f <= 4; f++) {
        const uint64_t t = B + S + 10 * M + f * 30 * M;
        g_kz3_wtrace(mainth, 32, 0, t, t);
        if (f == 4) break;
        g_kz3_wtrace(spu0, 33, 250 | (7u << 16), t + 1 * M, t + 11 * M);
        g_kz3_wtrace(spu0, 33, 244, t + 12 * M, t + 13 * M);
        g_kz3_wtrace(walker, 9, 0, t + 5 * M, t + 13 * M + M / 10);
        g_kz3_wtrace(mainth, 3, 75, t + 14 * M, t + 29 * M);
    }
    g_kz3_wtrace(spu0, 33, 250, B + 2 * S + 1, B + 2 * S + 2); /* past the window: writes the file */
    g_kz3_wtrace(spu0, 33, 250, B + 2 * S + 3, B + 2 * S + 4); /* after the dump: ignored */
    FILE* f = fopen(out, "rb");
    if (!f) {
        printf("FAIL: no file\n");
        return 1;
    }
    char hdr[24];
    if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "KZ3FTRC1", 8)) {
        printf("FAIL: header\n");
        fails++;
    }
    uint32_t nn, ne;
    memcpy(&nn, hdr + 8, 4);
    memcpy(&ne, hdr + 12, 4);
    fclose(f);
    if (nn != 3 || ne != 5 + 4 * 4) {
        printf("FAIL: %u threads %u events (want 3, 21)\n", nn, ne);
        fails++;
    }
    printf("%s: test_frame_trace C side (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
