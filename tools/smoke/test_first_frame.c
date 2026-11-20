/* Host test for KZ3 patch 2199: libs/video/rsx_first_frame.h. The "[frame] first frame presented at N ms" line is
 * printed exactly once however often and from however many threads the present path calls it. stderr is redirected to
 * a file and read back. No game data.  usage: test_first_frame <stderr-file> */
#define _POSIX_C_SOURCE 200809L /* pthread barriers under -std=c11 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Window hook for the non-atomic negative control only (the control header calls ff_window() between its check and its set;
 * the real header never does). While g_ff_win_on is set it holds each caller until two threads are between check and set,
 * so both have seen "not done" and both print: the broken header is caught every round on any core count, instead of
 * depending on the scheduler opening that window by chance. Cannot deadlock: nobody leaves before two have arrived, and a
 * second arrival always happens because the done flag stays 0 until someone leaves. */
static atomic_int g_ff_win_on, g_ff_win_in;
__attribute__((unused)) static void ff_window(void)
{
    if (!atomic_load(&g_ff_win_on)) return;
    atomic_fetch_add(&g_ff_win_in, 1);
    while (atomic_load(&g_ff_win_in) < 2) sched_yield();
}
#include "rsx_first_frame.h"

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

/* The race: NTHR threads wait behind a barrier and race the VERY FIRST call, round after round with fresh state
 * (s_rsx_ff_done reset by main between rounds; the header is included in this translation unit). Odd threads use the call
 * site's exact shape (guard, then note), even threads call note directly. ms = RACE_BASE + round * 8 + thread, so every printed line
 * names its round: exactly one line per round is the contract. A check-then-set that is not one atomic step prints
 * twice in some round once two threads are inside it together. */
#define NTHR 4
#define ROUNDS 20000
#define RACE_BASE 1000000ull /* above the sequential part's 777, so no racing value collides with it */
static pthread_barrier_t g_go, g_done;
static void* racer(void* a)
{
    const unsigned tid = (unsigned)(uintptr_t)a;
    for (unsigned r = 0; r < ROUNDS; r++) {
        pthread_barrier_wait(&g_go);
        const unsigned long long ms = RACE_BASE + (unsigned long long)r * 8u + tid;
        if (tid & 1u) {
            if (rsx_first_frame_pending()) rsx_first_frame_note(ms);
        } /* the call site's exact shape */
        else
            rsx_first_frame_note(ms);
        pthread_barrier_wait(&g_done);
    }
    return NULL;
}

int main(int argc, char** argv)
{
    char buf[4096];
    if (argc < 2) {
        printf("usage\n");
        return 2;
    }
    if (!freopen(argv[1], "w", stderr)) return 2;

    CHECK(rsx_first_frame_pending() == 1, "pending before the first frame");
    /* sequential: three "presents" through the guarded call site */
    for (int i = 0; i < 3; i++)
        if (rsx_first_frame_pending()) rsx_first_frame_note(777ull);
    CHECK(rsx_first_frame_pending() == 0, "not pending after the first frame");
    /* a direct second call (no guard) must not print either */
    rsx_first_frame_note(888ull);
    /* NTHR threads race the first call of each of ROUNDS rounds, each round with fresh state. The first block above printed
     * one line (777); the rounds follow. */
    pthread_t t[NTHR];
    pthread_barrier_init(&g_go, NULL, NTHR + 1);
    pthread_barrier_init(&g_done, NULL, NTHR + 1);
    for (unsigned i = 0; i < NTHR; i++) pthread_create(&t[i], NULL, racer, (void*)(uintptr_t)i);
    for (unsigned r = 0; r < ROUNDS; r++) {
        atomic_store(&s_rsx_ff_done, 0); /* fresh state: the first call of this round is the very first */
        atomic_store(&g_ff_win_in, 0);
        atomic_store(&g_ff_win_on, 1); /* only the control header calls ff_window() */
        pthread_barrier_wait(&g_go);
        pthread_barrier_wait(&g_done);
    }
    for (unsigned i = 0; i < NTHR; i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&g_go);
    pthread_barrier_destroy(&g_done);

    fflush(stderr);
    fclose(stderr);
    FILE* f = fopen(argv[1], "r");
    static unsigned per_round[ROUNDS];
    unsigned lines = 0, bad = 0, seq_lines = 0, dup_rounds = 0, miss_rounds = 0;
    char first[4096] = "";
    while (f && fgets(buf, sizeof buf, f)) {
        unsigned long long ms;
        if (!lines) snprintf(first, sizeof first, "%s", buf);
        lines++;
        if (sscanf(buf, "[frame] first frame presented at %llu ms\n", &ms) != 1)
            bad++;
        else if (ms == 777ull)
            seq_lines++;
        else if (ms >= RACE_BASE && (ms - RACE_BASE) / 8u < ROUNDS)
            per_round[(ms - RACE_BASE) / 8u]++;
        else
            bad++;
    }
    if (f) fclose(f);
    for (unsigned r = 0; r < ROUNDS; r++) {
        if (per_round[r] > 1)
            dup_rounds++;
        else if (!per_round[r])
            miss_rounds++;
    }
    CHECK(strcmp(first, "[frame] first frame presented at 777 ms\n") == 0,
          "exact line text of the sequential first frame: %s", first);
    CHECK(seq_lines == 1,
          "the sequential part printed %u lines with 777, want exactly 1 (3 guarded calls and a direct one)",
          seq_lines);
    CHECK(bad == 0, "%u lines that are not the marker line", bad);
    CHECK(dup_rounds == 0, "%u of %d racing rounds printed the line more than once", dup_rounds, ROUNDS);
    CHECK(miss_rounds == 0, "%u of %d racing rounds printed no line", miss_rounds, ROUNDS);
    CHECK(lines == 1u + ROUNDS, "stderr has %u lines, want %d (one per round and the sequential one)", lines,
          1 + ROUNDS);

    CHECK(rsx_ff_ms(10000000ull, 10000000ull + 25000ull) == 2ull, "ms conversion 2.5 ms -> 2");
    CHECK(rsx_ff_ms(0ull, 12345ull * 10000ull) == 12345ull, "ms conversion 12345 ms");
    CHECK(rsx_ff_ms(500ull, 100ull) == 0ull, "clock backwards -> 0");
    return fails ? 1 : 0;
}
