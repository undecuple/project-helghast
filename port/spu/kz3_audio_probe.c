/* KZ3 audio tick probe (SPU pass 37; RE pass 78-79, the RE notes sections 1-4): KZ3_AUDIO_TICK=1.
 *
 * The port-0 ring producer is the PPU thread brbDriver (entry 0xD42C7C; 1.14 0xD92638), paced by a 5333 us lv2
 * periodic timer: per tick it blocks in sys_event_queue_receive (inline sc 0x82 at 0xD42D2C), then writes every unit
 * between its cursor `last` (r26) and the published read index `pos`, one mix step (0xD44810; 1.14 0xD941D0) each.
 * kz3_ppu_overrides.cpp wraps the brbDriver entry and the mix step and, on the first brbDriver call, the lv2 handlers
 * of sc 0x82 (receive) and 0x8d (usleep); the wrappers call into this file:
 *  - per tick: interval between receive returns, wake latency (timer push -> receive return, patch 1054's
 *    sys_timer_last_send_qpc), delta = units to write this tick (the producer's lag), processing time (receive return ->
 *    next receive entry);
 *  - per mix step: time and the number of usleep(1000) polls inside (the wait for the previous half's SPU job list);
 *  - the QPC at which each ring slot was last written (g_kz3_audio_slot_wqpc, patch 1054 in cellAudio.c) so the
 *    mixer's KZ3_AUDIO_DIAG hole lines carry the slot's age.
 * [audio-tick] once with the driver object's fields (G/L/R/period), then every 10 s. Off: no wrapper is installed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern volatile long long g_kz3_audio_slot_wqpc[32];      /* patch 1054, cellAudio.c */
extern int64_t sys_timer_last_send_qpc(int32_t queue_id); /* patch 1054, sys_timer.c */

static int g_on = -1;

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static uint64_t rd64(uint32_t ea)
{
    uint64_t v;
    memcpy(&v, vm_base + ea, 8);
    return __builtin_bswap64(v);
}
static float rdf(uint32_t ea)
{
    uint32_t u = rd32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static int guest_ok(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0xF0000000u; /* the port's read index lives at 0x58100000 */
}
static long long qpc(void)
{
#ifdef _WIN32
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
#else
    return 0;
#endif
}
static long long qfreq(void)
{
#ifdef _WIN32
    static long long f;
    if (!f) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    return f;
#else
    return 1;
#endif
}
static long long to_us(long long d)
{
    return d * 1000000 / qfreq();
}

int kz3_audio_tick_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_AUDIO_TICK");
        const char* s = getenv("KZ3_AUDIO_SPU");   /* pass 39: the list timeline needs the same wrappers */
        g_on = ((e && *e && *e != '0') || (s && *s && *s != '0')) ? 1 : 0;
    }
    return g_on;
}

/* tick state (brbDriver thread only) */
static long long t_start, t_line, t_prev_ret, t_pre;
static uint32_t s_prev_last = 0xFFFFFFFFu, s_obj;
static unsigned long long n_ticks, int_h[6], d_h[6], d_max, wake_n, wake_sum_us, wake_max_us, proc_n, proc_sum_us,
    proc_max_us, int_max_us;
/* mix-step state */
static unsigned long long u_n, u_h[5], u_sum_us, u_max_us, u_sleeps, u_sleep_max;

void kz3_audio_tick_pre(uint32_t last)
{
    const long long now = qpc();
    t_pre = now;
    if (t_prev_ret) {
        long long p = to_us(now - t_prev_ret);
        proc_n++;
        proc_sum_us += (unsigned long long)p;
        if ((unsigned long long)p > proc_max_us) proc_max_us = (unsigned long long)p;
    }
    /* the units between the previous cursor and this one were written during the tick that just ended */
    if (s_obj && s_prev_last != 0xFFFFFFFFu && last != 0xFFFFFFFFu) {
        const uint32_t R = rd32(s_obj + 0x78);
        const float upb = rdf(s_obj + 0x88); /* units per block = 256 / G; the slot = unit only when it is 1 */
        if (R && R <= 32 && upb == 1.0f)
            for (uint32_t u = s_prev_last, k = 0; u != last && k < R; u = (u + 1) & (R - 1), k++)
                g_kz3_audio_slot_wqpc[u] = now;
    }
    s_prev_last = last;
}

