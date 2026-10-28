/* KZ3 audio mix-job (voice count) probe (SPU pass 71; the RE notes s.5.4, RE pass 127): KZ3_AUDIO_VOICE=1.
 *
 * The per-unit mix pass (0xD3C1D8) calls 0xD44AB8 (1.14 0xD94A2C) once per batch of "taken" items of a started
 * node; each call appends one synth_submix/aux job descriptor (s.5.1). Arguments at the call (0xD3C65C..0xD3C668,
 * read this round): r3 = the job-list owner, r4 = node, r5 = array of taken item pointers (stw r28 at 0xD3C5E0),
 * r6 = taken count, r7 = last-batch flag. Node fields (s.5.4): +0x18 class (batch size at class+0x230), +0x4C start
 * unit, +0x58 delay, +0x5C unit counter U (written by 0xD3E0B4 each pass, so a new U = a new unit), +0x80 state
 * bits, +0x98 item count. Item: +0x10 wire object; wire+0x0C float (taken when < 0.2 or forced).
 * Per unit: calls (= jobs), taken items, distinct nodes, distinct classes. Every 10 s: [audio-voice] averages, the
 * age (U - start) distribution of the batches of that window (a voice leak shows as ages that only grow), the wire
 * float histogram, the count of distinct nodes ever seen, and one unit dumped call by call (at most 64 lines). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static float rdf(uint32_t ea)
{
    uint32_t u = rd32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static int ok(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0xF0000000u;
}
static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

static int g_on = -1;
int kz3_mixjob_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_AUDIO_VOICE");
        g_on = (e && *e && *e != '0') ? 1 : 0;
        if (g_on) fprintf(stderr, "[audio-voice] probe on (0xD44AB8 per-batch rows, 10 s summaries)\n");
    }
    return g_on;
}

/* current unit */
static uint32_t u_cur = 0xFFFFFFFFu;
static unsigned u_calls, u_taken, u_nodes_n, u_cls_n;
static uint32_t u_nodes[256], u_cls[64];
/* window */
static unsigned long long w_t0, w_start;
static unsigned w_units, w_calls, w_taken, w_nodes, w_cls, w_calls_max, w_taken_max, w_win;
static unsigned w_fh[5];  /* wire float: <0.001, <0.05, <0.2, >=0.2 (forced), bad pointer */
static unsigned w_age[6]; /* U - start: <188 (1 s), <938 (5 s), <1875 (10 s), <5625 (30 s), >=5625, no start */
static unsigned w_age_max;
static uint32_t w_dump_unit = 0xFFFFFFFFu;
static int w_dump_left, w_want_dump;
/* taken items (open addressing): streak start / last unit; forced split (item+0x14 -> word bit 0) */
static uint32_t it_key[8192], it_start[8192], it_last[8192];
static unsigned it_n;
static unsigned w_forced, w_free, w_reset; /* forced flag set / taken by the 0.2 test / forced with wire f = 1e6 */
static uint32_t* it_slot(uint32_t v)
{
    unsigned h = (v * 2654435761u) & 8191u;
    for (unsigned k = 0; k < 8192; k++, h = (h + 1) & 8191u) {
        if (it_key[h] == v) return &it_key[h];
        if (!it_key[h]) {
            if (it_n >= 7000) return 0;
            it_key[h] = v;
            it_n++;
            it_start[h] = it_last[h] = 0xFFFFFFFFu;
            return &it_key[h];
        }
    }
    return 0;
}
/* every node ever seen (open addressing) */
static uint32_t all_nodes[8192];
static unsigned all_n;

static void add_set(uint32_t* s, unsigned* n, unsigned cap, uint32_t v)
{
    for (unsigned i = 0; i < *n; i++)
        if (s[i] == v) return;
    if (*n < cap) s[(*n)++] = v;
}
static void add_all(uint32_t v)
{
    unsigned h = (v * 2654435761u) & 8191u;
    for (unsigned k = 0; k < 8192; k++, h = (h + 1) & 8191u) {
        if (all_nodes[h] == v) return;
        if (!all_nodes[h]) {
            if (all_n < 7000) {
                all_nodes[h] = v;
                all_n++;
            }
            return;
        }
    }
}

static void close_unit(void)
{
    if (u_cur == 0xFFFFFFFFu) return;
    w_units++;
    w_calls += u_calls;
    w_taken += u_taken;
    w_nodes += u_nodes_n;
    w_cls += u_cls_n;
    if (u_calls > w_calls_max) w_calls_max = u_calls;
    if (u_taken > w_taken_max) w_taken_max = u_taken;
    u_calls = u_taken = u_nodes_n = u_cls_n = 0;
}

