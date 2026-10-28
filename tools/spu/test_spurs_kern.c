/* KZ3 patch 1074 host test: the per-SPU SPURS kernel's selection rule and idle wake (runtime/spu/spurs_kern_sched.h,
 * rule ported from ps3recomp PR #213 by NoahBPeterson). Checks, on a byte image of a CellSpurs instance:
 *  admission   - a workload runs only while wanted: readyCount + idle > contention, a signal (taken by one SPU), or
 *                the flag; readyCount 0 and no signal = no SPU (the old kernel ran it anyway);
 *  cap         - never more SPUs than maxContention; priority byte 0 = never on that SPU; not runnable = never;
 *  weight      - the SPU's priority first (1 beats 8), then the workload the SPU ran last;
 *  poll        - a running module keeps its SPU while still wanted, is told to switch when a better one is wanted or
 *                its own readyCount went to 0, and a poll never changes contention;
 *  wake        - an idle SPU sleeping on the instance wakes at once on a signal, and a signal between the gen read and
 *                the wait is not lost;
 *  stress      - 5 SPU threads selecting/leaving under one lock with readyCount 3: contention never exceeds 3, ends 0.
 * Self-contained (no game data, no work/): builds from the patched toolkit's header alone. */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "spurs_kern_sched.h"

static int fails;
#define CHECK(what, cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fails++;                                                                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__);                                                   \
        }                                                                                                              \
    } while (0)

static uint8_t S[0x1000];
static int g_idle; /* idle_admits */
static void inst_reset(void)
{
    memset(S, 0, sizeof S);
    memset(S + KZ3_K_FLAG, 0xFF, 16); /* no flag */
    S[KZ3_K_FLAG_RCV] = 0xFF;
}
/* wid w: enabled, state 2, minC 1, maxC m, priority p on SPUs 0..5 (0 on 6, 7), uniqueId w */
static void wkl_add(uint32_t w, uint8_t m, uint8_t p)
{
    S[KZ3_K_ENABLED + (w >> 3)] |= (uint8_t)(0x80u >> (w & 7));
    S[KZ3_K_STATE1 + w] = 2;
    S[KZ3_K_MINCONT + w] = 1;
    S[KZ3_K_MAXCONT + w] = m;
    uint8_t* info = S + KZ3_K_INFO1 + w * KZ3_K_INFO_SZ;
    info[0x14] = (uint8_t)w;
    for (int n = 0; n < 6; n++) info[0x18 + n] = p;
}
static uint32_t sel(uint16_t mine, uint32_t spu, uint32_t cur, int poll, uint32_t* st)
{
    uint32_t s0 = 0;
    return kz3_ksel(S, mine, spu, cur, poll, g_idle, st ? st : &s0);
}

/* ---- stress ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_maxc, g_bad;
static void* spu_thread(void* a)
{
    const uint32_t n = (uint32_t)(uintptr_t)a;
    uint32_t cur = KZ3_KSEL_NONE;
    for (int it = 0; it < 200000; it++) {
        pthread_mutex_lock(&g_lock);
        uint32_t st, w = kz3_ksel(S, 0xC000, n, cur, 0, 0, &st);
        if (w < 16) {
            int c = S[KZ3_K_CURCONT + w];
            if (c > g_maxc) g_maxc = c;
            if (w != 0) g_bad = 1;
        }
        pthread_mutex_unlock(&g_lock);
        if (w >= 16) continue;
        pthread_mutex_lock(&g_lock);
        kz3_kleave(S, w);
        pthread_mutex_unlock(&g_lock);
        cur = w;
    }
    return 0;
}
/* ---- wake ---- */
static kz3_kwake g_kw;
static volatile int g_woke;
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void* waiter(void* a)
{
    (void)a;
    uint32_t g = kz3_kwake_gen(&g_kw);
    g_woke = kz3_kwake_wait(&g_kw, g, 5000) ? 1 : -1;
    return 0;
}

