/* Host test for KZ3 patch 2187 (stutter Task 0): libs/video/rsx_present_stream.h (per-present QPC stream) and
 * rsx_stutter_diag.h (vblank/flip timing window), with synthetic samples. No game data. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rsx_present_stream.h"
#include "rsx_stutter_diag.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) { printf("[FAIL] "); printf(__VA_ARGS__); printf("\n"); fails++; }                                   \
        else printf("[PASS] %s\n", #c);                                                                                \
    } while (0)

#define CAP 256u
static rsx_ps_sample ring[CAP];
static void rec(uint64_t id, int diag)
{
    rsx_ps_sample* s = &ring[(id - 1u) & (CAP - 1u)];
    memset(s, 0, sizeof *s);
    s->present_id = id; s->guest_frame = (uint32_t)(id + 1000u); s->qpc = (int64_t)(id * 333333u);
    if (diag) { s->process_kernel_100ns = 1; s->process_user_100ns = 2; s->present_thread_kernel_100ns = 3; s->present_thread_user_100ns = 4; s->present_thread_id = 77; }
}

static unsigned count_rows(const char* path, int* sorted, uint64_t* last)
{
    FILE* f = fopen(path, "rb"); char line[512]; unsigned rows = 0; uint64_t prev = 0; *sorted = 1; *last = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || !strncmp(line, "present_id", 10)) continue;
        unsigned long long id = strtoull(line, NULL, 10);
        if (id <= prev) *sorted = 0;
        prev = id; rows++;
    }
    if (f) fclose(f);
    *last = prev;
    return rows;
}

/* Child: stream N presents the way ld_present_measure_record does (record, then flush when a batch is due), never closes. */
static void child(const char* path, unsigned n, int diag)
{
    FILE* f = fopen(path, "wb"); rsx_ps_stream st;
    rsx_ps_open(&st, f, 10000000, 123456, 133000000000000ull, diag);
    for (uint64_t id = 1; id <= n; id++) {
        rec(id, diag);
        if (rsx_ps_due(&st, id)) rsx_ps_flush(&st, ring, CAP, id);
    }
    raise(SIGKILL);   /* the Proton run ends this way; no fclose, no shutdown dump */
}

static rsx_ps_stream g_st; static uint64_t g_tot;
static void* flusher(void* a) { (void)a; for (int i = 0; i < 2000; i++) rsx_ps_flush(&g_st, ring, CAP, g_tot); return NULL; }

