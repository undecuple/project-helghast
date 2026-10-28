/*
 * KZ3_STALL_DIAG=1 (SPU pass 63, opt-in diagnostic): describe every SPU run and PPU completion wait that has not
 * returned for 3 s.
 *
 * Two stalls at the 5/1-SPU default (SPU pass 62's K4 runs) stopped presenting with nothing in the logs that named
 * the waiting code: spu-62-old (job 214 Gfx.Shadow.Infinite in its frame-arena wait, plus a jm_post run that the
 * [spurs-stuck] report could not describe) and spu-62-new (the main thread polling a flag at 0x220A78 while one task
 * thread ran at 100 % CPU). This file registers the live spu_context of every job/task run and policy-module run
 * (runtime patch 1081: g_kz3_run_ctx_hook) and every main-thread call of 0x220A78 (PPU override), and the SPURS
 * watcher thread (g_kz3_stall_scan, every 50 ms) prints, once per 5 s per stuck entry:
 *   [stall-diag] spu ...   kind (task / pm), image, overlay, taskset+task or wid+spu, ms, pc, lr, the register words the
 *                          wait loops use, reservation / event state, a 100 x 1 ms pc histogram and the stack's saved
 *                          link registers; when r85 points at a frame-arena record (the RE notes pass 111: N,
 *                          stride, stamp table, base, counter, label EA at +0x18, epoch at +0x1C) also the label
 *                          value and the N stamps (the free test of 0x5B9F0, see kz3_arena_free below).
 *   [stall-diag] ppu ...   flag EA and value, the caller's lr, r3 (the object) and its +0x5D44 word.
 * Nothing changes the guest: reads only (guest reads checked with VirtualQuery).
 *
 * Parked tasks are not stalls (SPU pass 66): a task whose pc sits at the taskset syscall trampoline (LS 0xA70,
 * SPURS_TASKSET_PM_SYSCALL_LS: WAIT_SIGNAL / YIELD / POLL) is waiting in the policy module by design. Images 44 and
 * 30 live there between their bursts and filled each K2/K4/T7 log with 300-400 lines (coord-65-k2/k4/t7). Every scan
 * (50 ms) notes when a task run is seen parked, and a task run counts as stuck only after 3 s without being seen
 * parked; a run that loops in its own code (or in an MFC/tag wait outside the syscall) is still described.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_cat.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
/* Host tests: guest bytes [0, g_kz3_sd_guest_size) of vm_base count as readable (no VirtualQuery). */
uint64_t g_kz3_sd_guest_size;
#endif
#include "spu_context.h"

extern uint8_t* vm_base;
extern void (*g_kz3_run_ctx_hook)(spu_context*, int on, int kind, uint32_t a, uint32_t b); /* runtime patch 1081 */
extern void (*g_kz3_stall_scan)(unsigned long long now_ms);                                /* runtime patch 1081 */
extern SPU_THREAD_LOCAL uint32_t g_kz3_cur_taskset, g_kz3_cur_taskid;                      /* runtime patch 1081 */

#define SD_SLOTS 96
#define SD_STUCK_MS 3000ull
#define SD_REPEAT_MS 5000ull

struct sd_slot {
    spu_context* volatile ctx; /* NULL = free */
    int kind;                  /* 0 = job/task run, 1 = policy-module run, 2 = PPU flag wait */
    uint32_t a, b, ts, task;   /* kind 0: image, args EA, taskset, task; kind 1: wid, spu; kind 2: obj, flag */
    uint32_t lr;               /* kind 2: the caller */
    unsigned long tid;
    unsigned long long t0, rep;
    unsigned long long parked; /* kind 0: last scan that saw the run at the taskset syscall (0 = never) */
    volatile int busy; /* the scan is reading ctx: the run's end waits (a task's ctx is on its thread's stack) */
};
static struct sd_slot s_slot[SD_SLOTS];
static int s_on = -1;
static SPU_THREAD_LOCAL int t_depth;
static SPU_THREAD_LOCAL int t_idx[8];