static void window_check(void)
{
    const unsigned long long t = now_ms();
    if (!w_start) w_start = w_t0 = t;
    if (t - w_t0 < 10000) return;
    const unsigned d = w_units ? w_units : 1;
    fprintf(stderr,
            "[audio-voice] t=%llus units=%u jobs/unit avg %.1f max %u | taken/unit avg %.1f max %u | nodes/unit %.1f "
            "classes/unit %.1f | nodes ever %u | wire f <1e-3 %u <0.05 %u <0.2 %u forced %u bad %u | age(units) "
            "<1s %u <5s %u <10s %u <30s %u >=30s %u nostart %u max %u\n",
            (t - w_start) / 1000, w_units, (double)w_calls / d, w_calls_max, (double)w_taken / d, w_taken_max,
            (double)w_nodes / d, (double)w_cls / d, all_n, w_fh[0], w_fh[1], w_fh[2], w_fh[3], w_fh[4], w_age[0],
            w_age[1], w_age[2], w_age[3], w_age[4], w_age[5], w_age_max);
    { /* live taken items (seen in the last 2 units) by streak length */
        unsigned sh[5] = {0}, live = 0, smax = 0;
        for (unsigned i = 0; i < 8192; i++) {
            if (!it_key[i] || it_last[i] == 0xFFFFFFFFu || u_cur - it_last[i] > 2) continue;
            const uint32_t l = it_last[i] - it_start[i];
            live++;
            sh[l < 188 ? 0 : l < 938 ? 1 : l < 1875 ? 2 : l < 5625 ? 3 : 4]++;
            if (l > smax) smax = l;
        }
        fprintf(stderr,
                "[audio-voice] t=%llus items: live %u streak <1s %u <5s %u <10s %u <30s %u >=30s %u max %u units | "
                "taken by forced %u (wire reset 1e6 %u) by f<0.2 %u | items ever %u\n",
                (t - w_start) / 1000, live, sh[0], sh[1], sh[2], sh[3], sh[4], smax, w_forced, w_reset, w_free, it_n);
        w_forced = w_free = w_reset = 0;
    }
    w_t0 = t;
    w_units = w_calls = w_taken = w_nodes = w_cls = w_calls_max = w_taken_max = w_age_max = 0;
    memset(w_fh, 0, sizeof w_fh);
    memset(w_age, 0, sizeof w_age);
    w_want_dump = 1;
    w_win++;
}

/* 0xD44AB8 entry: r4 node, r5 taken-item array, r6 taken count, r7 last flag (brbDriver thread only) */
void kz3_mixjob_probe_batch(uint32_t node, uint32_t items, uint32_t taken, uint32_t last)
{
    if (!ok(node)) return;
    const uint32_t U = rd32(node + 0x5C);
    if (U != u_cur) {
        close_unit();
        window_check();
        u_cur = U;
        if (w_want_dump) {
            w_want_dump = 0;
            w_dump_unit = U;
            w_dump_left = 64;
            fprintf(stderr, "[audio-voice] dump unit %u (window %u)\n", U, w_win);
        }
    }
    const uint32_t cls = rd32(node + 0x18);
    const uint32_t batch = ok(cls) ? rd32(cls + 0x230) : 0;
    const uint32_t start = rd32(node + 0x4C), delay = rd32(node + 0x58), st = rd32(node + 0x80);
    const uint32_t nitems = rd32(node + 0x98);
    u_calls++;
    u_taken += taken;
    add_set(u_nodes, &u_nodes_n, 256, node);
    add_set(u_cls, &u_cls_n, 64, cls);
    add_all(node);
    if (start == 0xFFFFFFFFu) {
        w_age[5]++;
    } else {
        const uint32_t age = U - start;
        w_age[age < 188 ? 0 : age < 938 ? 1 : age < 1875 ? 2 : age < 5625 ? 3 : 4]++;
        if (age > w_age_max) w_age_max = age;
    }
    float f0 = -1.0f;
    for (uint32_t i = 0; i < taken && i < 64; i++) {
        const uint32_t it = ok(items) ? rd32(items + 4 * i) : 0;
        const uint32_t wire = ok(it) ? rd32(it + 0x10) : 0;
        if (!ok(wire)) {
            w_fh[4]++;
            continue;
        }
        const float f = rdf(wire + 0x0C);
        if (i == 0) f0 = f;
        {
            const uint32_t fp = rd32(it + 0x14);
            const int forced = ok(fp) ? (int)(rd32(fp) & 1u) : 0;
            if (forced) {
                w_forced++;
                if (f >= 999999.0f) w_reset++;
            } else {
                w_free++;
            }
            uint32_t* k = it_slot(it);
            if (k) {
                const unsigned j = (unsigned)(k - it_key);
                if (it_last[j] == 0xFFFFFFFFu || U - it_last[j] > 1) it_start[j] = U;
                it_last[j] = U;
            }
        }
        w_fh[f < 0.001f ? 0 : f < 0.05f ? 1 : f < 0.2f ? 2 : 3]++;
    }
    if (U == w_dump_unit && w_dump_left > 0) {
        w_dump_left--;
        fprintf(stderr,
                "[audio-voice]  node %08x class %08x batch %u taken %u last %u start %08x delay %u U %u st %08x "
                "items %u f0 %.4g\n",
                node, cls, batch, taken, last, start, delay, U, st, nitems, (double)f0);
    }
}