int main(void)
{
    const char* dir = getenv("TMPDIR_TEST") ? getenv("TMPDIR_TEST") : ".";
    char p[512];

    /* 1. kill survival: N=200 presents, killed without close: everything up to the last full batch is on disk */
    for (int diag = 0; diag < 2; diag++) {
        snprintf(p, sizeof p, "%s/stream_kill_%d.csv", dir, diag);
        pid_t pid = fork();
        if (pid == 0) { child(p, 200, diag); _exit(1); }
        int status = 0; waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child was killed (status %d)", status);
        int sorted; uint64_t last; unsigned rows = count_rows(p, &sorted, &last);
        CHECK(rows == 192 && last == 192, "diag=%d: rows %u last %llu (expect 192: all but the last 8, at most 63 lost)", diag, rows, (unsigned long long)last);
        CHECK(sorted, "ids strictly increasing");
    }
    /* worst case: 127 presents -> one batch (64) on disk, 63 lost: the documented bound */
    {
        snprintf(p, sizeof p, "%s/stream_kill_127.csv", dir);
        pid_t pid = fork();
        if (pid == 0) { child(p, 127, 0); _exit(1); }
        int status = 0; waitpid(pid, &status, 0);
        int sorted; uint64_t last; unsigned rows = count_rows(p, &sorted, &last);
        CHECK(rows == 64 && 127 - rows == 63, "127 presents: %u rows on disk (63 lost at most)", rows);
    }

    /* 2. columns and clock line, byte for byte */
    {
        snprintf(p, sizeof p, "%s/stream_cols.csv", dir);
        FILE* f = fopen(p, "wb"); rsx_ps_stream st;
        rsx_ps_open(&st, f, 10000000, 123456, 99ull, 0);
        rec(1, 0); rsx_ps_flush(&st, ring, CAP, 1); fclose(f);
        char buf[512] = {0}; f = fopen(p, "rb"); size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); (void)n;
        CHECK(!strcmp(buf, "# qpc_frequency=10000000\n# clock qpc=123456 filetime=99\npresent_id,guest_frame,qpc\n1,1001,333333\n"),
              "plain csv:\n%s", buf);
        f = fopen(p, "wb"); rsx_ps_open(&st, f, 10000000, 1, 2, 1);
        rec(1, 1); rsx_ps_flush(&st, ring, CAP, 1); fclose(f);
        memset(buf, 0, sizeof buf); f = fopen(p, "rb"); n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        CHECK(strstr(buf, "present_id,guest_frame,qpc,process_kernel_100ns,process_user_100ns,present_thread_kernel_100ns,present_thread_user_100ns,present_thread_id\n")
              && strstr(buf, "1,1001,333333,1,2,3,4,77\n"), "diag csv:\n%s", buf);
    }

    /* 3. ring overwrite: ids the ring lost are skipped, never fabricated */
    {
        snprintf(p, sizeof p, "%s/stream_ring.csv", dir);
        FILE* f = fopen(p, "wb"); rsx_ps_stream st; rsx_ps_open(&st, f, 1, 1, 1, 0);
        memset(ring, 0, sizeof ring);
        for (uint64_t id = 1; id <= 1000; id++) rec(id, 0);
        rsx_ps_flush(&st, ring, CAP, 1000); fclose(f);
        int sorted; uint64_t last; unsigned rows = count_rows(p, &sorted, &last);
        CHECK(rows == CAP && last == 1000 && sorted, "ring of %u kept: rows %u last %llu", CAP, rows, (unsigned long long)last);
    }

    /* 4. switch-off control: with no stream file open nothing is due and flush writes nothing */
    {
        rsx_ps_stream off; memset(&off, 0, sizeof off); atomic_flag_clear(&off.lock);
        CHECK(!rsx_ps_due(&off, 100000), "no stream: never due");
        rsx_ps_flush(&off, ring, CAP, 1000);
        CHECK(off.written == 0 && off.f == NULL, "no stream: flush is a no-op");
    }

    /* 5. two threads flushing at once never interleave or duplicate rows */
    {
        snprintf(p, sizeof p, "%s/stream_mt.csv", dir);
        FILE* f = fopen(p, "wb"); rsx_ps_open(&g_st, f, 1, 1, 1, 0);
        for (uint64_t id = 1; id <= 200; id++) rec(id, 0);
        g_tot = 200;
        pthread_t a, b; pthread_create(&a, NULL, flusher, NULL); pthread_create(&b, NULL, flusher, NULL);
        pthread_join(a, NULL); pthread_join(b, NULL); fclose(f);
        int sorted; uint64_t last; unsigned rows = count_rows(p, &sorted, &last);
        CHECK(rows == 200 && sorted && last == 200, "two flushers: rows %u sorted %d", rows, sorted);
    }

    /* 6. vblank/flip window */
    {
        static rsx_sd_vb v; memset(&v, 0, sizeof v);
        const int64_t F = 1000;   /* 1 tick = 1 ms */
        for (int i = 1; i <= 100; i++) rsx_sd_late(&v, i, 0, F);   /* lateness 1..100 ms */
        rsx_sd_stat st = rsx_sd_take(&v.late_ms);
        CHECK(st.n == 100 && st.p50 == 50 && st.p95 == 95 && st.max == 100, "late n=%.0f p50=%.0f p95=%.0f max=%.0f", st.n, st.p50, st.p95, st.max);
        st = rsx_sd_take(&v.late_ms);
        CHECK(st.n == 0 && st.max == 0, "window reset");
        /* 3 ticks then one handler call: merged 3, delivery lateness from the OLDEST tick */
        rsx_sd_tick(&v, 100); rsx_sd_tick(&v, 116); rsx_sd_tick(&v, 132);
        rsx_sd_deliver(&v, 140, F);
        rsx_sd_pass(&v, 3); rsx_sd_pass(&v, 1); rsx_sd_pass(&v, 0);
        rsx_sd_marker(&v, 200); rsx_sd_marker(&v, 205);   /* second marker is not the oldest unmatched one */
        rsx_sd_flip_done(&v, 212, F);
        atomic_fetch_add(&v.poll_waiting, 5ull); atomic_fetch_add(&v.poll_done, 2ull);
        char line[1024]; rsx_sd_report(&v, line, sizeof line, 777, 10.0, 600);
        CHECK(strstr(line, "[vblank] qpc=777 win=10.0s ticks=3 ") && strstr(line, "passes=2 burst_passes=1 burst_ticks=2 max_fired=3")
              && strstr(line, "handler_calls=1 ticks_per_call=3.00 max_merged=3 tick_to_handler_ms n=1 p50=40.00")
              && strstr(line, "flip_marker_to_done_ms n=1 p50=12.00") && strstr(line, "ref_publish_per_s=60.0")
              && strstr(line, "flip_status_polls waiting=5 done=2"), "line: %s", line);
        rsx_sd_report(&v, line, sizeof line, 888, 10.0, 600);
        CHECK(strstr(line, "ticks=0 ") && strstr(line, "ref_publish_per_s=0.0") && strstr(line, "waiting=0 done=0"), "second window empty: %s", line);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
