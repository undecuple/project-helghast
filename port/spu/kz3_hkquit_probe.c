/* KZ3 Havok quit probe (SPU pass 57; the RE notes s.7/s.8): KZ3_JOIN2_PROBE=1, 1.00 only, logs only.
 *
 * T7 (quit to the main menu) stalls in cellSpursJoinTask2(taskset, 0) inside the hkSpuJobThreadPool destructor
 * 0x8E3C10 -> hkPs3SpuUtil::destroyTaskset 0x8E0334. Hooks (layouts: the RE notes s.2):
 *   0x8E3DAC processAllJobs(pool, queue, type)   pool+0x20 = queue, busy (pool+0x14) = 1, CreateTask2 x numTasks
 *   0x8E3E8C waitForCompletion(pool)             acquire(pool+0x1C) x numTasks, TryJoinTask2 0..127, busy = 0
 *   0x8E3C10 ~hkSpuJobThreadPool(pool)           pool+8 = util (util+8 = taskset), destroyTaskset(util) first
 *   0x8E0334 destroyTaskset(util)                JoinTask2(ts, 0..127), DestroyTaskset2
 *   0x8E89A0 / 0x8E88E4 semaphore release / acquire (counts only)
 * Per pool: processAllJobs / waitForCompletion counts, the last call of each (tid, ms); a ring of the last 32 P/W
 * events; the pool, its completion semaphore and its job queue (raw words) at the dtor and from the JoinTask2 spin
 * dump (patch 1072 calls g_kz3_j2_guest_dump). 1.14 rows: 0x90FDBC, 0x90FE9C, 0x90FC20, 0x90C444, 0x914BD0, 0x914B14
 * (not hooked: 1.00 only). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

extern uint8_t* vm_base;
extern void (*g_kz3_j2_guest_dump)(uint32_t ts, uint32_t id);

static int g_on = -1;
static volatile long g_rel, g_acq, g_lines;

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + t.tv_nsec / 1000000;
#endif
}
static unsigned long tid(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)pthread_self();
#endif
}
static int ok(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0xF0000000u;
}
static uint32_t r32(uint32_t ea)
{
    if (!ok(ea)) return 0xDEADDEADu;
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static int line_ok(void)
{
    return __sync_add_and_fetch(&g_lines, 1) <= 300;
}

typedef struct {
    uint32_t pool, ts;
    long p, w;
    unsigned long p_tid, w_tid;
    unsigned long long p_ms, w_ms;
    int w_open; /* a waitForCompletion has entered and not returned */
} pool_rec;
#define NPOOL 8
static pool_rec s_pool[NPOOL];
static volatile long s_pool_lock;
typedef struct {
    unsigned long long ms;
    uint32_t pool, kind; /* 1 processAllJobs, 2 waitForCompletion enter, 3 wait return, 4 dtor, 5 destroyTaskset */
    unsigned long tid;
} pw_ev;
#define NEV 32
static pw_ev s_ev[NEV];
static volatile long s_ev_n;

static void lock(void)
{
    while (__sync_lock_test_and_set(&s_pool_lock, 1))
#ifdef _WIN32
        Sleep(0);
#else
        ;
#endif
}
static void unlock(void)
{
    __sync_lock_release(&s_pool_lock);
}
static pool_rec* rec(uint32_t pool)
{
    for (int i = 0; i < NPOOL; i++)
        if (s_pool[i].pool == pool) return &s_pool[i];
    for (int i = 0; i < NPOOL; i++)
        if (!s_pool[i].pool) {
            s_pool[i].pool = pool;
            return &s_pool[i];
        }
    return NULL;
}
static void ev(uint32_t pool, uint32_t kind)
{
    long k = __sync_fetch_and_add(&s_ev_n, 1);
    pw_ev* e = &s_ev[k % NEV];
    e->ms = now_ms();
    e->pool = pool;
    e->kind = kind;
    e->tid = tid();
}

static void words(const char* tag, uint32_t ea, unsigned n)
{
    char b[200];
    int o = 0;
    for (unsigned i = 0; i < n && o < (int)sizeof b - 10; i++)
        o += snprintf(b + o, sizeof b - o, " %08X", r32(ea + 4 * i));
    fprintf(stderr, "[hkq]   %s @0x%08X:%s\n", tag, ea, b);
}