void kz3_audio_tick_post(uint32_t obj, uint32_t last)
{
    const long long now = qpc();
    if (!t_start) {
        t_start = now;
        t_line = now + 10 * qfreq();
    }
    if (!s_obj && guest_ok(obj)) {
        s_obj = obj;
        fprintf(stderr,
                "[audio-tick] brbDriver obj=0x%08X +0x4C queue=%u +0x70=%u +0x74 L=%u +0x78 R=%u +0x80=0x%08X "
                "+0x88 units/block=%.4f readidx@0x%08X\n",
                obj, rd32(obj + 0x4C), rd32(obj + 0x70), rd32(obj + 0x74), rd32(obj + 0x78), rd32(obj + 0x80),
                (double)rdf(obj + 0x88), rd32(obj + 0x68));
    }
    if (!s_obj) return;
    n_ticks++;
    if (t_prev_ret) {
        const long long iv = to_us(now - t_prev_ret);
        int_h[iv < 4000 ? 0 : iv < 6000 ? 1 : iv < 8000 ? 2 : iv < 12000 ? 3 : iv < 25000 ? 4 : 5]++;
        if ((unsigned long long)iv > int_max_us) int_max_us = (unsigned long long)iv;
    }
    t_prev_ret = now;
    const long long ts = sys_timer_last_send_qpc((int32_t)rd32(s_obj + 0x4C));
    if (ts > 0 && ts <= now) {
        const long long w = to_us(now - ts);
        wake_n++;
        wake_sum_us += (unsigned long long)w;
        if ((unsigned long long)w > wake_max_us) wake_max_us = (unsigned long long)w;
    }
    /* delta as the loop computes it (0xD42D44-0xD42DB0): pos = (u32)(readIndex * units/block), mod R */
    const uint32_t ria = rd32(s_obj + 0x68), R = rd32(s_obj + 0x78);
    if (guest_ok(ria) && R && last != 0xFFFFFFFFu) {
        const uint32_t pos = (uint32_t)((float)(int64_t)rd64(ria) * rdf(s_obj + 0x88));
        uint32_t d = last <= pos ? pos - last : pos + R - last;
        if (d >= R) d = 0;
        d_h[d == 0 ? 0 : d == 1 ? 1 : d == 2 ? 2 : d == 3 ? 3 : d < 7 ? 4 : 5]++;
        if (d > d_max) d_max = d;
    }
    if (now >= t_line) {
        t_line = now + 10 * qfreq();
        fprintf(
            stderr,
            "[audio-tick] t=%.1f ticks=%llu interval_ms <4=%llu 4-6=%llu 6-8=%llu 8-12=%llu 12-25=%llu >=25=%llu "
            "max=%.1f | wake_us avg=%llu max=%llu | delta 0=%llu 1=%llu 2=%llu 3=%llu 4-6=%llu >=7=%llu max=%llu | "
            "proc_us avg=%llu max=%llu | units=%llu unit_ms <1=%llu 1-2=%llu 2-5=%llu 5-10=%llu >=10=%llu avg_us=%llu "
            "max_us=%llu usleeps=%llu max/unit=%llu\n",
            (double)to_us(now - t_start) / 1e6, n_ticks, int_h[0], int_h[1], int_h[2], int_h[3], int_h[4], int_h[5],
            (double)int_max_us / 1000.0, wake_n ? wake_sum_us / wake_n : 0, wake_max_us, d_h[0], d_h[1], d_h[2], d_h[3],
            d_h[4], d_h[5], d_max, proc_n ? proc_sum_us / proc_n : 0, proc_max_us, u_n, u_h[0], u_h[1], u_h[2], u_h[3],
            u_h[4], u_n ? u_sum_us / u_n : 0, u_max_us, u_sleeps, u_sleep_max);
        n_ticks = wake_n = wake_sum_us = wake_max_us = proc_n = proc_sum_us = proc_max_us = int_max_us = d_max = 0;
        memset(int_h, 0, sizeof int_h);
        memset(d_h, 0, sizeof d_h);
        memset(u_h, 0, sizeof u_h);
        u_n = u_sum_us = u_max_us = u_sleeps = u_sleep_max = 0;
    }
}

