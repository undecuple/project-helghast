/* KZ3 Amadeus mixer job-group probe (SPU pass 52; RE pass 114 s.1 probes 1-3, the quit-to-menu hang): KZ3_AMA_PROBE=1.
 *
 * The sound-output thread (AmadeusScheduler) ends each Update (0x311658) with WaitForJobGroup 0x89D7C(E, 1000) on
 * WWS job manager 5 (jm_amadeus). Layouts (RE pass 114 s.1 and SPU pass 5d, checked against the decompile of
 * 0xA78E54 / 0x89D7C / 0x8978C / 0xA78648 this round):
 *   E (r3 of 0x89D7C): +4 job-list handle D = {+0 H, +4 SPURS instance, +8 wid}, +8 must be non-zero, +0x3C pre-gate counter.
 *   H (job-list header): +8 current list, +0x2C entry capacity, +0x30+4t entry array of list t (8-byte job headers,
 *     u16 +2 != 0 = valid), +0x40+16t sync row of list t: u16 +0 next ticket (PPU push updates it lazily),
 *     u16 +2 .. +0xE per-SPU done counters (0xFFFF idle). Done when min(row+2..+0xE) >= ticket + pending entries and,
 *     if H+8 == t, the workload's readyCount (SPURS +0x00+wid) and contention (+0x20+wid) are 0.
 *   Push 0x8978C(G = r3, r4 = swap value, r5 = 64-bit job header; ASSUMPTION from the decompile's argument order): A78648(H, jobhdr) writes the first free entry of list 3, then A78568 ->
 *     0xDF975C (cellSpursReadyCountSwap) on the workload.
 * Output: every wait that took more than 20 ms (first 40), and a state dump when one wait has polled 200, 2000 and
 * 20000 times (the job list header, list-3 entries around the ticket, the last 8 pushes, SPURS bytes for the wid). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

extern uint8_t* vm_base;

static int g_on = -1;
int kz3_ama_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_AMA_PROBE");
        g_on = e && *e && *e != '0';
    }
    return g_on;
}

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
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
static uint32_t r16(uint32_t ea)
{
    return ok(ea) ? ((uint32_t)vm_base[ea] << 8 | vm_base[ea + 1]) : 0xDEADu;
}
static uint32_t r8(uint32_t ea)
{
    return ok(ea) ? vm_base[ea] : 0xDEu;
}

#define NPUSH 8
static struct {
    unsigned long long t;
    uint32_t g, lr, ticket_before, ticket_after;
    uint64_t hdr, swap;
} g_push[NPUSH];
static volatile long g_npush, g_nwait, g_lock;
/* The wait in progress (one AmadeusScheduler thread; others are logged by count only). */
static uint32_t g_cur_e, g_cur_lr;
static unsigned long long g_cur_t0;
static long g_cur_polls;

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static void dump(const char* why, uint32_t e, long polls, uint32_t ret)
{
    const uint32_t d = e + 4, h = r32(d), spurs = r32(d + 4), wid = r32(d + 8);
    char b[4096];
    int p = 0;
    p += snprintf(
        b + p, sizeof b - p,
        "[ama-probe] %s t=%llu E=0x%08X lr=0x%08X polls=%ld ret=%u E+8=0x%X E+3C=%u H=0x%08X spurs=0x%08X wid=%u "
        "H+8(list)=%u cap=%u arrays=%08X %08X %08X %08X\n",
        why, now_ms(), e, g_cur_lr, polls, ret, r32(e + 8), r32(e + 0x3C), h, spurs, wid, r32(h + 8), r32(h + 0x2C),
        r32(h + 0x30), r32(h + 0x34), r32(h + 0x38), r32(h + 0x3C));
    for (int t = 0; t < 4; t++) {
        p += snprintf(b + p, sizeof b - p, "[ama-probe]   row%d:", t);
        for (int k = 0; k < 8; k++) p += snprintf(b + p, sizeof b - p, " %04X", r16(h + 0x40 + 16 * t + 2 * k));
        p += snprintf(b + p, sizeof b - p, "\n");
    }
    if (wid < 16 && ok(spurs))
        p += snprintf(b + p, sizeof b - p, "[ama-probe]   spurs w%u: ready=%u idle2=%u curcont=%u pendcont=%u\n", wid,
                      r8(spurs + wid), r8(spurs + 0x10 + wid), r8(spurs + 0x20 + wid), r8(spurs + 0x30 + wid));
    /* list 3 (the list the push writes) and the current list's entries around the ticket */
    const uint32_t lists[2] = {3, r32(h + 8)};
    for (int li = 0; li < 2; li++) {
        const uint32_t t = lists[li];
        if (t > 3 || (li == 1 && t == 3)) continue;
        const uint32_t arr = r32(h + 0x30 + 4 * t), tk = r16(h + 0x40 + 16 * t);
        const uint32_t lo = tk >= 6 ? tk - 6 : 0;
        p += snprintf(b + p, sizeof b - p, "[ama-probe]   list%u ticket=%u entries[%u..]:", t, tk, lo);
        for (uint32_t i = lo; i < lo + 14 && p < (int)sizeof b - 40; i++)
            p += snprintf(b + p, sizeof b - p, " %u:%08X.%08X", i, r32(arr + 8 * i), r32(arr + 8 * i + 4));
        p += snprintf(b + p, sizeof b - p, "\n");
    }
    const long np = g_npush;
    for (long i = np > NPUSH ? np - NPUSH : 0; i < np && p < (int)sizeof b - 200; i++) {
        const int s = (int)(i % NPUSH);
        const uint32_t cl = (uint32_t)g_push[s].hdr;
        p += snprintf(b + p, sizeof b - p,
                      "[ama-probe]   push#%ld t=%llu G=0x%08X lr=0x%08X r5=%016llX r4=0x%llX ticket %u->%u cl:", i + 1,
                      g_push[s].t, g_push[s].g, g_push[s].lr, (unsigned long long)g_push[s].hdr,
                      (unsigned long long)g_push[s].swap, g_push[s].ticket_before, g_push[s].ticket_after);
        for (int k = 0; k < 8; k++) p += snprintf(b + p, sizeof b - p, " %08X", r32(cl + 4 * k));
        p += snprintf(b + p, sizeof b - p, "\n");
    }
    fputs(b, stderr);
    fflush(stderr);
}