int main(void)
{
    uint32_t st;
    /* admission */
    inst_reset();
    wkl_add(1, 5, 8);
    CHECK("ready 0, no signal: no SPU", sel(0x4000, 0, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_READY1 + 1] = 2;
    CHECK("ready 2: SPU 0 takes wid 1", sel(0x4000, 0, KZ3_KSEL_NONE, 0, &st) == 1 && st == KZ3_POLL_READY);
    CHECK("contention raised to 1", S[KZ3_K_CURCONT + 1] == 1);
    CHECK("ready 2: SPU 1 takes wid 1", sel(0x4000, 1, KZ3_KSEL_NONE, 0, 0) == 1);
    CHECK("ready 2, contention 2: SPU 2 idles", sel(0x4000, 2, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_IDLE2 + 1] = 1;
    CHECK("ready 2 + idle 1 > 2: SPU 2 admitted", sel(0x4000, 2, KZ3_KSEL_NONE, 0, &st) == 1 && st == 0);
    kz3_kleave(S, 1);
    kz3_kleave(S, 1);
    kz3_kleave(S, 1);
    CHECK("leave x3: contention 0", S[KZ3_K_CURCONT + 1] == 0);
    kz3_kleave(S, 1);
    CHECK("leave at 0 stays 0", S[KZ3_K_CURCONT + 1] == 0);
    /* idle-SPU requests: with idle_admits, idleSpuCount alone admits, up to that count, below wanted work */
    inst_reset();
    wkl_add(1, 5, 8);
    wkl_add(2, 5, 8);
    S[KZ3_K_IDLE2 + 1] = 2;
    CHECK("idle 2, ready 0, idle_admits off: no SPU", sel(0x6000, 0, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    g_idle = 1;
    CHECK("idle_admits: SPU 0 takes wid 1", sel(0x6000, 0, KZ3_KSEL_NONE, 0, &st) == 1 && st == 0);
    CHECK("idle_admits: SPU 1 takes wid 1", sel(0x6000, 1, KZ3_KSEL_NONE, 0, 0) == 1);
    CHECK("idle_admits: idle count 2 reached, SPU 2 idles", sel(0x6000, 2, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_READY1 + 2] = 1;
    CHECK("wanted wid 2 beats the idle request (lower wid, same priority)", sel(0x6000, 2, 1, 0, 0) == 2);
    CHECK("poll on idle-admitted wid 1 with wid 2 wanted elsewhere full: stay", sel(0x6000, 0, 1, 1, 0) == 1);
    S[KZ3_K_READY1 + 2] = 2;
    CHECK("poll on idle-admitted wid 1, wid 2 wanted: switch", sel(0x6000, 0, 1, 1, 0) == 2);
    g_idle = 0;
    /* signal */
    inst_reset();
    wkl_add(3, 5, 8);
    S[KZ3_K_SIGNAL1] = 0x10; /* bit 15-3 */
    CHECK("signal: SPU 0 takes wid 3", sel(0x1000, 0, KZ3_KSEL_NONE, 0, &st) == 3 && st == KZ3_POLL_SIGNAL);
    CHECK("signal taken", S[KZ3_K_SIGNAL1] == 0);
    CHECK("signal taken: SPU 1 idles", sel(0x1000, 1, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    /* flag */
    inst_reset();
    wkl_add(2, 5, 8);
    S[KZ3_K_FLAG_RCV] = 2;
    memset(S + KZ3_K_FLAG + 0x0C, 0, 4);
    CHECK("flag 0 + receiver: taken", sel(0x2000, 0, KZ3_KSEL_NONE, 0, &st) == 2 && st == KZ3_POLL_FLAG);
    CHECK("flag re-armed", S[KZ3_K_FLAG + 0x0C] == 0xFF);
    /* cap, priority 0, runnable */
    inst_reset();
    wkl_add(1, 5, 8);
    S[KZ3_K_READY1 + 1] = 8;
    for (uint32_t n = 0; n < 5; n++) CHECK("ready 8 maxC 5: picks 1..5", sel(0x4000, n, KZ3_KSEL_NONE, 0, 0) == 1);
    CHECK("maxContention 5 reached: 6th SPU idles", sel(0x4000, 5, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    CHECK("contention = 5", S[KZ3_K_CURCONT + 1] == 5);
    inst_reset();
    wkl_add(1, 5, 8);
    S[KZ3_K_READY1 + 1] = 8;
    CHECK("priority byte 0 (SPU 6): never", sel(0x4000, 6, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    CHECK("not this instance's module: never", sel(0x0000, 0, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_STATE1 + 1] = 3;
    CHECK("state 3 (shutting down): never", sel(0x4000, 0, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_STATE1 + 1] = 2;
    S[KZ3_K_ENABLED] = 0;
    CHECK("not enabled: never", sel(0x4000, 0, KZ3_KSEL_NONE, 0, 0) == KZ3_KSEL_NONE);
    /* weight: priority, then the last workload */
    inst_reset();
    wkl_add(0, 5, 7);
    wkl_add(1, 5, 8);
    S[KZ3_K_READY1 + 0] = 2;
    S[KZ3_K_READY1 + 1] = 8;
    CHECK("prio 7 beats 8 (SPU 0)", sel(0xC000, 0, KZ3_KSEL_NONE, 0, 0) == 0);
    CHECK("prio 7 beats 8 (SPU 1)", sel(0xC000, 1, KZ3_KSEL_NONE, 0, 0) == 0);
    CHECK("wid 0 full (ready 2): SPU 2 takes wid 1", sel(0xC000, 2, KZ3_KSEL_NONE, 0, 0) == 1);
    inst_reset();
    wkl_add(4, 5, 8);
    wkl_add(5, 5, 8);
    S[KZ3_K_READY1 + 4] = 1;
    S[KZ3_K_READY1 + 5] = 1;
    CHECK("equal: the SPU's last workload wins", sel(0x0C00, 0, 5, 0, 0) == 5);
    CHECK("equal, no last: lowest wid", sel(0x0C00, 1, KZ3_KSEL_NONE, 0, 0) == 4);
    /* poll */
    inst_reset();
    wkl_add(0, 5, 7);
    wkl_add(1, 5, 8);
    S[KZ3_K_READY1 + 1] = 1;
    CHECK("SPU 0 takes wid 1", sel(0xC000, 0, KZ3_KSEL_NONE, 0, 0) == 1);
    CHECK("poll, still wanted: stay", sel(0xC000, 0, 1, 1, &st) == 1 && st == KZ3_POLL_READY);
    CHECK("poll leaves contention alone", S[KZ3_K_CURCONT + 1] == 1);
    S[KZ3_K_READY1 + 0] = 1;
    CHECK("poll, a higher-priority workload wanted: switch to it", sel(0xC000, 0, 1, 1, 0) == 0);
    CHECK("switch answer leaves contention alone", S[KZ3_K_CURCONT + 0] == 0 && S[KZ3_K_CURCONT + 1] == 1);
    S[KZ3_K_READY1 + 0] = 0;
    S[KZ3_K_READY1 + 1] = 0;
    CHECK("poll, own readyCount 0 and nothing else: none (leave)", sel(0xC000, 0, 1, 1, 0) == KZ3_KSEL_NONE);
    S[KZ3_K_SIGNAL1] = 0x40;
    CHECK("poll, own signal: stay, signal taken",
          sel(0xC000, 0, 1, 1, &st) == 1 && st == KZ3_POLL_SIGNAL && S[KZ3_K_SIGNAL1] == 0);
    /* wake */
    kz3_kwake_init(&g_kw);
    pthread_t th;
    g_woke = 0;
    double t0 = now_ms();
    pthread_create(&th, 0, waiter, 0);
    struct timespec d = {0, 50 * 1000000L};
    nanosleep(&d, 0);
    kz3_kwake_signal(&g_kw);
    pthread_join(th, 0);
    double dt = now_ms() - t0;
    CHECK("wake: woken by the signal", g_woke == 1);
    CHECK("wake: well before the 5 s timeout", dt < 1000.0);
    uint32_t g = kz3_kwake_gen(&g_kw);
    kz3_kwake_signal(&g_kw);
    t0 = now_ms();
    CHECK("wake between gen read and wait: not lost", kz3_kwake_wait(&g_kw, g, 5000) == 1 && now_ms() - t0 < 100.0);
    g = kz3_kwake_gen(&g_kw);
    t0 = now_ms();
    CHECK("no wake: times out", kz3_kwake_wait(&g_kw, g, 20) == 0 && now_ms() - t0 >= 15.0);
    /* stress */
    inst_reset();
    wkl_add(0, 5, 7);
    wkl_add(1, 5, 8);
    S[KZ3_K_READY1 + 0] = 3;
    pthread_t sp[5];
    for (uintptr_t n = 0; n < 5; n++) pthread_create(&sp[n], 0, spu_thread, (void*)n);
    for (int n = 0; n < 5; n++) pthread_join(sp[n], 0);
    CHECK("stress: contention never above readyCount 3", g_maxc <= 3 && g_maxc >= 1);
    CHECK("stress: wid 1 (ready 0) never ran", !g_bad);
    CHECK("stress: contention back to 0", S[KZ3_K_CURCONT + 0] == 0 && S[KZ3_K_CURCONT + 1] == 0);
    printf("stress: max contention %d of readyCount 3\n", g_maxc);
    printf(fails ? "FAIL: %d check(s)\n" : "PASS: per-SPU kernel selection, poll, wake, stress\n", fails);
    return fails ? 1 : 0;
}