/* mix step 0xD44810: called with the elapsed host ticks and the usleep count of one call */
static long long u_t0;
void kz3_audio_spu_unit_enter(void);
void kz3_audio_spu_unit_exit(void);
void kz3_audio_unit_enter(void)
{
    u_t0 = qpc();
    kz3_audio_spu_unit_enter();
}
void kz3_audio_unit_exit(unsigned sleeps)
{
    const long long us = to_us(qpc() - u_t0);
    u_n++;
    u_sum_us += (unsigned long long)us;
    u_h[us < 1000 ? 0 : us < 2000 ? 1 : us < 5000 ? 2 : us < 10000 ? 3 : 4]++;
    if ((unsigned long long)us > u_max_us) u_max_us = (unsigned long long)us;
    u_sleeps += sleeps;
    if (sleeps > u_sleep_max) u_sleep_max = sleeps;
    kz3_audio_spu_unit_exit();
}

/* ---- Pass 39 (the RE notes 5.3, probes P1/P3/P4): KZ3_AUDIO_SPU=1, the wid 9 job-list timeline. ----
 * Turns the tick probe's wrappers on too (kz3_audio_tick_on). Sources, all on the host QPC clock:
 *  - kick 0xA78818(handle, 1) (1.14 0xA8F728) on the brbDriver thread: kick time, list index hdr+8 (handle = {+0 hdr,
 *    +4 SPURS EA, +8 wid}); one record per kick in a 64-entry ring;
 *  - list poll 0xA78E54(handle, t) (1.14 0xA8FD64) on the brbDriver thread, per call: the poll's outcome, told apart by
 *    a sentinel in the callee's workload-info buffer (frame -0x180, buffer +0x70; bytes +0x20 contention, +0x23 ready
 *    count): A = returned 1 because hdr+8 != t (list not in use), B = returned 1 with both info bytes 0, C = returned 0
 *    before reading the info (job counters below the target or the entry walk changed), D = returned 0 on the info
 *    (contention or ready count non-zero); entry count of list t (walk *(hdr+0x30+4t) in 8-byte entries while
 *    u16 +2 != 0) and the row u16 hdr+0x40+16t +0 (ticket) .. +0xE at the first poll of a step;
 *  - SPU job entries of images 259/260/263 (patch 1044's entry hook, the module runs unchanged): first / last job
 *    start after each kick, jobs per kick, per-image interval to the next entry on the same host thread inside one
 *    PM run (ASSUMPTION: = the job's duration plus the PM's hand-off; a run's last job is not counted);
 *  - g_kz3_pm_run_hook (patch 1057) for the LoPrio instance's wid 9: PM runs per kick, the end of the first run
 *    that ended after the kick's last job start;
 *  - SPU PUTs into port 0's ring 0x58000000 + slot*0x2000 (8 slots; cellAudio.c) through the chained put hook:
 *    size, image of the job running on that thread, the slot's distance ahead of the mixer's read index (u64 at
 *    0x58100000), and the PUT time minus the PPU step's write mark for that slot (g_kz3_audio_slot_wqpc). */
struct spu_context;
extern void spu_overlay_set_entry_hook(int image_id, int (*fn)(struct spu_context*, int, void*), void* user);
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
extern void (*g_kz3_pm_run_hook)(uint32_t spurs_ea, uint32_t wid, int end);
#define KZ3_TLS __thread

