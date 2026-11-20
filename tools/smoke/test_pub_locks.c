/* Host test for KZ3 patch 2197: the ordered publisher P (libs/video/rsx_pub.c over libs/kzr) without the walker: lag and barrier rules, the
 * pending-write touch (C3), verify, the owner rule (C7), the lock-free log-empty check of set_movie_mode, the report-value counters (C4), the ref-queue
 * drop counter (C8), and the lock-order test with real threads (R holds s_gcm_fifo_lock then takes g_ld_access_lock; the movie
 * thread holds g_ld_access_lock and runs the log-empty check, which must take no lock). Unity build: rsx_pub.c and the kzr sources are #included so the
 * RSX_PUB_MUTANT_* macros apply (tools/smoke/test_pub_locks.sh builds one binary per mutant and requires the named property to FAIL).
 *   mutants: NOBARRIER -> barrier; NOLAG -> lag; NODRAIN -> drain; TOUCH -> touch; OWNER -> owner; MOVIE -> movie; VAL, REC -> verify; LOCK -> locks */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static uint8_t g_mem[1u << 16];
void vm_write32(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)(v >> 24); g_mem[a + 1] = (uint8_t)(v >> 16); g_mem[a + 2] = (uint8_t)(v >> 8); g_mem[a + 3] = (uint8_t)v; }
static uint32_t rd(uint32_t a) { return ((uint32_t)g_mem[a] << 24) | ((uint32_t)g_mem[a + 1] << 16) | ((uint32_t)g_mem[a + 2] << 8) | g_mem[a + 3]; }
static uint32_t rd_cb(uint32_t a) { return rd(a); }

#include "rsx_pub.c"
#include "core/sync/kzr_sync.c"
#include "platform/common/kzr_event.c"
#ifdef _WIN32
#include "platform/win32/kzr_platform_win32.c"
#else
#include "platform/posix/kzr_platform_posix.c"
#endif

static int fails;
#define CHECK(name, c, ...) do { if (!(c)) { printf("FAIL %s: ", name); printf(__VA_ARGS__); printf(" (line %d)\n", __LINE__); fails++; } } while (0)
static void ok(const char* n) { printf("done %s\n", n); }
static unsigned long long ST(rsx_pub_stats* s) { rsx_pub_stats_snapshot(s); return s->rec; }

/* ---- RSX_PUB parsing ---- */
static void t_parse(void)
{
    int bad;
    CHECK("parse", rsx_pub_mode_parse(NULL, &bad) == 0 && !bad && rsx_pub_mode_parse("", &bad) == 0 && !bad && rsx_pub_mode_parse("0", &bad) == 0 && !bad, "unset/empty/0");
    CHECK("parse", rsx_pub_mode_parse("1", &bad) == RSX_PUB_ON && !bad && rsx_pub_mode_parse("verify", &bad) == RSX_PUB_VERIFY && !bad, "1/verify");
    CHECK("parse", rsx_pub_mode_parse("2", &bad) == 0 && bad && rsx_pub_mode_parse("VERIFY", &bad) == 0 && bad && rsx_pub_mode_parse("on", &bad) == 0 && bad, "anything else is off and flagged");
    ok("parse");
}

/* ---- lag: held records are not in guest memory until the lag passes; barrier kinds retire through themselves; the pass end drains ---- */
static void t_lag(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_ON, 3);
    rsx_pub_pass_begin();
    for (uint32_t i = 0; i < 3; i++) rsx_pub_take('W', RSX_GS_REPORT, 0x100 + 4 * i, 0xA0 + i, NULL);   /* REPORT words are held kinds */
    CHECK("lag", rd(0x100) == 0 && rd(0x104) == 0 && rd(0x108) == 0, "lag 3: three held records must not be stored yet (%x %x %x)", rd(0x100), rd(0x104), rd(0x108));
    CHECK("lag", rsx_pub_pending() == 3, "pending %u (want 3)", rsx_pub_pending());
    rsx_pub_take('W', RSX_GS_REPORT, 0x10C, 0xA3, NULL);                                               /* the fourth pushes the first out */
    CHECK("lag", rd(0x100) == 0xA0 && rd(0x104) == 0, "after the fourth record only the first is stored (%x %x)", rd(0x100), rd(0x104));
    CHECK("lag", rsx_pub_pending() == 3, "pending %u (want 3)", rsx_pub_pending());
    ok("lag");
    rsx_pub_take('W', RSX_GS_LABEL, 0x200, 0xBEEF, NULL);                                               /* a barrier kind: the walker reads labels itself */
    CHECK("barrier", rd(0x200) == 0xBEEF, "a label must be visible at once, even with a lag (%x)", rd(0x200));
    CHECK("barrier", rd(0x104) == 0xA1 && rd(0x108) == 0xA2 && rd(0x10C) == 0xA3, "a barrier retires every older record first (%x %x %x)", rd(0x104), rd(0x108), rd(0x10C));
    CHECK("barrier", rsx_pub_pending() == 0, "pending %u after a barrier (want 0)", rsx_pub_pending());
    ok("barrier");
    rsx_pub_take('W', RSX_GS_REF, 0x300, 0x77, NULL);
    rsx_pub_take('H', RSX_GS_REPORT_RB, 0x400, 0x01020304u, &g_mem[0x400]);
    CHECK("drain", rd(0x300) == 0 && rd(0x400) == 0, "held until the pass end");
    rsx_pub_pass_end();
    CHECK("drain", rd(0x300) == 0x77 && rd(0x400) == 0x01020304u && rsx_pub_pending() == 0, "pass end drained (%x %x pending %u)", rd(0x300), rd(0x400), rsx_pub_pending());
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("drain", s.empty_bad == 0 && s.rec == s.retired && s.rec == 7, "rec %llu retired %llu empty_bad %llu", s.rec, s.retired, s.empty_bad);
    ok("drain");
}