static unsigned long long sd_now(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
#endif
}
static void sd_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {0, (long)ms * 1000000L};
    nanosleep(&t, NULL);
#endif
}
static unsigned long sd_tid(void)
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

static int sd_claim(spu_context* ctx)
{
    for (int i = 0; i < SD_SLOTS; i++) {
        spu_context* exp = NULL;
        if (!s_slot[i].ctx &&
            __atomic_compare_exchange_n(&s_slot[i].ctx, &exp, ctx, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return i;
    }
    return -1;
}

static void sd_push(spu_context* ctx, int kind, uint32_t a, uint32_t b, uint32_t ts, uint32_t task, uint32_t lr)
{
    int i = sd_claim(ctx);
    if (t_depth < 8) t_idx[t_depth] = i;
    t_depth++;
    if (i < 0) return;
    struct sd_slot* s = &s_slot[i];
    s->kind = kind;
    s->a = a;
    s->b = b;
    s->ts = ts;
    s->task = task;
    s->lr = lr;
    s->tid = sd_tid();
    s->rep = 0;
    s->parked = 0;
    __atomic_store_n(&s->t0, sd_now(), __ATOMIC_RELEASE);
}
static void sd_pop(void)
{
    if (t_depth <= 0) return;
    t_depth--;
    if (t_depth >= 8) return;
    const int i = t_idx[t_depth];
    if (i < 0) return;
    __atomic_store_n(&s_slot[i].t0, 0ull, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_slot[i].busy, __ATOMIC_SEQ_CST)) sd_sleep_ms(0);
    __atomic_store_n(&s_slot[i].ctx, (spu_context*)NULL, __ATOMIC_RELEASE);
}

static void sd_run_hook(spu_context* ctx, int on, int kind, uint32_t a, uint32_t b)
{
    if (on)
        sd_push(ctx, kind, a, b, kind == 0 ? g_kz3_cur_taskset : 0, kind == 0 ? g_kz3_cur_taskid : 0, 0);
    else
        sd_pop();
}

/* PPU 0x220A78 (wait until *flag == 1, usleep(10) per poll): called by the override in kz3_ppu_overrides.cpp. A
 * dummy context pointer marks the slot used. */
static spu_context s_ppu_mark;
void kz3_stall_diag_ppu_wait(int on, uint32_t obj, uint32_t flag, uint32_t lr)
{
    if (s_on <= 0) return;
    if (on)
        sd_push(&s_ppu_mark, 2, obj, flag, 0, 0, lr);
    else
        sd_pop();
}

static int sd_readable(uint32_t ea, uint32_t len)
{
    if (!vm_base || !len || (uint64_t)ea + len > 0x100000000ull) return 0;
#ifdef _WIN32
    const uint8_t* p = vm_base + ea;
    const uint8_t* end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return 0;
        p = (const uint8_t*)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
#else
    return (uint64_t)ea + len <= g_kz3_sd_guest_size;
#endif
}
static uint32_t be32p(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t ls32(const spu_context* c, uint32_t lsa)
{
    return be32p(c->ls + (lsa & 0x3FFFCu));
}

/* The free test of PPU 0x5B9F0 (0x5BA54-0x5BAAC, read this round; SPU job 0xE5B0 has the same shape, the RE notes
 * pass 111 s.1) for label L and stamp W: x = L's epoch (top nibble), plus 16 when W's epoch is above it; same epoch
 * -> free when W <= L (cmplw); else free when x - 10 <= W's epoch (signed cmpw): a stamp up to 10 epochs older than
 * the label (modulo 16) is free, a newer one is not. */
int kz3_arena_free(uint32_t L, uint32_t W)
{
    const int le = (int)(L >> 28), we = (int)(W >> 28);
    const int x = le + (we > le ? 16 : 0);
    if (x == we) return L >= W;
    return x - 10 <= we;
}

static void sd_arena(const spu_context* c)
{
    const uint32_t r85 = c->gpr[85]._u32[0];
    if (r85 >= 0x3FFE0u || (r85 & 0xF)) return;
    const uint32_t n = ls32(c, r85), stride = ls32(c, r85 + 4), tab = ls32(c, r85 + 8), base = ls32(c, r85 + 12);
    const uint32_t ctr = ls32(c, r85 + 16), lab = ls32(c, r85 + 24), ep = ls32(c, r85 + 28);
    if (!n || n > 1024 || !sd_readable(lab & ~3u, 4) || !sd_readable(tab & ~3u, 4 * n)) return;
    const uint32_t L = be32p(vm_base + (lab & ~3u));
    char b[1400];
    int o = kz3_cat(b, sizeof b, 0,
                     "[stall-diag]   arena rec@LS%05X N=%u stride=0x%X stamps@%08X base=%08X ctr@%08X label@%08X=%08X "
                     "epoch=%08X stamps:",
                     r85, n, stride, tab, base, ctr, lab, L, ep);
    unsigned nfree = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t W = be32p(vm_base + (tab & ~3u) + 4 * i);
        const int fr = kz3_arena_free(L, W);
        nfree += fr ? 1u : 0u;
        if (i < 64 && o < (int)sizeof b - 16) o = kz3_cat(b, sizeof b, o, " %08X%s", W, fr ? "*" : "");
    }
    fprintf(stderr, "%s (free by the test: %u)\n", b, nfree);
}

/* Pass 65 (K2 stalls at level start + 5-9 s with the 5/1 + per-SPU kernel defaults, wid 4 lanes in the jm dispatcher
 * S loop): the jm policy module's state for a stuck lane, the fields the RE notes "The jm policy module" s.3 asks
 * for: LS 0x12A0 (state), 0x12C0 (slot), 0x12D0, 0x12E0, 0x12F0 (pending), 0x1410 (job list header EA), 0x1420 (the
 * ready byte A3 exchanges), 0x1440 (cached entry, byte 0 = command), 0x14E0 (the fetched barrier entry, u16 counter at
 * +0xE), 0x14F0; the 128-byte header line at [0x1410]; and 16 bytes at every guest pointer in the 0x1440 and 0x14E0
 * entries (the live counter if one of them is the barrier's EA). First 16 dumps per run. */
static void sd_hex(char* b, int n, int* o, const uint8_t* p, unsigned len)
{
    for (unsigned i = 0; i < len && *o < n - 12; i += 4)
        *o = kz3_cat(b, (size_t)n, *o, " %08X", be32p(p + i));
}
static void sd_jm(const spu_context* c)
{
    static int dumps;
    if (c->image_id != 2 || ++dumps > 16) return;
    char b[1600];
    int o = kz3_cat(b, sizeof b, 0, "[stall-diag]   jm LS 12A0..12FF:");
    sd_hex(b, sizeof b, &o, c->ls + 0x12A0, 0x60);
    o = kz3_cat(b, sizeof b, o, " | 1410=%08X 1420=%08X | 1440:", ls32(c, 0x1410), ls32(c, 0x1420));
    sd_hex(b, sizeof b, &o, c->ls + 0x1440, 16);
    o = kz3_cat(b, sizeof b, o, " | 14E0:");
    sd_hex(b, sizeof b, &o, c->ls + 0x14E0, 32);
    fprintf(stderr, "%s\n", b);
    const uint32_t hdr = ls32(c, 0x1410) & ~0x7Fu;
    if (hdr >= 0x10000u && sd_readable(hdr, 128)) {
        o = kz3_cat(b, sizeof b, 0, "[stall-diag]   jm list header line %08X:", hdr);
        sd_hex(b, sizeof b, &o, vm_base + hdr, 128);
        fprintf(stderr, "%s\n", b);
    }
    for (int e = 0; e < 2; e++) {
        const uint32_t base = e ? 0x14E0u : 0x1440u;
        for (int w = 0; w < 4; w++) {
            const uint32_t ea = ls32(c, base + 4 * w) & ~0xFu;
            if (ea < 0x10000u || ea >= 0xF0000000u || !sd_readable(ea, 16)) continue;
            o = kz3_cat(b, sizeof b, 0, "[stall-diag]   jm LS%04X word%d -> %08X:", base, w, ea);
            sd_hex(b, sizeof b, &o, vm_base + ea, 16);
            fprintf(stderr, "%s (u16 +0xE = %u)\n", b, (unsigned)((vm_base[ea + 0xE] << 8) | vm_base[ea + 0xF]));
        }
    }
}

static void sd_describe_spu(struct sd_slot* s, spu_context* c, unsigned long long ms)
{
    const uint32_t pc = c->pc & 0x3FFFFu, sp = c->gpr[1]._u32[0] & 0x3FFF0u;
    if (s->kind == 0)
        fprintf(stderr,
                "[stall-diag] spu task/job img=%u ovl=%d taskset=0x%08X task=%u args=0x%08X tid=%lu running %llu ms\n",
                s->a, c->resident_ovl, s->ts, s->task, s->b, s->tid, ms);
    else
        fprintf(stderr, "[stall-diag] spu pm wid=%u spu=%u img=%d ovl=%d tid=%lu running %llu ms\n", s->a, s->b,
                c->image_id, c->resident_ovl, s->tid, ms);
    fprintf(
        stderr,
        "[stall-diag]   pc=0x%05X lr=0x%05X sp=0x%05X status=0x%X depth=%u tagstat=0x%X tagmask=0x%X ie=%u evmask=0x%X "
        "evstat=0x%X resv=%d@0x%08X atomic=%u parked=0x%X sstat=0x%X\n",
        pc, c->gpr[0]._u32[0] & 0x3FFFFu, sp, c->status, c->host_depth, c->mfc_tag_status, c->mfc_tag_mask,
        c->int_enable, c->event_mask, c->event_status, c->resv_valid, c->resv_ea, c->atomic_stat, c->list_stall_mask,
        c->list_stall_stat);
    char b[900];
    int o = kz3_cat(b, sizeof b, 0, "[stall-diag]   r3-r8:");
    for (int r = 3; r <= 8; r++)
        o = kz3_cat(b, sizeof b, o, " %08X.%08X", c->gpr[r]._u32[0], c->gpr[r]._u32[1]);
    o = kz3_cat(b, sizeof b, o, " r80-r97:");
    for (int r = 80; r <= 97 && o < (int)sizeof b - 12; r++)
        o = kz3_cat(b, sizeof b, o, " %08X", c->gpr[r]._u32[0]);
    fprintf(stderr, "%s\n", b);
    /* saved link registers up the SPU stack: back chain at [sp], the caller's lr at [back + 16] (SPU ABI) */
    o = kz3_cat(b, sizeof b, 0, "[stall-diag]   stack lrs:");
    uint32_t f = sp;
    for (int k = 0; k < 10 && f && f < 0x3FFF0u; k++) {
        const uint32_t back = ls32(c, f) & 0x3FFF0u;
        if (!back || back <= f) break;
        o = kz3_cat(b, sizeof b, o, " 0x%05X", ls32(c, back + 16) & 0x3FFFFu);
        f = back;
    }
    fprintf(stderr, "%s\n", b);
    uint32_t pcs[8] = {0}, cnt[8] = {0};
    for (int k = 0; k < 100; k++) {
        const uint32_t p = c->pc & 0x3FFFFu;
        int m;
        for (m = 0; m < 8 && cnt[m] && pcs[m] != p; m++) {
        }
        if (m < 8) {
            pcs[m] = p;
            cnt[m]++;
        }
        sd_sleep_ms(1);
    }
    o = kz3_cat(b, sizeof b, 0, "[stall-diag]   pcs:");
    for (int m = 0; m < 8 && cnt[m]; m++) o = kz3_cat(b, sizeof b, o, " 0x%05X:%u", pcs[m], cnt[m]);
    fprintf(stderr, "%s\n", b);
    sd_arena(c);
    if (s->kind == 1) sd_jm(c);
}

static void sd_scan(unsigned long long now)
{
    for (int i = 0; i < SD_SLOTS; i++) {
        struct sd_slot* s = &s_slot[i];
        spu_context* c = __atomic_load_n(&s->ctx, __ATOMIC_ACQUIRE);
        const unsigned long long t0 = __atomic_load_n(&s->t0, __ATOMIC_ACQUIRE);
        if (!c || !t0 || now < t0) continue;
        if (s->kind != 0 && (now - t0 < SD_STUCK_MS || now - s->rep < SD_REPEAT_MS)) continue;
        __atomic_store_n(&s->busy, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s->t0, __ATOMIC_SEQ_CST) != t0 || __atomic_load_n(&s->ctx, __ATOMIC_SEQ_CST) != c) {
            __atomic_store_n(&s->busy, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        if (s->kind == 0) {
            /* a task run: parked at the taskset syscall = waiting by design; age counts from the last time seen so */
            if ((c->pc & 0x3FFFFu) == SPURS_TASKSET_PM_SYSCALL_LS) s->parked = now;
            const unsigned long long since = s->parked > t0 ? s->parked : t0;
            if (now < since || now - since < SD_STUCK_MS || now - s->rep < SD_REPEAT_MS) {
                __atomic_store_n(&s->busy, 0, __ATOMIC_SEQ_CST);
                continue;
            }
        }
        s->rep = now;
        if (s->kind == 2) {
            const uint32_t fv = sd_readable(s->b & ~3u, 4) ? be32p(vm_base + (s->b & ~3u)) : 0xDEADDEADu;
            const uint32_t ow = sd_readable(s->a + 0x5D44u, 4) ? be32p(vm_base + s->a + 0x5D44u) : 0xDEADDEADu;
            const uint32_t gw = sd_readable(0x013362B8u, 4) ? be32p(vm_base + 0x013362B8u) : 0xDEADDEADu;
            fprintf(stderr,
                    "[stall-diag] ppu wait 0x220A78 tid=%lu for %llu ms: flag@%08X=%08X caller lr=0x%08X obj=%08X "
                    "obj+0x5D44=%08X [0x13362B8]=%08X\n",
                    s->tid, now - t0, s->b, fv, s->lr, s->a, ow, gw);
            if (sd_readable(s->b & ~0x7Fu, 128)) {
                char b[600];
                int o = kz3_cat(b, sizeof b, 0, "[stall-diag]   flag line@%08X:", s->b & ~0x7Fu);
                for (int w = 0; w < 32; w++)
                    o = kz3_cat(b, sizeof b, o, " %08X", be32p(vm_base + (s->b & ~0x7Fu) + 4 * w));
                fprintf(stderr, "%s\n", b);
            }
        } else {
            sd_describe_spu(s, c, now - t0);
        }
        fflush(stderr);
        __atomic_store_n(&s->busy, 0, __ATOMIC_SEQ_CST);
    }
}

void kz3_stall_diag_install(void)
{
    const char* e = getenv("KZ3_STALL_DIAG");
    s_on = (e && *e && *e != '0') ? 1 : 0;
    if (!s_on) return;
    g_kz3_run_ctx_hook = sd_run_hook;
    g_kz3_stall_scan = sd_scan;
    fprintf(stderr,
            "[stall-diag] on (KZ3_STALL_DIAG, SPU pass 63): runs and 0x220A78 waits over %llu ms are described\n",
            SD_STUCK_MS);
}