/* 0x89D7C entry / exit (WaitForJobGroup). */
void kz3_ama_wait_enter(uint32_t e, uint32_t lr)
{
    if (!kz3_ama_probe_on()) return;
    const long n = __atomic_add_fetch(&g_nwait, 1, __ATOMIC_RELAXED);
    lock();
    g_cur_e = e;
    g_cur_lr = lr;
    g_cur_t0 = now_ms();
    g_cur_polls = 0;
    unlock();
    if (n <= 3) dump("wait-first", e, 0, 0);
}
void kz3_ama_wait_exit(uint32_t e, uint32_t lr)
{
    if (!kz3_ama_probe_on()) return;
    static long slow;
    const unsigned long long dt = now_ms() - g_cur_t0;
    if (e == g_cur_e && dt > 20 && ++slow <= 40)
        fprintf(stderr, "[ama-probe] slow wait #%ld t=%llu E=0x%08X lr=0x%08X %llu ms polls=%ld (waits %ld)\n", slow,
                now_ms(), e, lr, dt, g_cur_polls, g_nwait);
    if (g_nwait % 2000 == 0) fprintf(stderr, "[ama-probe] waits=%ld pushes=%ld t=%llu\n", g_nwait, g_npush, now_ms());
}
/* 0xA798B8 poll -> r3 (1 = done). */
void kz3_ama_poll(uint32_t d, uint32_t ret)
{
    if (!kz3_ama_probe_on()) return;
    const long n = ++g_cur_polls;
    if (n == 200 || n == 2000 || n == 20000 || n == 100000) dump("stuck", d - 4, n, ret);
}
/* 0x8978C(G, r4, r5): before / after; hdr = r5, swap = r4. cl = low word of r5 (command list EA, ASSUMPTION). */
void kz3_ama_push(uint32_t g, uint64_t hdr, uint64_t swap, uint32_t lr, int after)
{
    if (!kz3_ama_probe_on()) return;
    const uint32_t h = r32(g);
    static uint32_t before;
    if (!after) {
        before = r16(h + 0x70);
        return;
    }
    const long i = __atomic_add_fetch(&g_npush, 1, __ATOMIC_RELAXED) - 1;
    const int s = (int)(i % NPUSH);
    g_push[s].t = now_ms();
    g_push[s].g = g;
    g_push[s].lr = lr;
    g_push[s].hdr = hdr;
    g_push[s].swap = swap;
    g_push[s].ticket_before = before;
    g_push[s].ticket_after = r16(h + 0x70);
    if (i < 3)
        fprintf(stderr, "[ama-probe] push-first #%ld G=0x%08X H=0x%08X lr=0x%08X r4=%016llX r5=%016llX ticket %u->%u\n",
                i + 1, g, h, lr, (unsigned long long)swap, (unsigned long long)hdr, before, g_push[s].ticket_after);
}