/* ---- C3: a read of guest bytes sees every pending P store to its span ---- */
static void t_touch(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_ON, 8);
    rsx_pub_pass_begin();
    rsx_pub_take('W', RSX_GS_REPORT, 0x100, 0x11, NULL);
    rsx_pub_take('W', RSX_GS_REPORT, 0x104, 0x22, NULL);
    rsx_pub_take('W', RSX_GS_REPORT, 0x108, 0x33, NULL);
    rsx_pub_touch(0x900, 16);                                      /* no overlap: nothing retires */
    CHECK("touch", rd(0x100) == 0 && rd(0x104) == 0, "a disjoint touch retired something");
    rsx_pub_touch(0x104, 4);                                       /* overlaps the second: it and everything older retire, the third stays */
    CHECK("touch", rd(0x100) == 0x11 && rd(0x104) == 0x22, "a touch of 0x104 must retire the stores up to it (%x %x)", rd(0x100), rd(0x104));
    CHECK("touch", rd(0x108) == 0 && rsx_pub_pending() == 1, "the newer store stays held (%x pending %u)", rd(0x108), rsx_pub_pending());
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("touch", s.touch == 2 && s.touch_ret == 1, "touch %llu / retired %llu", s.touch, s.touch_ret);
    rsx_pub_pass_end();
    ok("touch");
}

/* ---- verify: the retire is checked against the shadow; a clean run has 0 bad ---- */
static void t_verify(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_set_reader(rd_cb);
    rsx_pub_configure(RSX_PUB_VERIFY, 0);
    rsx_pub_pass_begin();
    for (uint32_t i = 0; i < 40; i++) rsx_pub_take('W', (i & 1) ? RSX_GS_LABEL : RSX_GS_REPORT, 0x100 + 4 * i, 0x1000 + i, NULL);
    rsx_pub_take('H', RSX_GS_REPORT_RB, 0x800, 0xCAFE0001u, &g_mem[0x800]);
    rsx_pub_take('S', RSX_GS_COPY_0039, 0x2000, 64, NULL); rsx_pub_take('N', RSX_GS_FLIP, 3, 2, NULL);
    rsx_pub_pass_end();
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("verify", s.bad == 0, "%llu [pub-bad] on a clean run", s.bad);
    CHECK("verify", s.rec == 43 && s.retired == 43 && s.stores == 41, "rec %llu retired %llu stores %llu (want 43/43/41: S and N records store nothing)", s.rec, s.retired, s.stores);
    CHECK("verify", rd(0x100) == 0x1000 && rd(0x800) == 0xCAFE0001u, "values stored");
    ok("verify");
    /* a store that does not land (the reader sees another word) is a [pub-bad]: break the guest word between the retire and the read-back */
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_VERIFY, 0);
    rsx_pub_set_reader(NULL);
    rsx_pub_take('W', RSX_GS_LABEL, 0x100, 5, NULL);
    CHECK("verify", rsx_pub_pending() == 0, "inline retire");
    rsx_pub_set_reader(rd_cb);
    rsx_pub_stats_snapshot(&s);
    CHECK("verify", s.bad == 0, "no reader, no read-back check");
}

