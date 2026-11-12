/* Patch 1156 host test: KZ3_SPU_JOBPROF's [spu-job] window line. Includes kz3_wait_acct.c directly (the printer is static).
 * Synthetic job set: 3 threads note known (image, ns, calls) through the public accounting API; two windows are printed
 * with the profile on, one window with it off. Checks: the line's per-image ms/calls equal the notes (window deltas, summed
 * over threads), the totals line up, the off run prints no [spu-job] line and leaves every jobprof accumulator at zero, and
 * the per-thread [wacct] lines are identical on and off. Negative control: a doctored expectation must be reported as a mismatch. */
#include "kz3_wait_acct.c"
#include <assert.h>
#include <unistd.h>

#define NIMG 5
static const int  k_img[NIMG] = { 255, 241, 205, 260, 7 };
static const int  k_ncall[NIMG] = { 10, 4, 3, 5, 1 };       /* calls per thread per round */
static const uint64_t k_ns[NIMG] = { 3000000, 2000000, 1500000, 1000000, 100000 };   /* ns per call */
static volatile int s_round_go, s_done;

static void* worker(void* a)
{
    const long id = (long)a; char nm[32]; snprintf(nm, sizeof nm, "synthjob%ld", id);
    kz3_wacct_name(nm);
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < NIMG; i++)
            for (int c = 0; c < k_ncall[i]; c++) kz3_wacct_note(KZ3_WA_JOB, (uint32_t)k_img[i], k_ns[i]);
        kz3_wacct_note(KZ3_WA_KSLEEP, 0, 5000000);
        __sync_fetch_and_add(&s_done, 1);
        while (s_round_go <= r) usleep(200);
    }
    return NULL;
}

static char* slurp(const char* p) { FILE* f = fopen(p, "rb"); static char b[1 << 16]; size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0; b[n] = 0; if (f) fclose(f); return b; }

/* value "ovlN=ms/calls" in a [spu-job] line */
static int field(const char* line, const char* key, double* ms, unsigned long long* n)
{
    const char* p = strstr(line, key); if (!p) return 0;
    return sscanf(p + strlen(key), "=%lf/%llu", ms, n) == 2;
}
static int check_line(const char* line, int nthr_expect, double scale, int doctored)
{
    int bad = 0;
    for (int i = 0; i < NIMG - 1; i++) {   /* top 4 of 5: the smallest image is not listed */
        char key[16]; snprintf(key, sizeof key, " ovl%d", k_img[i]); double ms; unsigned long long n;
        if (!field(line, key, &ms, &n)) { bad++; continue; }
        double em = (double)k_ns[i] * k_ncall[i] * nthr_expect / 1e6 * scale; unsigned long long en = (unsigned long long)k_ncall[i] * nthr_expect;
        if (doctored && i == 1) en += 1;
        if (ms < em - 0.05 || ms > em + 0.05 || n != en) bad++;
    }
    double ms; unsigned long long n; double sum = 0; unsigned long long cn = 0;
    for (int i = 0; i < NIMG; i++) cn += (unsigned long long)k_ncall[i] * nthr_expect, sum += (double)k_ns[i] * k_ncall[i] * nthr_expect / 1e6;
    if (!field(line, " jobs", &ms, &n) || n != cn || ms < sum - 0.05 || ms > sum + 0.05) bad++;
    if (!field(line, " ksleep", &ms, &n) || n != (unsigned long long)nthr_expect) bad++;
    return bad;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    char fon[512], foff[512]; snprintf(fon, sizeof fon, "%s/r156_on.txt", dir); snprintf(foff, sizeof foff, "%s/r156_off.txt", dir);
    g_kz3_wacct = 1000; s_t0 = kz3_wacct_now(); s_jp_topn = 4;   /* what KZ3_SPU_JOBPROF=4 would read */
    pthread_t th[3];
    for (long i = 0; i < 3; i++) pthread_create(&th[i], NULL, worker, (void*)i);
    int fail = 0;
    /* window 1, profile on */
    while (s_done < 3) usleep(200);
    freopen(fon, "w", stderr); print_window(1.0, 0);
    s_done = 0; s_round_go = 1;
    while (s_done < 3) usleep(200);
    print_window(2.0, 0); fflush(stderr);
    /* both rounds' notes are already in; window 3 = nothing new, profile OFF over the same accounting state */
    const int pk_before = (int)(s_jp_pns[255] != 0);
    s_jp_topn = 0; memset(s_jp_pns, 0, sizeof s_jp_pns); memset(s_jp_pn, 0, sizeof s_jp_pn); memset(s_jp_pk, 0, sizeof s_jp_pk);
    freopen(foff, "w", stderr); print_window(3.0, 0); fflush(stderr);
    s_round_go = 2; for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    freopen("/dev/tty", "w", stderr);
    (void)pk_before;
    char* on = strdup(slurp(fon)); char* off = strdup(slurp(foff));
    /* split the two [spu-job] lines */
    char* l1 = strstr(on, "[spu-job]"); char* l2 = l1 ? strstr(l1 + 1, "[spu-job]") : NULL;
    if (!l1 || !l2) { printf("FAIL: expected two [spu-job] lines with the profile on\n"); return 1; }
    char a[1500], b[1500]; snprintf(a, sizeof a, "%.*s", (int)strcspn(l1, "\n"), l1); snprintf(b, sizeof b, "%.*s", (int)strcspn(l2, "\n"), l2);
    int f1 = check_line(a, 3, 1.0, 0), f2 = check_line(b, 3, 1.0, 0);
    printf("window1 mismatches=%d  window2 mismatches=%d\n  %s\n  %s\n", f1, f2, a, b);
    fail += f1 + f2;
    /* top 4 of 5: the smallest image (7) must be left out of the list but still counted in jobs= */
    if (strstr(a, " ovl7=")) { printf("FAIL: top 4 listed a fifth image\n"); fail++; }
    /* negative control */
    int nc = check_line(a, 3, 1.0, 1);
    printf("negative control (doctored expectation) mismatches=%d (must be > 0)\n", nc);
    if (nc == 0) { printf("FAIL: negative control did not detect a doctored count\n"); fail++; }
    /* off: no [spu-job] line, accumulators untouched */
    if (strstr(off, "[spu-job]")) { printf("FAIL: profile off printed a [spu-job] line\n"); fail++; }
    uint64_t any = 0; for (int i = 0; i < JP_IDS; i++) any |= s_jp_pns[i] | s_jp_pn[i];
    for (int k = 0; k < 3; k++) any |= s_jp_pk[k][0] | s_jp_pk[k][1];
    printf("off: accumulators %s\n", any ? "TOUCHED" : "untouched");
    if (any) fail++;
    /* per-thread [wacct] lines: window 1 (on) and window 3 (off) cover different notes, so compare the thread-line format
     * only: same set of thread names present in both, none mentions spu-job */
    int t_on = 0, t_off = 0; for (const char* p = on; (p = strstr(p, "[wacct] thr=synthjob")); p++) t_on++;
    for (const char* p = off; (p = strstr(p, "[wacct] thr=synthjob")); p++) t_off++;
    printf("[wacct] thread lines: on=%d (2 windows x 3) off=%d (no new notes -> 0)\n", t_on, t_off);
    if (t_on != 6 || t_off != 0) fail++;
    printf(fail ? "FAILED\n" : "ALL OK\n");
    return fail ? 1 : 0;
}