static int g_spu_on = -1;
int kz3_audio_spu_on(void)
{
    if (g_spu_on < 0) {
        const char* e = getenv("KZ3_AUDIO_SPU");
        g_spu_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_spu_on;
}

struct kz3_kick {
    long long kick, first_job, last_job, pm_end;
    volatile unsigned jobs, runs;
    unsigned list;
};
static struct kz3_kick s_k[64];
static volatile unsigned s_kseq;          /* latest kick (index into s_k & 63), 0 = none yet */
static unsigned s_kick_of[2];             /* latest kick of list 0 / 1 */
static uint32_t s_lo_spurs, s_lo_wid = 0xFFFFFFFFu;
static volatile long s_run_gen;

/* per-image job intervals, jobs, ring PUTs */
static volatile long long s_img_us[3], s_img_n[3], s_img_jobs[3];
static int img_slot(int img) { return img == 259 ? 0 : img == 260 ? 1 : img == 263 ? 2 : -1; }
static KZ3_TLS long long t_job_t0;
static KZ3_TLS int t_job_img;
static KZ3_TLS long t_job_gen;

static int kz3_audio_job_entry(struct spu_context* ctx, int img, void* user)
{
    (void)ctx; (void)user;
    const long long now = qpc();
    const int si = img_slot(img);
    if (si < 0) return 0;
    if (t_job_t0 && now - t_job_t0 < qfreq() / 500000) {   /* < 2 us: the stale-resident call before the real one */
        t_job_img = img;
        return 0;
    }
    if (t_job_t0 && t_job_gen == s_run_gen) {
        const int ps = img_slot(t_job_img);
        if (ps >= 0) { __atomic_add_fetch(&s_img_us[ps], to_us(now - t_job_t0), __ATOMIC_RELAXED);
                       __atomic_add_fetch(&s_img_n[ps], 1, __ATOMIC_RELAXED); }
    }
    t_job_t0 = now; t_job_img = img; t_job_gen = s_run_gen;
    __atomic_add_fetch(&s_img_jobs[si], 1, __ATOMIC_RELAXED);
    const unsigned q = s_kseq;
    if (q) {
        struct kz3_kick* k = &s_k[q & 63];
        if (!k->first_job) k->first_job = now;
        k->last_job = now;
        __atomic_add_fetch(&k->jobs, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static KZ3_TLS unsigned t_run_q, t_run_jobs;
static void kz3_audio_pm_run(uint32_t spurs_ea, uint32_t wid, int end)
{
    if (spurs_ea != s_lo_spurs || wid != s_lo_wid) return;
    const unsigned q = s_kseq;
    if (!q) return;
    struct kz3_kick* k = &s_k[q & 63];
    if (!end) {
        __atomic_add_fetch(&s_run_gen, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&k->runs, 1, __ATOMIC_RELAXED);
        t_run_q = q; t_run_jobs = k->jobs;
        return;
    }
    /* the end of the last run (on this kernel thread) under this kick in which a job started */
    if (t_run_q == q && k->jobs != t_run_jobs) k->pm_end = qpc();
}

/* ring PUTs */
static void (*s_put_next)(uint32_t, uint32_t);
static volatile long long s_rp_n, s_rp_2000, s_rp_img[4], s_rp_dist[8], s_rp_lag_n, s_rp_lag_us, s_rp_lag_neg,
    s_rp_other2000;
static uint32_t s_rp_other_ea[4];
static void kz3_audio_put(uint32_t ea, uint32_t size)
{
    if (ea >= 0x58000000u && ea < 0x58010000u) {
        const unsigned slot = (ea - 0x58000000u) / 0x2000u;
        __atomic_add_fetch(&s_rp_n, 1, __ATOMIC_RELAXED);
        if (size == 0x2000) __atomic_add_fetch(&s_rp_2000, 1, __ATOMIC_RELAXED);
        const int si = img_slot(t_job_img);
        __atomic_add_fetch(&s_rp_img[si < 0 ? 3 : si], 1, __ATOMIC_RELAXED);
        const unsigned ridx = (unsigned)(rd64(0x58100000u) & 7u);
        __atomic_add_fetch(&s_rp_dist[(slot - ridx) & 7u], 1, __ATOMIC_RELAXED);
        const long long w = g_kz3_audio_slot_wqpc[slot & 31];
        if (w) {
            const long long d = to_us(qpc() - w);
            if (d >= 0) { __atomic_add_fetch(&s_rp_lag_n, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&s_rp_lag_us, d, __ATOMIC_RELAXED); }
            else __atomic_add_fetch(&s_rp_lag_neg, 1, __ATOMIC_RELAXED);
        }
    } else if (size == 0x2000 && img_slot(t_job_img) >= 0) {
        const long long n = __atomic_add_fetch(&s_rp_other2000, 1, __ATOMIC_RELAXED);
        if (n <= 4) s_rp_other_ea[n - 1] = ea;
    }
    if (s_put_next) s_put_next(ea, size);
}

/* list helpers (hdr layout: the RE notes 5.1) */
static unsigned list_entries(uint32_t hdr, unsigned t)
{
    const uint32_t arr = rd32(hdr + 0x30 + 4 * (t & 1));
    unsigned n = 0;
    if (!guest_ok(arr)) return 0;
    while (n < 4096) {
        uint16_t v;
        memcpy(&v, vm_base + arr + 8 * n + 2, 2);
        if (!v) break;
        n++;
    }
    return n;
}

void kz3_audio_kick(uint32_t handle)
{
    if (!kz3_audio_spu_on() || !guest_ok(handle)) return;
    const uint32_t hdr = rd32(handle);
    if (!s_lo_spurs) {
        s_lo_spurs = rd32(handle + 4);
        s_lo_wid = rd32(handle + 8);
        fprintf(stderr, "[audio-spu] probe on: list handle=0x%08X hdr=0x%08X spurs=0x%08X wid=%u\n", handle, hdr,
                s_lo_spurs, s_lo_wid);
        g_kz3_pm_run_hook = kz3_audio_pm_run;
        spu_overlay_set_entry_hook(259, kz3_audio_job_entry, NULL);
        spu_overlay_set_entry_hook(260, kz3_audio_job_entry, NULL);
        spu_overlay_set_entry_hook(263, kz3_audio_job_entry, NULL);
        if (g_spu_put_hook != kz3_audio_put) { s_put_next = g_spu_put_hook; g_spu_put_hook = kz3_audio_put; }
    }
    const unsigned list = guest_ok(hdr) ? rd32(hdr + 8) & 1 : 0;
    const unsigned q = s_kseq + 1;
    struct kz3_kick* k = &s_k[q & 63];
    memset(k, 0, sizeof *k);
    k->list = list;
    k->kick = qpc();
    s_kick_of[list] = q;
    __atomic_store_n(&s_kseq, q, __ATOMIC_RELEASE);
}

/* per step (brbDriver thread): polls of 0xA78E54 between kz3_audio_unit_enter and _exit */
static long long p_enter, p_jobs_done, p_ret1;
static unsigned p_polls, p_cls_last, p_entries, p_list, p_ticket, p_row[7];
static unsigned long long a_units, a_cls[5], a_d_ready, a_d_cont, a_wait_units, a_lines;
static unsigned long long a_n, a_entries, a_kick_first, a_first_last, a_last_pmend, a_pmend_ret, a_kick_ret, a_runs,
    a_jobs, a_wait_us, a_wait_max, a_kick_ret_max, a_jobspan_max, a_info_wait, a_info_n;
/* pre: before the lifted poll runs (info = the callee's buffer below the caller's sp: dead stack, safe to write) */
void kz3_audio_poll_pre(uint32_t info)
{
    if (!kz3_audio_spu_on() || !guest_ok(info)) return;
    vm_base[info + 0x20] = 0xEE;
    vm_base[info + 0x23] = 0xEE;
}
void kz3_audio_poll_post(uint32_t handle, uint32_t t, uint32_t info, uint32_t ret)
{
    if (!kz3_audio_spu_on() || !guest_ok(handle) || !guest_ok(info)) return;
    const long long now = qpc();
    const uint32_t hdr = rd32(handle);
    if (!guest_ok(hdr)) return;
    const unsigned inuse = rd32(hdr + 8), r = ret & 1;
    const unsigned c20 = vm_base[info + 0x20], c23 = vm_base[info + 0x23];
    unsigned cls;
    if (r) cls = (inuse != t) ? 0 : 1;            /* A, B */
    else if (c20 == 0xEE && c23 == 0xEE) cls = 2; /* C */
    else { cls = 3; if (c23) a_d_ready++; if (c20) a_d_cont++; }   /* D */
    if (!p_polls) {
        p_list = t & 1;
        p_entries = list_entries(hdr, t);
        uint16_t row[8];
        memcpy(row, vm_base + hdr + 0x40 + 16 * (t & 1), 16);
        p_ticket = __builtin_bswap16(row[0]);
        for (int i = 0; i < 7; i++) p_row[i] = __builtin_bswap16(row[i + 1]);
    }
    p_polls++;
    if (cls != 2 && !p_jobs_done) p_jobs_done = now;
    if (r && !p_ret1) p_ret1 = now;
    p_cls_last = cls;
}

static long long s_spu_line;
void kz3_audio_spu_unit_enter(void)
{
    p_enter = qpc();
    p_polls = 0; p_jobs_done = p_ret1 = 0; p_cls_last = 4;
}
void kz3_audio_spu_unit_exit(void)
{
    if (!kz3_audio_spu_on() || !s_lo_spurs) return;
    const long long now = qpc();
    a_units++;
    a_cls[p_polls ? p_cls_last : 4]++;
    const long long wait = p_ret1 ? to_us(p_ret1 - p_enter) : 0;
    if (p_polls > 1) a_wait_units++;
    const unsigned q = s_kick_of[p_list];
    const struct kz3_kick* k = q ? &s_k[q & 63] : NULL;
    if (p_polls && p_ret1 && k && k->kick && k->kick < p_ret1) {
        a_n++;
        a_entries += p_entries;
        a_jobs += k->jobs;
        a_runs += k->runs;
        a_wait_us += (unsigned long long)wait;
        if (p_jobs_done && p_ret1 >= p_jobs_done) { a_info_wait += (unsigned long long)to_us(p_ret1 - p_jobs_done); a_info_n++; }
        if ((unsigned long long)wait > a_wait_max) a_wait_max = (unsigned long long)wait;
        const long long kr = to_us(p_ret1 - k->kick);
        a_kick_ret += (unsigned long long)kr;
        if ((unsigned long long)kr > a_kick_ret_max) a_kick_ret_max = (unsigned long long)kr;
        if (k->first_job) {
            a_kick_first += (unsigned long long)to_us(k->first_job - k->kick);
            const long long span = to_us(k->last_job - k->first_job);
            a_first_last += (unsigned long long)span;
            if ((unsigned long long)span > a_jobspan_max) a_jobspan_max = (unsigned long long)span;
            if (k->pm_end && k->pm_end >= k->last_job) {
                a_last_pmend += (unsigned long long)to_us(k->pm_end - k->last_job);
                if (p_ret1 >= k->pm_end) a_pmend_ret += (unsigned long long)to_us(p_ret1 - k->pm_end);
            }
        }
        /* one detail line per 200 steps and every step that waited 20 ms or more, at most 150 */
        if (a_lines < 150 && ((a_units % 200) == 1 || wait >= 20000)) {
            a_lines++;
            fprintf(stderr,
                    "[audio-spu] step list=%u polls=%u last=%c wait_us=%lld entries=%u ticket=%u row=%u,%u,%u,%u,%u,%u,%u "
                    "| kick->ret1=%lld us kick->first_job=%lld first->last_job=%lld last_job->pm_end=%lld pm_end->ret1=%lld "
                    "jobs=%u runs=%u\n",
                    p_list, p_polls, "ABCD-"[p_cls_last], wait, p_entries, p_ticket, p_row[0], p_row[1], p_row[2], p_row[3],
                    p_row[4], p_row[5], p_row[6], kr, k->first_job ? to_us(k->first_job - k->kick) : -1,
                    k->first_job ? to_us(k->last_job - k->first_job) : -1,
                    k->pm_end && k->last_job ? to_us(k->pm_end - k->last_job) : -1,
                    k->pm_end && p_ret1 >= k->pm_end ? to_us(p_ret1 - k->pm_end) : -1, k->jobs, k->runs);
        }
    }
    if (!s_spu_line) s_spu_line = now + 10 * qfreq();
    if (now >= s_spu_line) {
        s_spu_line = now + 10 * qfreq();
        const unsigned long long n = a_n ? a_n : 1;
        fprintf(stderr,
                "[audio-spu] t=%.1f steps=%llu waited(>1 poll)=%llu last-poll A=%llu B=%llu C=%llu D=%llu none=%llu "
                "D:ready=%llu D:cont=%llu | per list (n=%llu): entries=%.1f jobs=%.1f runs=%.1f wait_us avg=%llu max=%llu "
                "kick->ret1 avg=%llu max=%llu kick->first_job=%llu first->last_job=%llu (max %llu) last_job->pm_end=%llu "
                "pm_end->ret1=%llu info_wait=%llu (n=%llu) | job interval us 259=%.0f/%lld 260=%.0f/%lld 263=%.0f/%lld jobs 259=%lld 260=%lld "
                "263=%lld | ring PUTs=%lld (0x2000: %lld) by 259/260/263/other=%lld/%lld/%lld/%lld dist-ahead "
                "0..7=%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld lag_after_ppu_mark avg=%lld us (n=%lld, before=%lld) "
                "other 0x2000 PUTs=%lld ea=0x%08X,0x%08X\n",
                (double)to_us(now - t_start) / 1e6, a_units, a_wait_units, a_cls[0], a_cls[1], a_cls[2], a_cls[3],
                a_cls[4], a_d_ready, a_d_cont, a_n, (double)a_entries / n, (double)a_jobs / n, (double)a_runs / n,
                a_wait_us / n, a_wait_max, a_kick_ret / n, a_kick_ret_max, a_kick_first / n, a_first_last / n,
                a_jobspan_max, a_last_pmend / n, a_pmend_ret / n, a_info_n ? a_info_wait / a_info_n : 0, a_info_n,
                s_img_n[0] ? (double)s_img_us[0] / s_img_n[0] : 0.0, s_img_n[0],
                s_img_n[1] ? (double)s_img_us[1] / s_img_n[1] : 0.0, s_img_n[1],
                s_img_n[2] ? (double)s_img_us[2] / s_img_n[2] : 0.0, s_img_n[2], s_img_jobs[0], s_img_jobs[1],
                s_img_jobs[2], s_rp_n, s_rp_2000, s_rp_img[0], s_rp_img[1], s_rp_img[2], s_rp_img[3], s_rp_dist[0],
                s_rp_dist[1], s_rp_dist[2], s_rp_dist[3], s_rp_dist[4], s_rp_dist[5], s_rp_dist[6], s_rp_dist[7],
                s_rp_lag_n ? s_rp_lag_us / s_rp_lag_n : 0, s_rp_lag_n, s_rp_lag_neg, s_rp_other2000, s_rp_other_ea[0],
                s_rp_other_ea[1]);
        a_units = a_wait_units = a_d_ready = a_d_cont = a_n = a_entries = a_jobs = a_runs = a_wait_us = a_wait_max = 0;
        a_info_wait = a_info_n = a_kick_ret = a_kick_ret_max = a_kick_first = a_first_last = a_jobspan_max = a_last_pmend = a_pmend_ret = 0;
        memset(a_cls, 0, sizeof a_cls);
        for (int i = 0; i < 3; i++) s_img_us[i] = s_img_n[i] = s_img_jobs[i] = 0;
        s_rp_n = s_rp_2000 = s_rp_lag_n = s_rp_lag_us = s_rp_lag_neg = 0;
        for (int i = 0; i < 4; i++) s_rp_img[i] = 0;
        for (int i = 0; i < 8; i++) s_rp_dist[i] = 0;
    }
}