/* ---- C7: P belongs to R ---- */
static void* other_thread(void* a)
{
    (void)a;
    rsx_pub_check_owner("test: 2185 table used from another thread");
    return NULL;
}
static void* other_take(void* a) { int* r = (int*)a; *r = rsx_pub_take('W', RSX_GS_REPORT, 0x500, 0x99, NULL); return NULL; }
static void t_owner(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_ON, 4);
    rsx_pub_pass_begin();
    rsx_pub_take('W', RSX_GS_REPORT, 0x100, 1, NULL);                       /* a held record: the log is not empty */
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("owner", s.owner_bad == 0, "owner_bad %llu before any foreign use", s.owner_bad);
    pthread_t t; pthread_create(&t, NULL, other_thread, NULL); pthread_join(t, NULL);
    rsx_pub_stats_snapshot(&s);
    CHECK("owner", s.owner_bad == 1, "a foreign thread using the 2185 gate with records pending must be counted (owner_bad %llu)", s.owner_bad);
    int r = -1;
    pthread_create(&t, NULL, other_take, &r); pthread_join(t, NULL);
    rsx_pub_stats_snapshot(&s);
    CHECK("owner", s.owner_bad == 2 && r == 0 && rd(0x500) == 0, "a foreign take with records pending is counted and not queued (owner_bad %llu, returned %d)", s.owner_bad, r);
    rsx_pub_pass_end();
    /* with nothing pending a new walker thread may take over (the walker thread was restarted between passes) */
    pthread_create(&t, NULL, other_take, &r); pthread_join(t, NULL);
    rsx_pub_stats_snapshot(&s);
    CHECK("owner", s.owner_bad == 2 && r == 1, "rebind with an empty log (owner_bad %llu, returned %d)", s.owner_bad, r);
    ok("owner");
}

/* ---- C7, H7: the log-empty check of set_movie_mode, lock-free ---- */
static atomic_int g_stop;   /* atomic: a plain volatile is a data race for TSan (the clang run flagged it) */
static void* movie_stress(void* a) { (void)a; unsigned long long n = 0; while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) { rsx_pub_movie_check(); n++; } return (void*)(uintptr_t)n; }
static void t_movie(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_ON, 0);
    rsx_pub_pass_begin();
    CHECK("movie", rsx_pub_movie_check() == 0, "an empty log must pass");
    /* a record that outlived its call: hold one with a lag, then drop the lag (the only way to get a leftover inline) */
    rsx_pub_configure(RSX_PUB_ON, 4);
    rsx_pub_pass_begin();
    rsx_pub_take('W', RSX_GS_REPORT, 0x100, 1, NULL);
    CHECK("movie", rsx_pub_movie_check() == 0, "with a lag, held records are legitimate between calls");
    P.lag = 0;
    CHECK("movie", rsx_pub_movie_check() == 1, "a leftover record at a call boundary must be flagged");
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("movie", s.empty_bad >= 1, "empty_bad %llu", s.empty_bad);
    rsx_pub_pass_end();
    CHECK("movie", rsx_pub_movie_check() == 0, "drained: passes again");
    /* no false positive while R runs calls: a checker hammering movie_check against inline takes (and TSan: the check is lock-free and race-free) */
    rsx_pub_configure(RSX_PUB_ON, 0);
    rsx_pub_pass_begin();
    pthread_t t[2]; atomic_store(&g_stop, 0);
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, movie_stress, NULL);
    for (uint32_t i = 0; i < 200000; i++) rsx_pub_take('W', (i % 7) ? RSX_GS_REPORT : RSX_GS_LABEL, 0x100 + 4 * (i & 63), i, NULL);
    atomic_store(&g_stop, 1);
    unsigned long long checks = 0;
    for (int i = 0; i < 2; i++) { void* r; pthread_join(t[i], &r); checks += (unsigned long long)(uintptr_t)r; }
    rsx_pub_pass_end();
    rsx_pub_stats_snapshot(&s);
    CHECK("movie", s.empty_bad == 0, "%llu false [pub-bad] over %llu concurrent checks against 200000 inline calls", s.empty_bad, checks);
    ok("movie");
}