static void dump_pool(uint32_t pool)
{
    if (!ok(pool)) return;
    const uint32_t util = r32(pool + 8), q = r32(pool + 0x20), sem = r32(pool + 0x1C);
    fprintf(stderr,
            "[hkq] pool 0x%08X vt=0x%08X util=0x%08X ts=0x%08X numTasks=%u/%u busy=%u sem=0x%08X queue=0x%08X\n", pool,
            r32(pool), util, ok(util) ? r32(util + 8) : 0, r32(pool + 0xC), r32(pool + 0x10),
            ok(pool) ? vm_base[pool + 0x14] : 0, sem, q);
    if (ok(sem)) words("sem", sem, 4);
    if (ok(q)) {
        words("queue+0x00", q, 8);
        words("queue+0x20", q + 0x20, 8);
        words("queue+0x80", q + 0x80, 8);
        words("queue+0xA0", q + 0xA0, 8);
        const uint32_t data = r32(q + 0x30);
        if (ok(data)) {
            words("data+0x00", data, 8);
            words("data+0x20", data + 0x20, 8);
            words("data+0x40", data + 0x40, 8);
            words("data+0x60", data + 0x60, 8);
        }
        for (unsigned t = 0; t < 11; t++) {
            const uint32_t s = r32(q + 0x90 + 4 * t);
            if (ok(s)) {
                char tag[24];
                snprintf(tag, sizeof tag, "sem[%u]", t);
                words(tag, s, 4);
            }
        }
    }
}

static void dump_all(const char* why)
{
    const unsigned long long now = now_ms();
    fprintf(stderr, "[hkq] --- %s: sem release=%ld acquire=%ld ---\n", why, g_rel, g_acq);
    lock();
    for (int i = 0; i < NPOOL; i++) {
        pool_rec* r = &s_pool[i];
        if (!r->pool) continue;
        fprintf(stderr,
                "[hkq] pool 0x%08X: processAllJobs=%ld (last tid %lu, -%llu ms) waitForCompletion=%ld (last tid %lu, "
                "-%llu ms)%s\n",
                r->pool, r->p, r->p_tid, r->p ? now - r->p_ms : 0, r->w, r->w_tid, r->w ? now - r->w_ms : 0,
                r->w_open ? " WAIT OPEN" : "");
    }
    unlock();
    for (int i = 0; i < NPOOL; i++)
        if (s_pool[i].pool) dump_pool(s_pool[i].pool);
    const long n = s_ev_n;
    for (long k = n > NEV ? n - NEV : 0; k < n; k++) {
        pw_ev* e = &s_ev[k % NEV];
        fprintf(stderr, "[hkq]   ev -%llu ms pool=0x%08X kind=%u tid=%lu\n", now - e->ms, e->pool, e->kind, e->tid);
    }
    fflush(stderr);
}

static void guest_dump(uint32_t ts, uint32_t id)
{
    char why[64];
    snprintf(why, sizeof why, "JoinTask2 spin ts=0x%08X id=%u", ts, id);
    dump_all(why);
}

int kz3_hkq_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_JOIN2_PROBE");
        g_on = e && *e == '1';
        if (g_on) g_kz3_j2_guest_dump = guest_dump;
    }
    return g_on;
}

void kz3_hkq_sem(int release)
{
    if (release)
        __sync_fetch_and_add(&g_rel, 1);
    else
        __sync_fetch_and_add(&g_acq, 1);
}

void kz3_hkq_process(uint32_t pool, uint32_t queue, uint32_t type, uint32_t lr)
{
    long n;
    lock();
    pool_rec* r = rec(pool);
    if (r) {
        n = ++r->p;
        r->p_tid = tid();
        r->p_ms = now_ms();
    } else
        n = 0;
    unlock();
    ev(pool, 1);
    if (n <= 2 && line_ok())
        fprintf(stderr, "[hkq] processAllJobs #%ld pool=0x%08X queue=0x%08X type=%u lr=0x%08X tid=%lu\n", n, pool,
                queue, type, lr, tid());
}

void kz3_hkq_wait(uint32_t pool, int after)
{
    lock();
    pool_rec* r = rec(pool);
    if (r) {
        if (!after) {
            r->w++;
            r->w_tid = tid();
            r->w_ms = now_ms();
            r->w_open = 1;
        } else
            r->w_open = 0;
    }
    unlock();
    ev(pool, after ? 3 : 2);
}

void kz3_hkq_dtor(uint32_t pool, uint32_t lr, int after)
{
    ev(pool, after ? 6 : 4);
    if (!line_ok()) return;
    if (!after) {
        fprintf(stderr, "[hkq] ~hkSpuJobThreadPool enter pool=0x%08X lr=0x%08X tid=%lu\n", pool, lr, tid());
        dump_all("pool dtor");
    } else
        fprintf(stderr, "[hkq] ~hkSpuJobThreadPool done pool=0x%08X tid=%lu\n", pool, tid());
    fflush(stderr);
}

void kz3_hkq_destroy(uint32_t util, uint32_t lr, int after)
{
    ev(util, after ? 7 : 5);
    if (line_ok())
        fprintf(stderr, "[hkq] destroyTaskset %s util=0x%08X ts=0x%08X lr=0x%08X tid=%lu\n", after ? "done" : "enter",
                util, ok(util) ? r32(util + 8) : 0, lr, tid());
}