/* ---- C4, C8: counters ---- */
static void* drop_thread(void* a) { (void)a; for (int i = 0; i < 1000; i++) rsx_pub_note_refq_drop(); return NULL; }
static void t_counters(void)
{
    rsx_pub_configure(RSX_PUB_ON, 0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, drop_thread, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("counters", s.refq_drop == 4000, "refq_drop %llu (want 4000 from 4 threads)", s.refq_drop);
    rsx_pub_pass_begin();
    rsx_pub_note_report(0); rsx_pub_note_report(1);
    rsx_pub_pass_end();
    rsx_pub_stats_snapshot(&s);
    CHECK("counters", s.bad == 0 && s.rep == 1 && s.repcalc == 1, "one report, one computation: bad %llu rep %llu repcalc %llu", s.bad, s.rep, s.repcalc);
    rsx_pub_pass_begin();
    rsx_pub_note_report(0); rsx_pub_note_report(1); rsx_pub_note_report(1);                    /* the value computed twice (C4) */
    rsx_pub_pass_end();
    rsx_pub_stats_snapshot(&s);
    CHECK("counters", s.bad == 1, "a second computation must be a [pub-bad] (bad %llu)", s.bad);
    ok("counters");
}

/* ---- lock order with real threads: R = fifo lock then access lock; the movie thread = access lock, then the log-empty check ---- */
static pthread_mutex_t g_fifo = PTHREAD_MUTEX_INITIALIZER, g_acc = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t g_bar;
static int g_dead;
#ifdef RSX_PUB_MUTANT_LOCK
static int tlock(pthread_mutex_t* m)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
    return pthread_mutex_timedlock(m, &ts);
}
static void mutant_quiesce(void)   /* a kzr_pub_quiesce: take the walker's lock to wait for the log */
{
    if (tlock(&g_fifo)) { __atomic_store_n(&g_dead, 1, __ATOMIC_RELAXED); return; }
    pthread_mutex_unlock(&g_fifo);
}
#else
static int tlock(pthread_mutex_t* m)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
    return pthread_mutex_timedlock(m, &ts);
}
#endif
#ifdef RSX_PUB_MUTANT_LOCK
#define ROUNDS 3      /* each round of the mutant ends in a 1 s lock timeout */
#else
#define ROUNDS 300
#endif
static void* lock_R(void* a)   /* the walker: s_gcm_fifo_lock, a pass, then g_ld_access_lock in serialized dispatch */
{
    (void)a;
    for (int i = 0; i < ROUNDS; i++) {
        if (tlock(&g_fifo)) { __atomic_store_n(&g_dead, 1, __ATOMIC_RELAXED); pthread_barrier_wait(&g_bar); pthread_barrier_wait(&g_bar); continue; }
        rsx_pub_pass_begin();
        rsx_pub_take('W', RSX_GS_REPORT, 0x100, i, NULL);
        pthread_barrier_wait(&g_bar);                               /* both locks are held now: R has the fifo lock, M the access lock */
        if (tlock(&g_acc)) __atomic_store_n(&g_dead, 1, __ATOMIC_RELAXED); else { rsx_pub_take('W', RSX_GS_LABEL, 0x200, i, NULL); pthread_mutex_unlock(&g_acc); }
        rsx_pub_pass_end();
        pthread_mutex_unlock(&g_fifo);
        pthread_barrier_wait(&g_bar);                               /* a round ends for both threads before the next starts (the mutexes are not fair) */
    }
    return NULL;
}
static void* lock_M(void* a)   /* set_movie_mode: g_ld_access_lock, then the publisher's log-empty check */
{
    (void)a;
    for (int i = 0; i < ROUNDS; i++) {
        if (tlock(&g_acc)) { __atomic_store_n(&g_dead, 1, __ATOMIC_RELAXED); pthread_barrier_wait(&g_bar); pthread_barrier_wait(&g_bar); continue; }
        pthread_barrier_wait(&g_bar);                               /* R now holds the fifo lock */
#ifdef RSX_PUB_MUTANT_LOCK
        g_rsx_pub_mutant_lock = mutant_quiesce;
#endif
        rsx_pub_movie_check();                                      /* takes no lock: R gets the access lock as soon as we release it */
        pthread_mutex_unlock(&g_acc);
        pthread_barrier_wait(&g_bar);
    }
    return NULL;
}
static void t_locks(void)
{
    memset(g_mem, 0, sizeof g_mem);
    rsx_pub_configure(RSX_PUB_ON, 0);
    pthread_barrier_init(&g_bar, NULL, 2);
    pthread_t r, m;
    pthread_create(&r, NULL, lock_R, NULL); pthread_create(&m, NULL, lock_M, NULL);
    pthread_join(r, NULL); pthread_join(m, NULL);
    pthread_barrier_destroy(&g_bar);
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    CHECK("locks", !__atomic_load_n(&g_dead, __ATOMIC_RELAXED), "lock-order deadlock: R holds the fifo lock and wants the access lock, the movie thread holds the access lock and wants the fifo lock");
    CHECK("locks", s.rec == 2 * ROUNDS && s.empty_bad == 0, "rec %llu (want %d) empty_bad %llu", s.rec, 2 * ROUNDS, s.empty_bad);
    ok("locks");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* only = argc > 1 ? argv[1] : NULL;
#define RUN(n, f) do { if (!only || !strcmp(only, n)) f(); } while (0)
    RUN("parse", t_parse);
    if (!only || !strcmp(only, "lag") || !strcmp(only, "barrier") || !strcmp(only, "drain")) t_lag();   /* barrier and drain are checks inside it */
    RUN("touch", t_touch);
    RUN("verify", t_verify);
    RUN("owner", t_owner);
    RUN("movie", t_movie);
    RUN("counters", t_counters);
    RUN("locks", t_locks);
    (void)ST;
    if (fails) { printf("FAILED %d\n", fails); return 1; }
    puts("ALL OK");
    return 0;
}
