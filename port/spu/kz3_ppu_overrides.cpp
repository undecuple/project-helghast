/* KZ3 host replacements for lifted PPU functions (ppu_lifter.py --overrides, patch 1018).
 * Each one compiles only against a lift that announced it (PPU_OVERRIDE_<addr> in ppu_overrides.h since patch 3005;
 * older lifts announced them in ppu_recomp.h), so an older lift without --overrides still links (with the original
 * behaviour). Only this TU includes ppu_overrides.h: the lifted chunks do not, so an override change recompiles this
 * file and the one chunk that defines the __lifted body.
 * Facts and verification: the RE notes, "Prenarrative end (pass 9)". */
#include "ppu_recomp.h"
#if defined(__has_include)
#  if __has_include("ppu_overrides.h")
#    include "ppu_overrides.h"
#  endif
#endif
#include <stdio.h>
#include <stdlib.h>

#if defined(PPU_OVERRIDE_00CB26E0) || defined(PPU_OVERRIDE_00CC29C0)
/* @0x00CB26E0 BinkShouldSkip(bink) (1.14 twin @0x00CC29C0, body identical, same Bink struct offsets, pass 95) (ASSUMPTION: Bink SDK name; statically linked, no symbols).
 * Bink struct fields used: +0x08 Frames, +0x0C FrameNum (next frame to decode), +0x10 LastFrameNum
 * (set when an async decode completes, @0x00CB3F20).
 *
 * The game's Bink player update (@0x00080B5C, vtable 0xEA8D50 slot 0x10) per due frame does:
 *   BinkDoFrameAsyncWait(-1)  -> LastFrameNum = finished frame
 *   BinkNextFrame             -> FrameNum + 1, or 1 after the last frame (wrap)
 *   while (BinkShouldSkip) { BinkDoFrameAsync; BinkDoFrameAsyncWait; BinkNextFrame }
 *   BinkDoFrameAsync
 *   if (LastFrameNum == Frames) -> movie done (+0xD0), MovieWidget fires onend
 * BinkShouldSkip refuses to skip within 2 frames of the end, but not on the frame right after the
 * wrap: when playback is behind there, the skip loop decodes frame 1, LastFrameNum becomes 1, the end
 * test never sees LastFrameNum == Frames, and the movie plays again from the start (forever, for the
 * non-looping prenarrative: onend="open(intro.gum)" never fires). On a PS3 the decoder keeps up and
 * the skip is rare; here the SPU decode is slower than real time, so it hits most runs.
 * Fix: no skip while the last decoded frame is the final one (FrameNum wrapped to 1). One frame is
 * shown late instead of skipped; nothing else changes. KZ3_BINK_WRAP_FIX=0 restores the original
 * (and logs what it would have done). */
static int g_wrap_fix = -1;

static void kz3_bink_body(ppu_context* ctx, void (*orig)(ppu_context*))
{
    uint32_t bink = (uint32_t)ctx->gpr[3];
    if (bink && vm_read32(bink + 0x0C) == 1 && vm_read32(bink + 0x08) > 1 &&
        vm_read32(bink + 0x10) == vm_read32(bink + 0x08)) {
        if (g_wrap_fix < 0) {
            const char* e = getenv("KZ3_BINK_WRAP_FIX");
            g_wrap_fix = !(e && e[0] == '0');
        }
        if (g_wrap_fix) {
            fprintf(stderr, "[bink] wrap after frame %u of %u: no skip (end test keeps LastFrameNum)\n",
                    vm_read32(bink + 0x10), vm_read32(bink + 0x08));
            ctx->gpr[3] = 0;
            return;
        }
        uint32_t frames = vm_read32(bink + 0x08);
        orig(ctx);
        fprintf(stderr, "[bink] wrap after frame %u of %u: original BinkShouldSkip = %u\n",
                frames, frames, (unsigned)(uint32_t)ctx->gpr[3]);
        return;
    }
    orig(ctx);
}
#ifdef PPU_OVERRIDE_00CB26E0
void func_00CB26E0(ppu_context* ctx) { kz3_bink_body(ctx, func_00CB26E0__lifted); }
#endif
#ifdef PPU_OVERRIDE_00CC29C0
void func_00CC29C0(ppu_context* ctx) { kz3_bink_body(ctx, func_00CC29C0__lifted); }
#endif
#endif

#if defined(PPU_OVERRIDE_000111F8) || defined(PPU_OVERRIDE_00011208)   /* 1.14 twin 0x00011208: identical body */
/* @0x000111F8 hash(k=r3, length=r4, initval=r5) -> r3 (zero-extended u32): Bob Jenkins' lookup2, byte-wise
 * little-endian, golden ratio 0x9E3779B9 (matched against the disassembly: 12-byte blocks, 11-case tail with
 * c's low byte reserved for the length). The level load's 'Asset Streaming Thread' spent nearly all its
 * samples in the lifted body (one vm_read8 call per byte, spu-9f/9g). Same result, read straight from guest
 * memory. KZ3_HASH_VERIFY=1 also runs the lifted body and reports any difference (first 8);
 * KZ3_HASH_LOG=1 logs calls of 256 KiB or more with the caller (first 32). */
extern "C" uint8_t* vm_base;

static inline uint32_t kz3_lookup2(const uint8_t* k, uint32_t length, uint32_t initval)
{
#define KZ3_MIX(a, b, c) { \
    a -= b; a -= c; a ^= (c >> 13); b -= c; b -= a; b ^= (a << 8);  c -= a; c -= b; c ^= (b >> 13); \
    a -= b; a -= c; a ^= (c >> 12); b -= c; b -= a; b ^= (a << 16); c -= a; c -= b; c ^= (b >> 5);  \
    a -= b; a -= c; a ^= (c >> 3);  b -= c; b -= a; b ^= (a << 10); c -= a; c -= b; c ^= (b >> 15); }
    uint32_t a = 0x9E3779B9u, b = 0x9E3779B9u, c = initval, len = length;
    while (len >= 12) {
        a += k[0] + ((uint32_t)k[1] << 8) + ((uint32_t)k[2] << 16) + ((uint32_t)k[3] << 24);
        b += k[4] + ((uint32_t)k[5] << 8) + ((uint32_t)k[6] << 16) + ((uint32_t)k[7] << 24);
        c += k[8] + ((uint32_t)k[9] << 8) + ((uint32_t)k[10] << 16) + ((uint32_t)k[11] << 24);
        KZ3_MIX(a, b, c);
        k += 12; len -= 12;
    }
    c += length;
    switch (len) {
    case 11: c += (uint32_t)k[10] << 24; /* fall through */
    case 10: c += (uint32_t)k[9] << 16;  /* fall through */
    case 9:  c += (uint32_t)k[8] << 8;   /* fall through */
    case 8:  b += (uint32_t)k[7] << 24;  /* fall through */
    case 7:  b += (uint32_t)k[6] << 16;  /* fall through */
    case 6:  b += (uint32_t)k[5] << 8;   /* fall through */
    case 5:  b += k[4];                  /* fall through */
    case 4:  a += (uint32_t)k[3] << 24;  /* fall through */
    case 3:  a += (uint32_t)k[2] << 16;  /* fall through */
    case 2:  a += (uint32_t)k[1] << 8;   /* fall through */
    case 1:  a += k[0];
    }
    KZ3_MIX(a, b, c);
#undef KZ3_MIX
    return c;
}

static int g_hash_verify = -1, g_hash_log = -1;

#if 1  /* func_00AB14F0 (guest memcpy) is always in the lift */
#include <string.h>
/* KZ3_MEMCPY_TEST=1: one-shot check of the lifted guest memcpy @0x00AB14F0 against memmove on overlapping
 * copies (RE pass 15 check (b): the Agent3Funcs registrar @0x909EBC copies its 0x33-byte template with
 * it, ASSUMPTION that src and dst may overlap). Runs on the first lookup2 call, on that thread's context, in
 * guest stack space below r1 (the whole context is saved and restored). Prints one line per case. */
static void kz3_memcpy_selftest(ppu_context* ctx, void (*memcpy_fn)(ppu_context*))
{
    ppu_context saved = *ctx;
    uint32_t sp = ((uint32_t)ctx->gpr[1] - 0x2000u) & ~0xFu;  /* new stack top for the call */
    uint32_t buf = sp + 0x800u;                                 /* 0x400-byte scratch above it, below r1 */
    int bad = 0, cases = 0;
    static const uint32_t lens[] = {0x33, 0x10, 0x80, 0x123};
    for (int dir = 0; dir < 2; dir++)
        for (uint32_t so = 0; so < 16; so++)
            for (unsigned li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                uint32_t n = lens[li];
                uint32_t src = buf + 0x40 + so, dst = dir ? src - 0x10 : src + 0x10;
                uint8_t ref[0x400];
                for (int i = 0; i < 0x400; i++) vm_base[buf + i] = (uint8_t)(i * 7 + 1);
                memcpy(ref, vm_base + buf, sizeof ref);
                memmove(ref + (dst - buf), ref + (src - buf), n);
                *ctx = saved;
                ctx->gpr[1] = sp; ctx->gpr[3] = dst; ctx->gpr[4] = src; ctx->gpr[5] = n;
                memcpy_fn(ctx);
                int first = -1;
                for (int i = 0; i < 0x400 && first < 0; i++) if (vm_base[buf + i] != ref[i]) first = i;
                cases++;
                if (first >= 0) {
                    bad++;
                    if (bad <= 12)
                        fprintf(stderr, "[memcpy-test] MISMATCH dst=src%s0x10 src_off=%u n=0x%X: first diff at buf+0x%X "
                                "(dst+%d) got %02X want %02X\n", dir ? "-" : "+", so, n, first, first - (int)(dst - buf),
                                vm_base[buf + first], ref[first]);
                }
            }
    *ctx = saved;
    fprintf(stderr, "[memcpy-test] lifted memcpy @0x00AB14F0 vs memmove: %d/%d overlapping cases differ\n", bad, cases);
}
#endif

static void kz3_hash_body(ppu_context* ctx, void (*orig)(ppu_context*), void (*memcpy_fn)(ppu_context*))
{
#if 1  /* func_00AB14F0 (guest memcpy) is always in the lift */
    { static int once = 0;
      if (!once) { once = 1; const char* t = getenv("KZ3_MEMCPY_TEST"); if (t && t[0] == '1' && vm_base) kz3_memcpy_selftest(ctx, memcpy_fn); } }
#endif
    uint32_t k = (uint32_t)ctx->gpr[3], len = (uint32_t)ctx->gpr[4], init = (uint32_t)ctx->gpr[5];
    if (g_hash_verify < 0) {
        const char* v = getenv("KZ3_HASH_VERIFY"); const char* l = getenv("KZ3_HASH_LOG");
        g_hash_log = (l && l[0] == '1');
        g_hash_verify = (v && v[0] == '1');
    }
    if (!vm_base || k < 0x10000u || (uint64_t)k + len > 0xFFFF0000ull) {   /* odd input: the original */
        static int nf = 0;
        if (g_hash_log && nf++ < 8)
            fprintf(stderr, "[hash] fallback k=0x%08X len=%u lr=0x%08X vm_base=%p\n", k, len, (uint32_t)ctx->lr, (void*)vm_base);
        orig(ctx);
        return;
    }
    if (g_hash_log && len >= 256u * 1024) {
        static int n = 0;
        if (n++ < 32) fprintf(stderr, "[hash] lookup2 k=0x%08X len=%u lr=0x%08X\n", k, len, (uint32_t)ctx->lr);
    }
    if (g_hash_log) {
        static unsigned long long calls = 0, bytes = 0;
        bytes += len;
        if ((++calls & 0xFFFFF) == 0)
            fprintf(stderr, "[hash] %llu calls, %llu bytes; this one k=0x%08X len=%u lr=0x%08X\n",
                    calls, bytes, k, len, (uint32_t)ctx->lr);
    }
    uint32_t h = kz3_lookup2(vm_base + k, len, init);
    if (g_hash_verify) {
        orig(ctx);
        static int bad = 0;
        if (ctx->gpr[3] != (uint64_t)h && bad++ < 8)
            fprintf(stderr, "[hash] MISMATCH k=0x%08X len=%u init=0x%08X native=0x%08X lifted=0x%016llX\n",
                    k, len, init, h, (unsigned long long)ctx->gpr[3]);
        static unsigned long long calls = 0;
        if (++calls == 100000) fprintf(stderr, "[hash] verify: 100000 calls compared, %d mismatches\n", bad);
    }
    ctx->gpr[3] = h;
}
/* the guest memcpy (selftest only): 1.00 0x00AB14F0, 1.14 0x00ABBC28 (addr_map_114.tsv, conf 0.99 L1-unique) */
#ifdef PPU_OVERRIDE_000111F8
void func_000111F8(ppu_context* ctx) { kz3_hash_body(ctx, func_000111F8__lifted, func_00AB14F0); }
#endif
#ifdef PPU_OVERRIDE_00011208
void func_00011208(ppu_context* ctx) { kz3_hash_body(ctx, func_00011208__lifted, func_00ABBC28); }
#endif
#endif

/* The GCM window move (pass 9: KZ3 uses guest 0x20000000+, the runtime's default window) is set in
 * port/kz3/main.cpp (ppu_hle_inject_base = 0x7F000000, KZ3_INJECT_BASE overrides). */

#if defined(PPU_OVERRIDE_000203DC) || defined(PPU_OVERRIDE_00020714)   /* 1.14 twin 0x00020714: identical body */
/* @0x000203DC heap_alloc(heap, size) (ASSUMPTION: name from behaviour; called by malloc @0x00086428 with the global
 * heap 0x0145BE88). Runs the original and logs the first 8 NULL returns with the caller, the call count, the bytes
 * requested so far and the highest end address handed out (the guest pool is 0x40000000..0x45B00000, one
 * sys_memory_allocate). Pass 12 (spu-12c) saw no NULL return up to the level-load stop at ~335 s, so that stop is
 * not an allocation failure; it is the PPU fiber scheduler at @0x000C3A54 (see the fiber overrides below). */
static void kz3_heap_body(ppu_context* ctx, void (*orig)(ppu_context*))
{
    uint32_t heap = (uint32_t)ctx->gpr[3], size = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    orig(ctx);
    uint32_t p = (uint32_t)ctx->gpr[3];
    static unsigned long long calls = 0, bytes = 0;
    static uint32_t hi = 0;
    calls++; bytes += size;
    if (p && p + size > hi) hi = p + size;
    if (!p && size) {
        static int n = 0;
        if (n++ < 8) {
            fprintf(stderr, "[heap] alloc FAILED heap=0x%08X size=0x%X lr=0x%08X tid=%llu after %llu calls, %llu bytes "
                    "requested, high-water 0x%08X; heap[0..7]:", heap, size, lr, (unsigned long long)ctx->thread_id,
                    calls, bytes, hi);
            for (int i = 0; i < 8 && vm_base; i++)
                fprintf(stderr, " %08X", __builtin_bswap32(*(uint32_t*)(vm_base + heap + 4 * i)));
            fprintf(stderr, "\n");
        }
    }
    if ((calls & 0x3FFFF) == 0)
        fprintf(stderr, "[heap] %llu allocs, %llu bytes requested, high-water 0x%08X\n", calls, bytes, hi);
}
#ifdef PPU_OVERRIDE_000203DC
void func_000203DC(ppu_context* ctx) { kz3_heap_body(ctx, func_000203DC__lifted); }
#endif
#ifdef PPU_OVERRIDE_00020714
void func_00020714(ppu_context* ctx) { kz3_heap_body(ctx, func_00020714__lifted); }
#endif
#endif

#if defined(PPU_OVERRIDE_000C3A54) && defined(PPU_OVERRIDE_000C3A68) && defined(PPU_OVERRIDE_000C39BC)
#define KZ3_FIBER_V100 1
#endif
#if defined(PPU_OVERRIDE_000C59F4) && defined(PPU_OVERRIDE_000C5A08) && defined(PPU_OVERRIDE_000C595C)
#define KZ3_FIBER_V114 1   /* twins +0x1FA0 (the RE notes s.35/36.7: identical bodies); save/restore 0xC5A38/0xC5B4C */
#endif
#if defined(KZ3_FIBER_V100) || defined(KZ3_FIBER_V114)
/* The PPU fiber scheduler (hand-written assembly in the EBOOT; names ASSUMPTION, from behaviour). Facts and
 * verification: the RE notes, "Pass 13: PPU fiber scheduler".
 *   @0x000C3A98 save(buf)    r14-r31, f14-f31, v20-v31, r1 (+0x90), r2, r0=LR (+0xA0), r3, r4, r5, CR into buf
 *   @0x000C3BAC restore(buf) the reverse, then blr to the saved LR on the saved r1 (longjmp)
 *   @0x000C3CC4 start(opd=r5, sp=r6): r2 = toc, r1 = sp, LR = entry, blr (jumps into the fiber entry)
 *   @0x000C3864 schedule(S): the loop below; never returns (only leaves through restore or start)
 *   @0x000C3A54 run_all(S, list{count, entries*}): save(S); S fields; schedule(S). Its "return" is the restore of
 *               S once every fiber is done, so it returns to its caller with r3 = S.
 *   @0x000C3A68 wait(S, ptr) (from @0x000C383C wait(S, ptr, value), which stores value at slot+0x14): save into
 *               the slot's buffer, slot+0 = ptr, ++S+0x284, schedule(S); resumed when *ptr == value.
 *   @0x000C39BC finish(S): slot+4 = 0, schedule(S) (called at the end of a fiber body, e.g. @0x003348E4).
 * Scheduler S: +0x000 the caller's saved context, +0x280 current slot, +0x284 number of waiting fibers,
 * +0x288 + i*0x18 (i < 5) slots {+0 watched ptr, +4 job entry (0 = free), +0xC stack top, +0x10 context buffer,
 * +0x14 wanted value}, +0x300/+0x304 job cursor/end (8-byte entries {opd, arg}). Global 0x013362B8 = running S.
 * Lifted as plain C, start and restore only set ctx->r1/LR and return into the lifted caller: run_all spun forever
 * in schedule (the round-12 stop at ~335 s). Here run_all runs the same schedule on the calling host fiber and each
 * guest fiber gets a host fiber (guest_fiber.c). Guest-visible state (S fields, save buffers, registers after a
 * restore) is written as the original does. KZ3_FIBER_LOG=N logs the first N events (default 24). */
#include "guest_fiber.h"
#include "kz3_fiber_stall.h"
#include <string.h>
extern "C" void ps3_indirect_call(ppu_context* ctx);
/* Pass 34 stall report: patch 1045 (cellSpurs.c) and kz3_fiber_stall_rt.c. */
extern "C" int kz3_spurs_ready_peek(uint32_t wid, uint32_t* ready_host, uint32_t* ready_inst, uint32_t* state, uint32_t* curcont, uint32_t* spurs_ea);
extern "C" uint32_t kz3_spurs_ready_kick(uint32_t wid, uint32_t value);
extern "C" int kz3_spurs_workload_image(uint32_t wid);
extern "C" int kz3_ds_spu_state(uint32_t wid, uint32_t spu_num, uint32_t* pc, int* ovl, int* policy, int* in_putllc);
extern "C" const char* kz3_spu_image_name_of(int image_id);
extern "C" int kz3_spurs_dump_runs(const char* tag);   /* patch 1047: kernel threads in a run, lanes, busy claims */
extern "C" int* ps3_ppu_dispatch_depth_slot(void);   /* patch 1031 */
extern "C" void (*g_spu_put_hook)(uint32_t ea, uint32_t size);           /* runtime/spu/spu_channels.c */
extern "C" void cellGcm_note_guest_write_ea(uint32_t ea, uint32_t size);    /* rsx patch 2017 */
namespace {
/* KZ3_SPU_PUT_NOTE=1 (opt-in, SPU pass 19): every SPU DMA PUT also goes to cellGcm_note_guest_write_ea, so the
 * live renderer re-reads RSX-visible memory an SPU job wrote in the same frame (MLAA, the post chain), instead of
 * finding it by validation. Chained in front of the GCM layer's put hook (installed by cellGcmInit). */
void (*g_put_hook_next)(uint32_t, uint32_t);
void kz3_spu_put_note(uint32_t ea, uint32_t size)
{
    cellGcm_note_guest_write_ea(ea, size);
    if (g_put_hook_next) g_put_hook_next(ea, size);
}
void install_put_note()
{
    static const int on = [] { const char* e = getenv("KZ3_SPU_PUT_NOTE"); return e && atoi(e) > 0; }();
    void (*cur)(uint32_t, uint32_t) = g_spu_put_hook;
    if (!on || !cur || cur == kz3_spu_put_note) return;   /* wait for cellGcmInit's hook, then chain once */
    g_put_hook_next = cur;
    g_spu_put_hook = kz3_spu_put_note;
    fprintf(stderr, "[spu-put-note] SPU PUTs now noted to the live renderer\n");
}
} // namespace
extern "C" int* ps3_ppu_vcall_sp_slot(void);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

#ifdef KZ3_FIBER_V100
static constexpr uint32_t kSchedCur = 0x013362B8u;   /* global: the running scheduler S (stw at @0x000C3A1C) */
static inline void kz3_fib_save(ppu_context* c) { func_000C3A98(c); }
static inline void kz3_fib_restore(ppu_context* c) { func_000C3BAC(c); }
#else
/* 1.14: lis 0x13B / stw -0x56BC at 0x000C59AC..0x000C59BC = 0x013AA944 (run_all's twin, same layout as 1.00) */
static constexpr uint32_t kSchedCur = 0x013AA944u;
static inline void kz3_fib_save(ppu_context* c) { func_000C5A38(c); }
static inline void kz3_fib_restore(ppu_context* c) { func_000C5B4C(c); }
#endif
namespace {
const uint32_t kSlot0 = 0x288, kSlotSize = 0x18, kSlots = 5;
struct FiberSched {
    uint32_t S; guest_fiber* home; guest_fiber* slot[kSlots]; guest_fiber* dead;
    uint32_t arg[kSlots];            /* each slot's fiber argument (pass 32 stall diagnostic) */
    uint32_t entries, count;         /* the run_all list, for the stall print */
    uint32_t wait_lr[kSlots];        /* each slot's wait-site lr (pass 34 stall classification) */
};
thread_local FiberSched t_fs[4];
thread_local int t_fs_n;
int g_fiber_log = -1;
unsigned long long g_fiber_runs, g_fiber_jobs, g_fiber_waits, g_fiber_finishes;
/* Watchdog (KZ3_FIBER_WATCHDOG=seconds, default 10, 0 = off): a host thread that reports a guest fiber running
 * that long without a switch, with the guest back chain read from the shared register block (racy; diagnostic). */
ppu_context* volatile g_wd_ctx;
volatile uint32_t g_wd_S, g_wd_arg, g_wd_slot;
volatile unsigned long long g_wd_seq;   /* bumped at every fiber switch */
volatile int g_wd_in_job;

void wd_mark(int in_job, uint32_t S, uint32_t slot, uint32_t arg)
{
    g_wd_in_job = in_job; g_wd_S = S; g_wd_slot = slot; g_wd_arg = arg; g_wd_seq++;
}

void wd_thread(void*)
{
    const char* e = getenv("KZ3_FIBER_WATCHDOG");
    const double limit = e ? atof(e) : 10.0;
    unsigned long long seen = ~0ull;
    double since = 0, next = 0;
    const char* se = getenv("KZ3_FIBER_SAMPLE");
    const double sample = se ? atof(se) : 0, t_start = guest_fiber_seconds();
    double sample_next = 0;
    for (;;) {
        guest_fiber_sleep_ms(1000);
        double t = guest_fiber_seconds();
        unsigned long long q = g_wd_seq;
        if (sample > 0 && t >= sample_next && g_wd_ctx) {
            /* KZ3_FIBER_SAMPLE=period: where the scheduler's thread is, whether or not a fiber runs. */
            sample_next = t + sample;
            ppu_context* c = g_wd_ctx;
            fprintf(stderr, "[fiber] sample t=%.0f in_job=%d lr=0x%08X ctr=0x%08X r1=0x%08X r3=0x%08X chain:", t - t_start,
                    (int)g_wd_in_job, (uint32_t)c->lr, (uint32_t)c->ctr, (uint32_t)c->gpr[1], (uint32_t)c->gpr[3]);
            uint32_t sp = (uint32_t)c->gpr[1];
            for (int i = 0; i < 24 && sp && sp < 0xF0000000u; i++) {
                uint32_t back = (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + sp));
                if (!back || back <= sp || back - sp > 0x100000) break;
                fprintf(stderr, " 0x%08X", (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + back + 0x10)));
                sp = back;
            }
            fprintf(stderr, "\n");
        }
        if (q != seen) { seen = q; since = t; next = t + limit; continue; }
        if (!g_wd_in_job || t < next || !g_wd_ctx) continue;
        next = t + 30;
        ppu_context* c = g_wd_ctx;
        fprintf(stderr, "[fiber] watchdog: S=0x%08X slot %u arg 0x%08X running %.0f s without a switch; lr=0x%08X "
                "ctr=0x%08X r1=0x%08X r3=0x%08X chain:", (uint32_t)g_wd_S, (uint32_t)g_wd_slot, (uint32_t)g_wd_arg,
                t - since, (uint32_t)c->lr, (uint32_t)c->ctr, (uint32_t)c->gpr[1], (uint32_t)c->gpr[3]);
        uint32_t sp = (uint32_t)c->gpr[1];
        for (int i = 0; i < 24 && sp && sp < 0xF0000000u; i++) {
            uint32_t back = (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + sp));
            if (!back || back <= sp || back - sp > 0x100000) break;
            fprintf(stderr, " 0x%08X", (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + back + 0x10)));
            sp = back;
        }
        fprintf(stderr, "\n");
    }
}

inline uint32_t gr32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(vm_base + a)); }
inline void gw32(uint32_t a, uint32_t v) { *(uint32_t*)(vm_base + a) = __builtin_bswap32(v); }
bool flog()
{
    if (g_fiber_log < 0) { const char* e = getenv("KZ3_FIBER_LOG"); g_fiber_log = e ? atoi(e) : 24; }
    return g_fiber_log > 0 && g_fiber_log-- > 0;
}
FiberSched* find_sched(uint32_t S)
{
    for (int i = t_fs_n - 1; i >= 0; i--) if (t_fs[i].S == S) return &t_fs[i];
    return nullptr;
}
int slot_index(uint32_t S, uint32_t slot)
{
    uint32_t off = slot - S - kSlot0;
    return (off % kSlotSize == 0 && off / kSlotSize < kSlots) ? (int)(off / kSlotSize) : -1;
}

/* Body of every guest fiber: the register block was set up as start() leaves it, so call LR. */
void job_body(void* vctx, void* arg)
{
    ppu_context* ctx = (ppu_context*)vctx;
    FiberSched* fs = (FiberSched*)arg;
    uint32_t entry = (uint32_t)ctx->lr;
    ctx->ctr = entry;
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f(ctx); }
    /* The original would blr to LR (= entry); finish() is the only way out of a fiber. Treat it as finish. */
    fprintf(stderr, "[fiber] S=0x%08X entry 0x%08X returned without finish(); freeing its slot\n", fs->S, entry);
    uint32_t slot = gr32(fs->S + 0x280);
    gw32(slot + 4, 0);
    int i = slot_index(fs->S, slot);
    if (i >= 0) fs->slot[i] = nullptr;
    fs->dead = guest_fiber_current();
    guest_fiber_switch(fs->home);
}

unsigned long long g_fiber_forced, g_fiber_rekicks, g_fiber_completions;
const double deadlock_s = [] { const char* e = getenv("KZ3_FIBER_DEADLOCK"); return e ? atof(e) : 2.0; }();
/* Pass 34 (RE pass 67/68): the stalled waits of rsx r32c / spu-31a / gp-t2 are DeferredSchedule arena waits
 * (kz3_fiber_stall.h). Report per stuck slot: watched word, wanted value, wait-site lr and class; then the WWS
 * queue-0 entry (job list EA, SPURS instance, workload id, lock), the list header line's first words, the job
 * manager workload's ready count (host copy and instance byte), state and contention, and each virtual SPU's PC
 * with its resident job module (a PC in the module's PUTLLC retry loop = the completion store live-locking). */
/* The queue table: KZ3_DS_QUEUE_TABLE (hex) or the first of the v1.00 / 1.14 tables whose entry 0 names a workload
 * the runtime owns on the same SPURS instance. */
uint32_t queue_table;
struct DsQueue { uint32_t list, spurs, wid, lock; bool valid; };
DsQueue ds_queue_at(uint32_t t)
{
    DsQueue q{};
    if (!t || t >= 0xF0000000u) return q;
    q.list = gr32(t + 4); q.spurs = gr32(t + 8); q.wid = gr32(t + 0xC); q.lock = gr32(t + 0x3C);
    uint32_t spurs_rt = 0;
    q.valid = q.wid < 16 && kz3_spurs_ready_peek(q.wid, nullptr, nullptr, nullptr, nullptr, &spurs_rt) && spurs_rt == q.spurs;
    return q;
}
DsQueue ds_queue0()
{
    static const uint32_t env = [] { const char* e = getenv("KZ3_DS_QUEUE_TABLE"); return e ? (uint32_t)strtoul(e, nullptr, 16) : 0u; }();
    const uint32_t cands[3] = { env, KZ3_DS_QUEUE_TABLE_100, KZ3_DS_QUEUE_TABLE_114 };
    DsQueue q{};
    for (uint32_t t : cands) {
        if (!t) continue;
        q = ds_queue_at(t);
        queue_table = t;
        if (q.valid) break;
    }
    return q;
}
/* Prints the report; returns the number of DeferredSchedule-class stuck slots and, in *other, the lowest stuck
 * slot of any other class (-1 if none). */
int stall_report(FiberSched* fs, uint32_t S, uint32_t cur, double secs, int* other)
{
    int ds = 0;
    *other = -1;
    fprintf(stderr, "[fiber] S=0x%08X waiting %.0f s (list %u entries, cursor %u):", S, secs, fs->count,
            fs->entries ? (cur - fs->entries) / 8 : 0);
    for (uint32_t i = 0; i < kSlots; i++) {
        uint32_t sl = S + kSlot0 + i * kSlotSize, w = gr32(sl);
        if (gr32(sl + 4) && w) fprintf(stderr, " [%u] *0x%08X=0x%X want 0x%X", i, w, gr32(w), gr32(sl + 0x14));
    }
    fprintf(stderr, "\n");
    for (uint32_t i = 0; i < kSlots; i++) {
        uint32_t sl = S + kSlot0 + i * kSlotSize, w = gr32(sl);
        if (!gr32(sl + 4) || !w) continue;
        int slot = -1, cls = kz3_ds_classify(w, fs->wait_lr[i], &slot);
        const char* name = cls == KZ3_STALL_DS_STATIC ? "DeferredSchedule static arena" :
                           cls == KZ3_STALL_DS_LR ? "DeferredSchedule heap arena (by wait lr)" : "other (force-resume class)";
        fprintf(stderr, "[fiber]   slot %u arg 0x%08X waits on 0x%08X site lr 0x%08X: %s", i, fs->arg[i], w, fs->wait_lr[i], name);
        if (cls == KZ3_STALL_DS_STATIC) fprintf(stderr, " slot %d (arena 0x%08X flags 0x%X)", slot, w - KZ3_DS_ARENA_FLAG, gr32(w - KZ3_DS_ARENA_FLAG));
        fprintf(stderr, "\n");
        if (cls != KZ3_STALL_OTHER) ds++;
        else if (*other < 0) *other = (int)i;
    }
    if (!ds) return 0;
    DsQueue q = ds_queue0();
    fprintf(stderr, "[fiber]   queue0 table 0x%08X: list=0x%08X spurs=0x%08X wid=%u lock=%u%s", queue_table, q.list, q.spurs,
            q.wid, q.lock, q.valid ? "" : " (no table matches the runtime's workloads; wid 0 = jm_hiprio assumed, RE pass 70/71)");
    uint32_t wid = q.valid ? q.wid : 0;
    if (q.list && q.list < 0xF0000000u) {
        fprintf(stderr, " header:");
        for (uint32_t k = 0; k < 8; k++) fprintf(stderr, " %08X", gr32(q.list + 4 * k));
    }
    fprintf(stderr, "\n");
    uint32_t rh = 0, ri = 0, st = 0, cc = 0, sea = 0;
    int img = kz3_spurs_workload_image(wid);
    if (kz3_spurs_ready_peek(wid, &rh, &ri, &st, &cc, &sea))
        fprintf(stderr, "[fiber]   workload %u (%s, image %d): ready host=%u inst=%u state=%u curcont=%u spurs=0x%08X\n", wid,
                kz3_spu_image_name_of(img), img, rh, ri, st, cc, sea);
    else
        fprintf(stderr, "[fiber]   workload %u: not in use\n", wid);
    kz3_spurs_dump_runs("[fiber]  ");
    for (uint32_t n = 0; n < 6; n++) {
        uint32_t pc = 0; int ovl = 0, pol = 0, inloop = 0;
        if (!kz3_ds_spu_state(wid, n, &pc, &ovl, &pol, &inloop)) continue;
        fprintf(stderr, "[fiber]   spu %u: pc=0x%05X (%s) policy=%d module=%d (%s)%s\n", n, pc,
                ovl ? "in a job module" : kz3_jm_pc_class_name(kz3_jm_pc_class(pc)), pol, ovl, kz3_spu_image_name_of(ovl),
                inloop ? " IN THE PUTLLC RETRY LOOP (completion store live-lock)" : "");
    }
    return ds;
}
/* @0x000C3864 schedule(S), run on the host fiber that called run_all. Returns when every fiber is done. */
void schedule(ppu_context* ctx, FiberSched* fs)
{
    const uint32_t S = fs->S;
    kz3_ds_breaker brk{};
    bool spinning = false;
    for (;;) {
        if (fs->dead) { guest_fiber_destroy(fs->dead); fs->dead = nullptr; }
        uint32_t free_slot = 0;
        bool idle = true, resumed = false;
        if (gr32(S + 0x284) != 0) {
            for (uint32_t i = 0; i < kSlots; i++) {
                uint32_t sl = S + kSlot0 + i * kSlotSize;
                if (gr32(sl + 4) == 0) { free_slot = sl; continue; }
                uint32_t w = gr32(sl);
                if (w && gr32(w) == gr32(sl + 0x14)) {
                    gw32(sl, 0);
                    gw32(S + 0x280, sl);
                    gw32(S + 0x284, gr32(S + 0x284) - 1);
                    if (!fs->slot[i]) { fprintf(stderr, "[fiber] S=0x%08X slot %u ready but no host fiber\n", S, i); abort(); }
                    if (flog()) fprintf(stderr, "[fiber] S=0x%08X resume slot %u (*0x%08X=0x%X)\n", S, i, w, gr32(w));
                    wd_mark(1, S, i, 0);
                    guest_fiber_switch(fs->slot[i]);
                    wd_mark(0, S, i, 0);
                    resumed = true;
                    break;
                }
                if (!w) fprintf(stderr, "[fiber] S=0x%08X slot %u busy without a watch (count %u)\n", S, i, gr32(S + 0x284));
                idle = false;
            }
            if (resumed) { spinning = false; continue; }
        } else {
            free_slot = S + kSlot0;
        }
        uint32_t cur = gr32(S + 0x300), end = gr32(S + 0x304);
        if (cur < end && free_slot) {
            int i = slot_index(S, free_slot);
            gw32(free_slot + 4, cur);
            gw32(S + 0x280, free_slot);
            gw32(S + 0x300, cur + 8);
            uint32_t opd = gr32(cur), arg = gr32(cur + 4), sp = gr32(free_slot + 0xC);
            guest_fiber* f = guest_fiber_create(job_body, fs, 0);
            ppu_context* st = (ppu_context*)guest_fiber_state(f);
            st->gpr[0] = gr32(opd); st->gpr[2] = gr32(opd + 4); st->gpr[1] = sp; st->lr = st->gpr[0];
            st->gpr[3] = arg; st->gpr[4] = S; st->gpr[5] = opd; st->gpr[6] = sp; st->gpr[7] = sp;
            fs->slot[i] = f;
            fs->arg[i] = arg;
            fs->wait_lr[i] = 0;
            g_fiber_jobs++;
            if (flog()) fprintf(stderr, "[fiber] S=0x%08X start slot %d opd 0x%08X entry 0x%08X arg 0x%08X sp 0x%08X\n",
                                S, i, opd, (uint32_t)st->gpr[0], arg, sp);
            wd_mark(1, S, (uint32_t)i, arg);
            guest_fiber_switch(f);
            wd_mark(0, S, (uint32_t)i, arg);
            spinning = false;
            continue;
        }
        if (idle) return;
        /* Every busy fiber waits for a value written by an SPU job (DeferredSchedule) or by another fiber. A real
         * wait ends within the same run_all, i.e. within one frame; a stall that outlives KZ3_FIBER_DEADLOCK
         * seconds is handled by class (kz3_fiber_stall.h): DeferredSchedule waits are never forced; the job
         * manager workload is re-kicked at one period and the completion value written at two periods. Any other
         * class gets the round-32 breaker: its fiber resumes as if the wait had been met. 0 = report only. */
        double t = guest_fiber_seconds();
        if (!spinning) { spinning = true; kz3_ds_breaker_reset(&brk, t); }
        int act = kz3_ds_breaker_step(&brk, t, deadlock_s);
        if (act) {
            int other = -1;
            int ds = stall_report(fs, S, cur, t - brk.t0, &other);
            if (ds && (act & KZ3_DS_REKICK)) {
                DsQueue q = ds_queue0();
                uint32_t wid = q.valid ? q.wid : 0;
                uint32_t old = kz3_spurs_ready_kick(wid, 8);
                g_fiber_rekicks++;
                fprintf(stderr, "[fiber] stall workaround 1 after %.1f s: ReadyCountSwap(wid %u, 8) re-issued (old %u); re-kicks so far %llu\n",
                        t - brk.t0, wid, old, g_fiber_rekicks);
            }
            if (ds && (act & KZ3_DS_COMPLETE)) {
                /* KZ3_FIBER_COMPLETE=1 opts in (RE pass 70: the preferred way out is the runtime's run budget,
                 * KZ3_SPURS_RUN_MS, patch 1048, which loses nothing); off, the report says what it would write. */
                static const bool complete_on = [] { const char* e = getenv("KZ3_FIBER_COMPLETE"); return e && atoi(e) > 0; }();
                for (uint32_t i = 0; i < kSlots; i++) {
                    uint32_t sl = S + kSlot0 + i * kSlotSize, w = gr32(sl);
                    if (!gr32(sl + 4) || !w || kz3_ds_classify(w, fs->wait_lr[i], nullptr) == KZ3_STALL_OTHER) continue;
                    if (!complete_on) {
                        fprintf(stderr, "[fiber] stall workaround 2 after %.1f s: would write the completion value *0x%08X = 0x%X for slot %u "
                                "(KZ3_FIBER_COMPLETE=1 enables it; loses this entity's push-buffer section for one frame)\n",
                                t - brk.t0, w, gr32(sl + 0x14), i);
                        continue;
                    }
                    g_fiber_completions++;
                    fprintf(stderr, "[fiber] stall workaround 2 after %.1f s: writing the completion value *0x%08X = 0x%X for slot %u "
                            "(was 0x%X; this entity's push-buffer section is lost for one frame); completions so far %llu\n",
                            t - brk.t0, w, gr32(sl + 0x14), i, gr32(w), g_fiber_completions);
                    gw32(w, gr32(sl + 0x14));
                }
                continue;   /* the scheduler's own check resumes the fibers */
            }
            if (!ds && other >= 0 && deadlock_s > 0 && t - brk.t0 >= deadlock_s) {
                int pick = other;
                uint32_t sl = S + kSlot0 + (uint32_t)pick * kSlotSize, w = gr32(sl);
                g_fiber_forced++;
                fprintf(stderr, "[fiber] deadlock: resuming slot %d (arg 0x%08X) after %.1f s without its wait on *0x%08X "
                        "(=0x%X want 0x%X) being met; forced resumes so far %llu\n", pick, fs->arg[pick], t - brk.t0, w,
                        gr32(w), gr32(sl + 0x14), g_fiber_forced);
                gw32(sl, 0);
                gw32(S + 0x280, sl);
                gw32(S + 0x284, gr32(S + 0x284) - 1);
                wd_mark(1, S, (uint32_t)pick, fs->arg[pick]);
                guest_fiber_switch(fs->slot[pick]);
                wd_mark(0, S, (uint32_t)pick, fs->arg[pick]);
                spinning = false;
                continue;
            }
        }
        guest_fiber_yield_thread();
    }
}
} // namespace

/* @0x000C3A54 run_all(S, list) */
static void kz3_fiber_run_all(ppu_context* ctx)
{
    const uint32_t S = (uint32_t)ctx->gpr[3], list = (uint32_t)ctx->gpr[4];
    const uint32_t count = list ? gr32(list) : 0, entries = list ? gr32(list + 4) : 0;
    g_fiber_runs++;
    if (flog())
        fprintf(stderr, "[fiber] run_all #%llu S=0x%08X list=0x%08X count=%u entries=0x%08X first={0x%08X,0x%08X} lr=0x%08X "
                "(jobs so far %llu, waits %llu)\n", g_fiber_runs, S, list, count, entries, count ? gr32(entries) : 0,
                count ? gr32(entries + 4) : 0, (uint32_t)ctx->lr, g_fiber_jobs, g_fiber_waits);
    if (!S || !count || t_fs_n >= 4 || find_sched(S)) {
        fprintf(stderr, "[fiber] run_all S=0x%08X count=%u depth %d: not run (NULL scheduler, empty list or nesting)\n",
                S, count, t_fs_n);
        return;
    }
    ctx->gpr[0] = ctx->lr;
    kz3_fib_save(ctx);                       /* save(S): the caller's context, as the original */
    gw32(kSchedCur, S);                       /* @0x000C39E8 */
    gw32(S + 0x284, 0);
    gw32(S + 0x280, 0);
    gw32(S + 0x300, entries);
    gw32(S + 0x304, entries + count * 8);
    FiberSched* fs = &t_fs[t_fs_n++];
    memset(fs, 0, sizeof *fs);
    fs->S = S;
    fs->entries = entries;
    fs->count = count;
    fs->home = guest_fiber_self(ctx, sizeof *ctx);
    /* Patch 1031: the runtime's indirect-call depth guard (and its vcall stack) count per fiber. finish() leaves a
     * job fiber from inside job_body's ps3_indirect_call, so with one count per host thread every finished job
     * leaked one level; after ~4000 jobs on a thread (~500 s into the level) the guard skipped every indirect call
     * there and the game state fell apart (SPU pass 19). Thread-safe one-time registration (static init). */
    static const bool locals_added = [] {
        guest_fiber_add_local(ps3_ppu_dispatch_depth_slot);
        guest_fiber_add_local(ps3_ppu_vcall_sp_slot);
        return true;
    }();
    (void)locals_added;
    install_put_note();
    if (!g_wd_ctx) {
        g_wd_ctx = ctx;
        const char* e = getenv("KZ3_FIBER_WATCHDOG");
        if (!e || atof(e) > 0) guest_fiber_spawn_thread(wd_thread, nullptr);
    }
    fs->home = guest_fiber_current();
    schedule(ctx, fs);
    if (fs->dead) guest_fiber_destroy(fs->dead);
    t_fs_n--;
    if (flog()) fprintf(stderr, "[fiber] run_all S=0x%08X done (jobs %llu, waits %llu, finishes %llu)\n", S, g_fiber_jobs,
                        g_fiber_waits, g_fiber_finishes);
    gw32(S + 0x280, 0);                       /* the done path of @0x000C3864 */
    gw32(S + 0x300, 0);
    gw32(S + 0x304, 0);
    gw32(kSchedCur, 0);
    ctx->gpr[3] = S;
    kz3_fib_restore(ctx);                       /* restore(S): registers as the original returns them */
}

/* @0x000C3A68 wait(S, ptr): runs on a guest fiber. */
static void kz3_fiber_wait(ppu_context* ctx, void (*orig)(ppu_context*))
{
    const uint32_t S = (uint32_t)ctx->gpr[3];
    FiberSched* fs = find_sched(S);
    if (!fs || guest_fiber_current() == fs->home) {
        fprintf(stderr, "[fiber] wait S=0x%08X outside a fiber; running the lifted body\n", S);
        orig(ctx);
        return;
    }
    const uint32_t slot = gr32(S + 0x280), buf = gr32(slot + 0x10);
    g_fiber_waits++;
    ctx->gpr[5] = slot;
    ctx->gpr[7] = S;
    ctx->gpr[3] = buf;
    ctx->gpr[0] = ctx->lr;
    kz3_fib_save(ctx);                       /* save(buf) */
    ctx->gpr[3] = S;
    gw32(slot, (uint32_t)ctx->gpr[4]);
    gw32(S + 0x284, gr32(S + 0x284) + 1);
    {   /* The wait site: the lr of wait(S, ptr, value)'s caller when we are inside that stub, else our lr. */
        uint32_t lr = (uint32_t)ctx->lr;
        if (kz3_ds_lr_in_wait_stub(lr))
            lr = (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + (uint32_t)ctx->gpr[1] + KZ3_DS_WAIT_STUB_LR_SLOT));
        int si = slot_index(S, slot);
        if (si >= 0) fs->wait_lr[si] = lr;
    }
    if (flog()) fprintf(stderr, "[fiber] S=0x%08X wait slot %d on *0x%08X=0x%X want 0x%X (lr 0x%08X site 0x%08X)\n", S,
                        slot_index(S, slot), (uint32_t)ctx->gpr[4], gr32((uint32_t)ctx->gpr[4]), gr32(slot + 0x14),
                        (uint32_t)ctx->lr, slot_index(S, slot) >= 0 ? fs->wait_lr[slot_index(S, slot)] : 0);
    guest_fiber_switch(fs->home);
    ctx->gpr[3] = buf;                        /* resumed by schedule(): restore(buf) */
    kz3_fib_restore(ctx);
}

/* @0x000C39BC finish(S): runs on a guest fiber and never returns. */
static void kz3_fiber_finish(ppu_context* ctx, void (*orig)(ppu_context*))
{
    const uint32_t S = (uint32_t)ctx->gpr[3];
    FiberSched* fs = find_sched(S);
    if (!fs || guest_fiber_current() == fs->home) {
        fprintf(stderr, "[fiber] finish S=0x%08X outside a fiber; running the lifted body\n", S);
        orig(ctx);
        return;
    }
    const uint32_t slot = gr32(S + 0x280);
    gw32(slot + 4, 0);
    int i = slot_index(S, slot);
    if (i >= 0) fs->slot[i] = nullptr;
    g_fiber_finishes++;
    if (flog()) fprintf(stderr, "[fiber] S=0x%08X finish slot %d (#%llu)\n", S, i, g_fiber_finishes);
    fs->dead = guest_fiber_current();
    guest_fiber_switch(fs->home);
    fprintf(stderr, "[fiber] finished fiber resumed (S=0x%08X)\n", S);
    abort();
}
#ifdef KZ3_FIBER_V100
void func_000C3A54(ppu_context* ctx) { kz3_fiber_run_all(ctx); }
void func_000C3A68(ppu_context* ctx) { kz3_fiber_wait(ctx, func_000C3A68__lifted); }
void func_000C39BC(ppu_context* ctx) { kz3_fiber_finish(ctx, func_000C39BC__lifted); }
#endif
#ifdef KZ3_FIBER_V114
void func_000C59F4(ppu_context* ctx) { kz3_fiber_run_all(ctx); }
void func_000C5A08(ppu_context* ctx) { kz3_fiber_wait(ctx, func_000C5A08__lifted); }
void func_000C595C(ppu_context* ctx) { kz3_fiber_finish(ctx, func_000C595C__lifted); }
#endif
#endif

#if defined(PPU_OVERRIDE_000640E0) || defined(PPU_OVERRIDE_00064F88)
/* Per-version data (1.14 twin 0x00064F88, 77/77 insns, pass 95; values from the RE notes s.35): the GCM IO table
 * base/stride words, the renderer global G, the throttle's call-site lr. The r31+0x11294 command-context field is unchanged. */
#ifdef PPU_OVERRIDE_000640E0
static constexpr uint32_t kLblIoBase = 0x01594914u, kLblIoStride = 0x01594918u, kLblG = 0x0133163Cu, kLblThrottleLr = 0x001ECD58u;
#else
static constexpr uint32_t kLblIoBase = 0x0160A214u, kLblIoStride = 0x0160A218u, kLblG = 0x013A56BCu, kLblThrottleLr = 0x001F84C8u;
#endif
/* @0x000640E0 wait_label(r3, index, wanted) (ASSUMPTION: name from behaviour): polls
 * *( [0x01594914] + index * [0x01594918] ) until it is >= wanted (unsigned), sys_timer_usleep(30) between polls.
 * Pass 14 diagnostic: the level's one-frame-ahead throttle @0x001ECD18..0x001ECDD0 waits here on the allocator
 * label [obj+0x13BD8] for value [obj+0x13BDC] and then queues NV406E 0x64/0x6C (label, value+1) into the command
 * context ld [r31+0x11290] (RSX pass 13). This override polls the same word on the host first, so a stuck wait can
 * be reported (label word, the caller's command context begin/end/current, the runtime's FIFO control put/get),
 * then runs the original, which returns at once. KZ3_LABEL_LOG=N logs the first N waits (default 16); a wait still
 * unsatisfied after 2 s is logged every 5 s. Guest-visible behaviour is unchanged. */
#include "guest_fiber.h"
#include <chrono>
#include <thread>
#include <initializer_list>
extern "C" uint32_t ppu_hle_inject_base;
static inline uint32_t kz3_rd32(uint32_t ea) { return __builtin_bswap32(*(uint32_t*)(vm_base + ea)); }
/* Patch 1032 wait accounting (runtime/diag/kz3_wait_acct.h; KZ3_WA_LABEL = 3). */
extern "C" int g_kz3_wacct;
extern "C" int kz3_wacct_init(void);
extern "C" uint64_t kz3_wacct_begin_slow(void);
extern "C" void kz3_wacct_end(int kind, uint32_t site, uint64_t t0);
static void kz3_wait_label_body(ppu_context* ctx, void (*orig)(ppu_context*));
static void kz3_wait_label(ppu_context* ctx, void (*orig)(ppu_context*))
{
    const int on = g_kz3_wacct < 0 ? kz3_wacct_init() : g_kz3_wacct;
    const uint64_t wa0 = on ? kz3_wacct_begin_slow() : 0;
    const uint32_t idx = (uint32_t)ctx->gpr[4];
    kz3_wait_label_body(ctx, orig);
    kz3_wacct_end(3, idx, wa0);
}
static void kz3_wait_label_body(ppu_context* ctx, void (*orig)(ppu_context*))
{
    static int lim = -1;
    if (lim < 0) { const char* e = getenv("KZ3_LABEL_LOG"); lim = e ? atoi(e) : 16; }
    static unsigned long long calls = 0;
    calls++;
    uint32_t idx = (uint32_t)ctx->gpr[4], want = (uint32_t)ctx->gpr[5], lr = (uint32_t)ctx->lr;
    uint32_t base = kz3_rd32(kLblIoBase), stride = kz3_rd32(kLblIoStride);
    uint32_t ea = base + idx * stride;
    uint32_t r31 = (uint32_t)ctx->gpr[31];
    auto describe = [&](const char* what, double secs) {
        uint32_t cc = (lr == kLblThrottleLr && r31) ? kz3_rd32(r31 + 0x11294) : 0;  /* ld: 64-bit field, low word */
        uint32_t ctl = ppu_hle_inject_base + 0x2000u;
        fprintf(stderr, "[label] %s #%llu lr=0x%08X idx=0x%X want=0x%X word@0x%08X=0x%X (base 0x%08X stride %u) "
                "t=%.1fs", what, calls, lr, idx, want, ea, kz3_rd32(ea), base, stride, secs);
        if (cc)
            fprintf(stderr, " cmdctx=0x%08X [begin 0x%08X end 0x%08X cur 0x%08X cb 0x%08X]", cc, kz3_rd32(cc),
                    kz3_rd32(cc + 4), kz3_rd32(cc + 8), kz3_rd32(cc + 12));
        fprintf(stderr, " ctrl put=0x%X get=0x%X ref=0x%X\n", kz3_rd32(ctl + 0), kz3_rd32(ctl + 4),
                kz3_rd32(ctl + 8));
        /* The renderer's command contexts G+0x10/+0x90/+0xF0 (G = [0x0133163C]; +0x70/+0x74 = placeholder P / P+0xC),
         * and where the NV406E pairs for this label sit in the throttle's context [begin, current). */
        uint32_t g = kz3_rd32(kLblG);
        if (g && (what[0] == 'S' || lr == kLblThrottleLr))
            for (uint32_t o : {0x10u, 0x90u, 0xF0u}) {
                uint32_t c = g + o;
                fprintf(stderr, "[label]   ctx G+0x%02X=0x%08X begin 0x%08X end 0x%08X cur 0x%08X P 0x%08X ret 0x%08X\n",
                        o, c, kz3_rd32(c), kz3_rd32(c + 4), kz3_rd32(c + 8), kz3_rd32(c + 0x70), kz3_rd32(c + 0x74));
            }
        if (cc && what[0] == 'S') {
            static int scans = 0;
            uint32_t b = kz3_rd32(cc), cur = kz3_rd32(cc + 8);
            if (scans++ < 2 && b && cur > b && cur - b < 0x400000u) {
                int n = 0;
                for (uint32_t a = b; a + 12 <= cur && n < 16; a += 4)
                    if (kz3_rd32(a) == 0x00040064u && kz3_rd32(a + 4) == (idx << 4)) {
                        fprintf(stderr, "[label]   pair at 0x%08X (+0x%X): %08X %08X %08X %08X\n", a, a - b,
                                kz3_rd32(a), kz3_rd32(a + 4), kz3_rd32(a + 8), kz3_rd32(a + 12));
                        n++;
                    }
                fprintf(stderr, "[label]   %d pair(s) for 0x%X in [0x%08X, 0x%08X); words at begin: %08X %08X %08X %08X\n",
                        n, idx << 4, b, cur, kz3_rd32(b), kz3_rd32(b + 4), kz3_rd32(b + 8), kz3_rd32(b + 12));
            }
        }
    };
    static int throttle_logged = 0;
    if (calls <= (unsigned long long)lim || (lr == kLblThrottleLr && throttle_logged++ < lim)) describe("wait", 0.0);
    if (kz3_rd32(ea) < want) {
        auto t0 = std::chrono::steady_clock::now();
        double next = 2.0;
        while (kz3_rd32(ea) < want) {
            std::this_thread::sleep_for(std::chrono::microseconds(30));   /* the original sleeps 30 us too */
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (s >= next) { describe("STUCK", s); next += 5.0; }
        }
    }
    orig(ctx);
}
#ifdef PPU_OVERRIDE_000640E0
void func_000640E0(ppu_context* ctx) { kz3_wait_label(ctx, func_000640E0__lifted); }
#endif
#ifdef PPU_OVERRIDE_00064F88
void func_00064F88(ppu_context* ctx) { kz3_wait_label(ctx, func_00064F88__lifted); }
#endif
#endif

#if defined(PPU_OVERRIDE_001FF008) || defined(PPU_OVERRIDE_0020A958)   /* 1.14 twin 0x0020A958: identical body */
/* @0x001FF008 queue_mlaa(renderer, 0, 0) (ASSUMPTION: name from behaviour; 1.14 @0x0020A958). Called once per frame
 * from @0x001FF48C (bl at 0x001FF744; 1.14: 0x0020B094 in 0x0020ADDC); it only queues the SPU jobs Transpose, MLAA,
 * Transpose, MLAA (post job 0x26, ovl230 in the profiler, one of the two hottest SPU jobs) with 0x001FBE64/0x0001BE2C
 * and writes their parameter block at renderer+0x2D50. Its return value is unused and nothing waits on these jobs
 * themselves: the frame's completion is the list-end Unlock clearing renderer+0x18EC (the RE notes section 18).
 * ENHANCEMENT: when the host AA pass is enabled (patch 2023, KZ3_HOST_AA unset or =fxaa) the jobs are not queued and
 * the host FXAA pass is armed to run where MLAA's output would land; KZ3_HOST_AA=off keeps the game's own MLAA. */
extern "C" int  rsx_live_draw_host_aa_mode(void);   /* patch 2023 */
extern "C" void rsx_live_draw_host_aa_arm(int on);  /* patch 2023 */
static void kz3_mlaa_body(ppu_context* ctx, void (*orig)(ppu_context*), uint32_t addr)
{
    if (rsx_live_draw_host_aa_mode()) {
        static int s_logged = 0;
        if (!s_logged++) fprintf(stderr, "[host-aa] SPU MLAA skipped (@0x%08X), host FXAA armed\n", addr);
        rsx_live_draw_host_aa_arm(1);
        return;
    }
    orig(ctx);
}
#ifdef PPU_OVERRIDE_001FF008
void func_001FF008(ppu_context* ctx) { kz3_mlaa_body(ctx, func_001FF008__lifted, 0x001FF008u); }
#endif
#ifdef PPU_OVERRIDE_0020A958
void func_0020A958(ppu_context* ctx) { kz3_mlaa_body(ctx, func_0020A958__lifted, 0x0020A958u); }
#endif
#endif

#if defined(PPU_OVERRIDE_000284A0) || defined(PPU_OVERRIDE_000284D8)
/* 1.14 patch set selectors (the RE notes s.31/s.33/s.37, pass 155). SelectPatchSetSP @0x000284A0 and
 * SelectPatchSetMP @0x000284D8 are the only calls that change the FIOS patch directory ("/patch_data_ps3_sp" / "_mp",
 * via 0xE0A280) after the inline startup default (0x2D5C0, SP). Their callers are the mode setters 0x8B64FC / 0x8B6DE4.
 * The lifted body still runs (it only writes the guest FIOS object, which the port bypasses); then the overlay's
 * patch directory follows, so later opens of localcacheps3 lumps and gums see the same set the game believes it has.
 * Without this, Botzone (mode 2 / MP data) gets the 1.00 MP lumps under the SP set and stops at Syncing (net 34). */
extern "C" void kz3_vfs_follow_patch_set(int set, unsigned lr);   /* port/kz3/psarc_vfs.cpp */
#ifdef PPU_OVERRIDE_000284A0
void func_000284A0(ppu_context* ctx) { unsigned lr = (unsigned)ctx->lr; func_000284A0__lifted(ctx); kz3_vfs_follow_patch_set(0, lr); }
#endif
#ifdef PPU_OVERRIDE_000284D8
void func_000284D8(ppu_context* ctx) { unsigned lr = (unsigned)ctx->lr; func_000284D8__lifted(ctx); kz3_vfs_follow_patch_set(1, lr); }
#endif
#endif

#if defined(PPU_OVERRIDE_001FDA60) || defined(PPU_OVERRIDE_002093B0)   /* 1.14 twin 0x002093B0 (addr_map_114.tsv, L1-unique 0.99) */
/* @0x001FDA60 MotionBlur wrapper (the RE notes s.6; 1.14 @0x002093B0). Called once per viewport from the
 * post chain @0x001FDD40 (`bl` right after the idx5 -> idx2 Upsample); it only allocates a 0x18-byte job record and
 * queues post job 0x20 (Gfx.Post.MotionBlur, mask 1) with 0x001FCBD0 / 0x0001BE2C. Return value unused (void).
 * ENHANCEMENT (rsx patch 2107, Options > Display > Motion blur, display.ini motion_blur, default on): off = the job is
 * not queued, every other post job runs as before. The registration makes port/kz3/main.cpp show the row. */
extern "C" int  kz3_motion_blur_on(void);                 /* port/kz3/main.cpp */
extern "C" void kz3_motion_blur_override_register(void);  /* port/kz3/main.cpp */
[[maybe_unused]] static const int s_kz3_mblur_reg = (kz3_motion_blur_override_register(), 1);
static void kz3_mblur_body(ppu_context* ctx, void (*orig)(ppu_context*), uint32_t addr)
{
    if (kz3_motion_blur_on()) { orig(ctx); return; }
    static int s_logged = 0;
    if (!s_logged++) fprintf(stderr, "[motion-blur] off: post job 0x20 not queued (@0x%08X)\n", addr);
}
#ifdef PPU_OVERRIDE_001FDA60
void func_001FDA60(ppu_context* ctx) { kz3_mblur_body(ctx, func_001FDA60__lifted, 0x001FDA60u); }
#endif
#ifdef PPU_OVERRIDE_002093B0
void func_002093B0(ppu_context* ctx) { kz3_mblur_body(ctx, func_002093B0__lifted, 0x002093B0u); }
#endif
#endif

/* Pass 32: RE pass 64's logger hooks (port/kz3/kz3_log.h). HardExit @0x0008AB70 logs the game's own fatal text
 * with its caller; the link resolver @0x000DDFF0 logs each unresolved dependent link (status 8) once per target.
 * 1.14 (switch-114): KZ3_HOOK_HARDEXIT(0008C360), KZ3_HOOK_LINK_RESOLVE(000DFF30), KZ3_HOOK_CRASHDUMP_FIELD(0008CF08). */
#if defined(PPU_OVERRIDE_0008AB70) || defined(PPU_OVERRIDE_000DDFF0) || defined(PPU_OVERRIDE_0008C360) || \
    defined(PPU_OVERRIDE_000DFF30) || defined(PPU_OVERRIDE_0008CF08)
#include "kz3_log.h"
#endif
#ifdef PPU_OVERRIDE_0008AB70
KZ3_HOOK_HARDEXIT(0008AB70)
#endif
#ifdef PPU_OVERRIDE_000DDFF0
KZ3_HOOK_LINK_RESOLVE(000DDFF0)
#endif
/* 1.14 twins (the RE notes s.34, s.36.7: HardExit's 1.14 body also stores the text as SetCrashDumpField field 3
 * before its deliberate stw 0x0DEADCA7 -> [0]; SetCrashDumpField 0x0008CF08 exists only there) */
#ifdef PPU_OVERRIDE_0008C360
KZ3_HOOK_HARDEXIT(0008C360)
#endif
#ifdef PPU_OVERRIDE_000DFF30
KZ3_HOOK_LINK_RESOLVE(000DFF30)
#endif
#ifdef PPU_OVERRIDE_0008CF08
KZ3_HOOK_CRASHDUMP_FIELD(0008CF08)
#endif
#ifdef PPU_OVERRIDE_000DD600
/* @0x000DD600 (1.14 only) ResourceManager::Find(mgr, query) (RE pass 55; query = {RTTI* type, String* name
 * (length at +8, text inline at +0x10; pass 59), flags}). Prints every lookup that returns 0, the first divergence from
 * v1.00 (GameScript in the movie, BulletSystem after it). KZ3_RES_MISS=0 silences it. Body from switch-114. */
void func_000DD600(ppu_context* ctx)
{
    uint32_t q = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    func_000DD600__lifted(ctx);
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("KZ3_RES_MISS"); s_on = !(e && e[0] == '0'); }
    if (!s_on || (uint32_t)ctx->gpr[3] || !vm_base || q < 0x10000u || q >= 0xF0000000u) return;
    auto rd = [](uint32_t a) -> uint32_t { const uint8_t* b = vm_base + a;
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; };
    uint32_t type = rd(q), sp = rd(q + 4), flags = rd(q + 8);
    char name[65] = "";
    if (sp >= 0x10000u && sp < 0xF0000000u) {
        /* The String's text is inline at +0x10, its length at +8 (RE pass 59; 0x321078 reads it so). */
        uint32_t len = rd(sp + 8);
        int n = 0; for (; n < 64 && (uint32_t)n < len; n++) { uint8_t c = vm_base[sp + 0x10 + n]; if (!c || c < 0x20) break; name[n] = (char)c; } name[n] = 0;
    }
    static int s_count = 0;
    if (s_count++ < 200)
        fprintf(stderr, "[res-miss] type=0x%08X name='%s' flags=0x%08X lr=0x%08X\n", type, name, flags, lr);
}
#endif

/* RE pass 158 (diagnostic, 1.14 only): where does a lump batch stop? KZ3_LUMP_TRACE=0 silences it. Three post-hooks,
 * printed in call order so they interleave with the "[fs] open" lines (the lump is the last .core opened):
 *   [lump-pass] load   0xD9900  (one lump: 1 = loaded and published, 0 = failed; the failure path logs nothing itself)
 *   [lump-pass] objects 0x9488C (the object pass inside it: 0 = ok)
 *   [lump-pass] batch  0x320D38 (the batch driver: package name from batch+0xC, result byte at batch+0x1C, 0 = failed)
 * the RE notes s.44. */
#if defined(PPU_OVERRIDE_000D9900) || defined(PPU_OVERRIDE_0009488C) || defined(PPU_OVERRIDE_00320D38)
static int kz3_lump_trace_on(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("KZ3_LUMP_TRACE"); s_on = !(e && e[0] == '0'); }
    return s_on;
}
static uint32_t kz3_lump_rd(uint32_t a) { const uint8_t* b = vm_base + a;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static int kz3_lump_budget(void) { static int s_n = 0; return s_n++ < 600; }
#endif
#ifdef PPU_OVERRIDE_000D9900
void func_000D9900(ppu_context* ctx)
{
    uint32_t lr = (uint32_t)ctx->lr;
    func_000D9900__lifted(ctx);
    if (kz3_lump_trace_on() && kz3_lump_budget())
        fprintf(stderr, "[lump-pass] load ret=%u lr=0x%08X\n", (uint32_t)ctx->gpr[3], lr);
}
#endif
#ifdef PPU_OVERRIDE_0009488C
void func_0009488C(ppu_context* ctx)
{
    func_0009488C__lifted(ctx);
    if (kz3_lump_trace_on() && kz3_lump_budget())
        fprintf(stderr, "[lump-pass] objects ret=%d\n", (int32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_00320D38
void func_00320D38(ppu_context* ctx)
{
    uint32_t batch = (uint32_t)ctx->gpr[3];
    func_00320D38__lifted(ctx);
    if (!kz3_lump_trace_on() || !vm_base || batch < 0x10000u || batch >= 0xF0000000u || !kz3_lump_budget()) return;
    char name[96] = "";
    uint32_t sp = kz3_lump_rd(batch + 0xC);
    if (sp >= 0x10000u && sp < 0xF0000000u) {
        uint32_t len = kz3_lump_rd(sp + 8); int n = 0;
        for (; n < 95 && (uint32_t)n < len; n++) { uint8_t c = vm_base[sp + 0x10 + n]; if (!c || c < 0x20) break; name[n] = (char)c; }
        name[n] = 0;
    }
    fprintf(stderr, "[lump-pass] batch pkg='%s' result=%u\n", name, (unsigned)vm_base[batch + 0x1C]);
}
#endif

/* RE pass 161 (diagnostic, 1.14 only, builds on the pass 158 hooks above): which stage of the object pass 0x9488C
 * returns the 1 seen at the post-switch defaultshared open, and why. KZ3_LUMP_TRACE=0 silences it. All lines are
 * "[lump-stage] ..." and print only a NON-ZERO stage result (a clean lump prints nothing), except GetLength, which prints once
 * per stream. Stage = vtable slot of the lump reader (vptr 0xF24680) in the order 0x9488C calls them:
 *   0x10 95DFC, 0x14 960B8, 0x24 97AB0, 0x28 981A0, 0x2C DE3A0 (counts), 0x18 9694C, 0x30 98404, 0x34 DE9D4 (resident lookup
 *   0xDC2D0 / create), 0x20 DF530 (object bodies; skips a resident object through 0x9A000).
 * Also the byte stream under them: 0x9359C = refill (1 = the read or GetLength failed), 0x268F4 = GetLength, 0x29BD8 = read
 * (returns 1 = ok). The stream object (ctx+0x14) keeps an error flag at +5 and the error text object at +0x30 (0x26384). */
#if defined(PPU_OVERRIDE_0009359C) || defined(PPU_OVERRIDE_000268F4) || defined(PPU_OVERRIDE_00029BD8) || defined(PPU_OVERRIDE_00095DFC) || \
    defined(PPU_OVERRIDE_000960B8) || defined(PPU_OVERRIDE_00097AB0) || defined(PPU_OVERRIDE_000981A0) || defined(PPU_OVERRIDE_000DE3A0) || \
    defined(PPU_OVERRIDE_0009694C) || defined(PPU_OVERRIDE_00098404) || defined(PPU_OVERRIDE_000DE9D4) || defined(PPU_OVERRIDE_000DF530)
static uint32_t kz3_stg_rd(uint32_t a) { const uint8_t* b = vm_base + a;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static int kz3_stg_ok(uint32_t a) { return vm_base && a >= 0x10000u && a < 0xF0000000u; }
static int kz3_stg_budget(void) { static int s_n = 0; return s_n++ < 400; }
static void kz3_stg_stream(const char* tag, uint32_t stream, uint64_t ret)
{
    if (!kz3_stg_ok(stream)) { fprintf(stderr, "[lump-stage] %s ret=%llu stream=0x%08X (not a pointer)\n", tag, (unsigned long long)ret, stream); return; }
    char txt[49] = ""; int n = 0;
    for (; n < 48; n++) { uint8_t c = vm_base[stream + 0x30 + n]; if (c >= 0x20 && c < 0x7F) txt[n] = (char)c; else txt[n] = '.'; }
    txt[n] = 0;
    fprintf(stderr, "[lump-stage] %s ret=%llu stream=0x%08X err=%u impl=0x%08X handle=0x%08X +30:%s\n", tag, (unsigned long long)ret, stream,
            (unsigned)vm_base[stream + 5], kz3_stg_rd(stream + 0xC), kz3_stg_rd(stream + 0x10), txt);
}
#define KZ3_STAGE_HOOK(ADDR, NAME)                                                                                            \
    void func_##ADDR(ppu_context* ctx)                                                                                        \
    {                                                                                                                         \
        uint32_t self = (uint32_t)ctx->gpr[3];                                                                                \
        func_##ADDR##__lifted(ctx);                                                                                           \
        int32_t r = (int32_t)ctx->gpr[3];                                                                                     \
        if (r != 0 && kz3_lump_trace_on() && kz3_stg_budget()) {                                                              \
            fprintf(stderr, "[lump-stage] %s (0x%s) ret=%d ctx=0x%08X ctx+4=%d\n", NAME, #ADDR, r, self,                 \
                    kz3_stg_ok(self) ? (int32_t)kz3_stg_rd(self + 4) : -1);                                                   \
            if (kz3_stg_ok(self)) kz3_stg_stream(NAME, kz3_stg_rd(self + 0x14), (uint64_t)r);                                 \
        }                                                                                                                     \
    }
#ifdef PPU_OVERRIDE_00095DFC
KZ3_STAGE_HOOK(00095DFC, "stage 0x10")
#endif
#ifdef PPU_OVERRIDE_000960B8
KZ3_STAGE_HOOK(000960B8, "stage 0x14")
#endif
#ifdef PPU_OVERRIDE_00097AB0
KZ3_STAGE_HOOK(00097AB0, "stage 0x24")
#endif
#ifdef PPU_OVERRIDE_000981A0
KZ3_STAGE_HOOK(000981A0, "stage 0x28")
#endif
#ifdef PPU_OVERRIDE_000DE3A0
KZ3_STAGE_HOOK(000DE3A0, "stage 0x2C counts")
#endif
#ifdef PPU_OVERRIDE_0009694C
KZ3_STAGE_HOOK(0009694C, "stage 0x18")
#endif
#ifdef PPU_OVERRIDE_00098404
KZ3_STAGE_HOOK(00098404, "stage 0x30")
#endif
#ifdef PPU_OVERRIDE_000DE9D4
KZ3_STAGE_HOOK(000DE9D4, "stage 0x34 lookup/create")
#endif
#ifdef PPU_OVERRIDE_000DF530
KZ3_STAGE_HOOK(000DF530, "stage 0x20 bodies")
#endif
#ifdef PPU_OVERRIDE_0009359C   /* refill: r3 = reader ctx, r4 = force flag; 1 = failed */
void func_0009359C(ppu_context* ctx)
{
    uint32_t self = (uint32_t)ctx->gpr[3];
    func_0009359C__lifted(ctx);
    if ((uint32_t)ctx->gpr[3] != 0 && kz3_lump_trace_on() && kz3_stg_budget() && kz3_stg_ok(self)) {
        uint32_t hi = kz3_stg_rd(self + 0xC8), lo = kz3_stg_rd(self + 0xCC);
        fprintf(stderr, "[lump-stage] refill FAILED ctx=0x%08X offset=0x%08X%08X buf=%u d0=0x%08X d4=0x%08X\n", self, hi, lo,
                kz3_stg_rd(self + 0xC0), kz3_stg_rd(self + 0xD0), kz3_stg_rd(self + 0xD4));
        kz3_stg_stream("refill", kz3_stg_rd(self + 0x14), 1);
    }
}
#endif
#ifdef PPU_OVERRIDE_000268F4   /* GetLength: r3 = stream; returns the size in r3 */
void func_000268F4(ppu_context* ctx)
{
    uint32_t stream = (uint32_t)ctx->gpr[3];
    func_000268F4__lifted(ctx);
    static int s_n = 0;
    if (kz3_lump_trace_on() && s_n++ < 300)
        fprintf(stderr, "[lump-stage] GetLength stream=0x%08X -> %llu%s\n", stream, (unsigned long long)ctx->gpr[3],
                ctx->gpr[3] ? "" : "  (ZERO)");
}
#endif
#ifdef PPU_OVERRIDE_00029BD8   /* Read: r3 = stream, r4.. = destination/length/offset (unknown order, all printed); 0 = failed */
void func_00029BD8(ppu_context* ctx)
{
    uint32_t stream = (uint32_t)ctx->gpr[3];
    uint64_t a4 = ctx->gpr[4], a5 = ctx->gpr[5], a6 = ctx->gpr[6], a7 = ctx->gpr[7];
    func_00029BD8__lifted(ctx);
    if ((uint32_t)ctx->gpr[3] == 0 && kz3_lump_trace_on() && kz3_stg_budget()) {
        fprintf(stderr, "[lump-stage] Read FAILED r4=0x%llX r5=0x%llX r6=0x%llX r7=0x%llX\n", (unsigned long long)a4,
                (unsigned long long)a5, (unsigned long long)a6, (unsigned long long)a7);
        kz3_stg_stream("read", stream, 0);
    }
}
#endif
#endif

/* Pass 33: gameplay-test glue (port/spu/kz3_gameplay.h; the gameplay test plan B4, B7, B8). Post-hooks: B7 on the
 * PlayerProfile ctor (KZ3_SP_UNLOCK=1 sets +0x11F18 once per boot, so every reader of the flag, the Select Mission
 * chain 0x4D25EC, its siblings and Lua IsSPAllUnlocked, sees the same state; hooking one reader would cover one
 * consumer and run every frame), B4 on SetViewAngles and UpdateMoveInput (KZ3_GAMEPLAY_TRACE=1, one line per second).
 * 1.14 (switch-114): KZ3_HOOK_SP_UNLOCK(004CF158), KZ3_HOOK_TRACE_VIEW(005B7760), KZ3_HOOK_TRACE_MOVE(0069B330), and
 * the B8 Motion Sensor hooks that only exist there: KZ3_HOOK_SIXAXIS_DEFAULT(00964880),
 * KZ3_HOOK_SIXAXIS_PADTYPE(009652DC), KZ3_HOOK_SIXAXIS_LOAD(009703A0) (rows for ppu_overrides.txt on that branch;
 * on main these PPU_OVERRIDE_ symbols are never defined, so the B8 glue compiles to nothing). */
#include "kz3_gameplay.h"   /* declarations and macros only; also the pass 43 test aids below */
#ifdef PPU_OVERRIDE_004C2F08
KZ3_HOOK_SP_UNLOCK(004C2F08)
#endif
#if defined(PPU_OVERRIDE_005A3350) || defined(PPU_OVERRIDE_005B7760)
/* vf136 for a non-Soldier entity (vehicle, turret): a guest call on the hook's context after the body returned
 * (r3-r12 are volatile; the callee's own frame goes below the hook's r1). Returns r3 (the matrix EA). */
extern "C" uint32_t kz3_trace_vcall_ctx(uint32_t entity, uint32_t opd, void* user)
{
    ppu_context* ctx = (ppu_context*)user;
    ctx->gpr[3] = entity;
    ctx->ctr = gr32(opd);
    ctx->gpr[2] = gr32(opd + 4);
    ctx->lr = 0;
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f(ctx); }
    return (uint32_t)ctx->gpr[3];
}
#endif
#ifdef PPU_OVERRIDE_005A3350
KZ3_HOOK_TRACE_VIEW(005A3350)
#endif
#ifdef PPU_OVERRIDE_005B7760   /* 1.14 twin of SetViewAngles (identical body, pass 95) */
KZ3_HOOK_TRACE_VIEW(005B7760)
#endif
#ifdef PPU_OVERRIDE_004CF158   /* 1.14 PlayerProfile ctor; +0x11F18 hook site unchanged (pass 95) */
KZ3_HOOK_SP_UNLOCK(004CF158)
#endif
#ifdef PPU_OVERRIDE_0069B330   /* 1.14 UpdateMoveInput; struct offsets shifted in the body, the hook reads this+0xF8/+0xFC: ASSUMPTION unchanged */
KZ3_HOOK_TRACE_MOVE(0069B330)
#endif
#ifdef PPU_OVERRIDE_006861B0
KZ3_HOOK_TRACE_MOVE(006861B0)
#endif
#ifdef PPU_OVERRIDE_00964880        /* B8, 1.14 only */
KZ3_HOOK_SIXAXIS_DEFAULT(00964880)
#endif
#ifdef PPU_OVERRIDE_009652DC
KZ3_HOOK_SIXAXIS_PADTYPE(009652DC)
#endif
#ifdef PPU_OVERRIDE_009703A0
KZ3_HOOK_SIXAXIS_LOAD(009703A0)
#endif

/* Pass 43: test aids (kz3_gameplay.h top comment; the RE notes "Test aids"), all off by default:
 * KZ3_GOD_MODE=1 on IsGodMode, KZ3_INFINITE_AMMO=1|2 on the Humanoid owner ammo test and the ejector's shot,
 * KZ3_DEBUG_MENU=1 on the String-from-C-string ctor at the cheat_enabled call only. Player list pointer and health
 * offset per version (1.00 0x013A6D7C / +0x580, 1.14 0x0141C030 / +0x570; the +0x58x word is the death timer, the
 * health read now goes through Destructibility *(ent+0x104)+0x24 in kz3_gameplay.c, RE pass 107). */
#ifdef PPU_OVERRIDE_005A7158
KZ3_HOOK_GOD_MODE(005A7158, 0x013A6D7Cu, 0x580u)
#endif
#ifdef PPU_OVERRIDE_005BB618
KZ3_HOOK_GOD_MODE(005BB618, 0x0141C030u, 0x570u)
#endif
#ifdef PPU_OVERRIDE_0061A480
KZ3_HOOK_INF_AMMO_OWNER(0061A480, 0x013A6D7Cu, 0x580u)
#endif
#ifdef PPU_OVERRIDE_0062F180
KZ3_HOOK_INF_AMMO_OWNER(0062F180, 0x0141C030u, 0x570u)
#endif
#ifdef PPU_OVERRIDE_0073D4B8
KZ3_HOOK_INF_MAG(0073D4B8, 0x013A6D7Cu)
#endif
#ifdef PPU_OVERRIDE_00752F38
KZ3_HOOK_INF_MAG(00752F38, 0x0141C030u)
#endif
#ifdef PPU_OVERRIDE_00016700
KZ3_HOOK_DEBUG_MENU(00016700, 0x0089A164u, 0x00E6163Cu)
#endif
#ifdef PPU_OVERRIDE_00016618
KZ3_HOOK_DEBUG_MENU(00016618, 0x008BEF00u, 0x00EDD3A4u)
#endif

/* Net pass 9: the region label of online.ini on the game's region list (port/net/kz3_net_hooks.h). Localizer
 * lookup v1.00 0x0031B9F0 / 1.14 0x00327620 (addr_map_114 L1-unique 0.99, 6/6 callees); the second argument is the
 * WString::assign helper it calls, v1.00 0x00023FF8 / 1.14 0x000244C4 (not a twin of the lookup). Needs the net
 * sources linked (KZ3_WITH_NET, port/spu/CMakeLists.txt passes it to this file). */
#if defined(KZ3_WITH_NET) && (defined(PPU_OVERRIDE_0031B9F0) || defined(PPU_OVERRIDE_00327620))
#include "kz3_net_hooks.h"
#ifdef PPU_OVERRIDE_0031B9F0
KZ3_HOOK_REGION_LABEL(0031B9F0, 00023FF8)
#endif
#ifdef PPU_OVERRIDE_00327620
KZ3_HOOK_REGION_LABEL(00327620, 000244C4)
#endif
#endif

/* ---- SPU pass 35 probes (off unless their switch is set; the rows are in ppu_overrides.txt) ---- */
#if defined(PPU_OVERRIDE_00B37FBC) || defined(PPU_OVERRIDE_00B42FCC)   /* per-view result descriptor rebuild (RE pass 73 s.11.2): KZ3_CULL_PROBE=1 */
extern "C" void kz3_cull_probe_desc(uint32_t desc_ea);
#endif
#ifdef PPU_OVERRIDE_00B37FBC
void func_00B37FBC(ppu_context* ctx) { kz3_cull_probe_desc((uint32_t)ctx->gpr[3]); func_00B37FBC__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_00B42FCC   /* 1.14 twin, 116/116 identical (pass 95) */
void func_00B42FCC(ppu_context* ctx) { kz3_cull_probe_desc((uint32_t)ctx->gpr[3]); func_00B42FCC__lifted(ctx); }
#endif
#if defined(PPU_OVERRIDE_007EE150) || defined(PPU_OVERRIDE_007ED438) || defined(PPU_OVERRIDE_001BCDF8) || \
    defined(PPU_OVERRIDE_0080FA30) || defined(PPU_OVERRIDE_0080ECF8) || defined(PPU_OVERRIDE_001C8398)
extern "C" void kz3_hud_probe_call(int which, uint32_t r3, const uint32_t* globals);   /* KZ3_HUD_PROBE=1 */
/* globals: font flag byte, Game+4 word, HUD manager global, profile pointer word (the RE notes 6.2-6.3) */
static const uint32_t kz3_hud_globals_100[4] = {0x1345C80, 0x1587884, 0x136351C, 0x1587FEC};
static const uint32_t kz3_hud_globals_114[4] = {0x13BA380, 0x15FCF84, 0x13D7C18, 0x15FD6FC};
#endif
#ifdef PPU_OVERRIDE_007EE150   /* HUD manager render slot, 1.00 */
void func_007EE150(ppu_context* ctx) { kz3_hud_probe_call(0, (uint32_t)ctx->gpr[3], kz3_hud_globals_100); func_007EE150__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_007ED438   /* HUD render, 1.00 */
void func_007ED438(ppu_context* ctx) { kz3_hud_probe_call(1, (uint32_t)ctx->gpr[3], kz3_hud_globals_100); func_007ED438__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_001BCDF8   /* font system init, 1.00 */
void func_001BCDF8(ppu_context* ctx) { kz3_hud_probe_call(2, (uint32_t)ctx->gpr[3], kz3_hud_globals_100); func_001BCDF8__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_0080FA30   /* 1.14 */
void func_0080FA30(ppu_context* ctx) { kz3_hud_probe_call(0, (uint32_t)ctx->gpr[3], kz3_hud_globals_114); func_0080FA30__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_0080ECF8
void func_0080ECF8(ppu_context* ctx) { kz3_hud_probe_call(1, (uint32_t)ctx->gpr[3], kz3_hud_globals_114); func_0080ECF8__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_001C8398
void func_001C8398(ppu_context* ctx) { kz3_hud_probe_call(2, (uint32_t)ctx->gpr[3], kz3_hud_globals_114); func_001C8398__lifted(ctx); }
#endif
#if defined(PPU_OVERRIDE_0005B9F0) || defined(PPU_OVERRIDE_001D8B08) || defined(PPU_OVERRIDE_0005C2D0) || \
    defined(PPU_OVERRIDE_001E4278)
/* KZ3_ARENA_PROBE=1 (RE pass 74 s.11.5, candidate 1): frame-arena chunk takes and the view builder's chain wait. */
extern "C" void kz3_arena_probe_pre(uint32_t arena);
extern "C" void kz3_arena_probe_post(uint32_t arena, uint32_t stamp, uint32_t wait, uint32_t chunk);
extern "C" void kz3_arena_probe_builder_pre(uint32_t block);
extern "C" void kz3_arena_probe_builder_post(uint32_t block);
extern "C" void kz3_arena_probe_set_g(uint32_t g);   /* the renderer singleton pointer's address (1.00 0x133163C default) */
#ifdef PPU_OVERRIDE_0005C2D0
/* 1.14: G = 0x013A56BC (exact data pair of 0x133163C, 438 sites; the RE notes s.35). ASSUMPTION: G+0x10 / +0x50 /
 * +0x5A8 (frame arena, its label) keep their 1.00 offsets: the arena take 0x5C2D0 is identical, the Renderer struct is unchecked. */
static const int kz3_arena_g_set = (kz3_arena_probe_set_g(0x013A56BCu), 0);
#endif
#endif
#ifdef PPU_OVERRIDE_0005B9F0   /* arena chunk take (arena, stamp, wait) -> chunk EA, 1.00 */
void func_0005B9F0(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3], s = (uint32_t)ctx->gpr[4], w = (uint32_t)ctx->gpr[5];
    kz3_arena_probe_pre(a); func_0005B9F0__lifted(ctx); kz3_arena_probe_post(a, s, w, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_0005C2D0   /* 1.14 */
void func_0005C2D0(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3], s = (uint32_t)ctx->gpr[4], w = (uint32_t)ctx->gpr[5];
    kz3_arena_probe_pre(a); func_0005C2D0__lifted(ctx); kz3_arena_probe_post(a, s, w, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_001D8B08   /* view job builder (block, ...): spins on block+0x8A0 first, 1.00 */
void func_001D8B08(ppu_context* ctx)
{
    uint32_t b = (uint32_t)ctx->gpr[3];
    kz3_arena_probe_builder_pre(b); func_001D8B08__lifted(ctx); kz3_arena_probe_builder_post(b);
}
#endif
#ifdef PPU_OVERRIDE_001E4278   /* 1.14 twin, 187/187 identical (pass 95) */
void func_001E4278(ppu_context* ctx)
{
    uint32_t b = (uint32_t)ctx->gpr[3];
    kz3_arena_probe_builder_pre(b); func_001E4278__lifted(ctx); kz3_arena_probe_builder_post(b);
}
#endif
#if defined(PPU_OVERRIDE_00D42C7C) || defined(PPU_OVERRIDE_00D92638) || defined(PPU_OVERRIDE_00D44810) || \
    defined(PPU_OVERRIDE_00D941D0)
/* KZ3_AUDIO_TICK=1 (SPU pass 37, the RE notes 1-4): the brbDriver tick and the mix step, timed by
 * kz3_audio_probe.c. On the first brbDriver call the lv2 handlers of sc 0x82 (event_queue_receive, the tick wait) and
 * 0x8d (usleep, the mix step's 1 ms job-list poll) are wrapped; the wrappers only act on the brbDriver thread.
 * At the receive sc (inline at 0xD42D2C) r31 = the driver object and r26 = the write cursor `last`. */
extern "C" int  kz3_audio_tick_on(void);
extern "C" void kz3_audio_tick_pre(uint32_t last);
extern "C" void kz3_audio_tick_post(uint32_t obj, uint32_t last);
extern "C" void kz3_audio_unit_enter(void);
extern "C" void kz3_audio_unit_exit(unsigned sleeps);
typedef int64_t (*kz3_sc_fn)(ppu_context*);
extern "C" struct kz3_lv2_tbl { kz3_sc_fn h[1024]; } g_lv2_syscalls;   /* lv2_register.c (lv2_syscall_table) */
static kz3_sc_fn s_kz3_recv_orig, s_kz3_usleep_orig;
static thread_local int t_kz3_brb;
static thread_local unsigned t_kz3_unit_sleeps;
static int64_t kz3_recv_wrap(ppu_context* ctx)
{
    if (!t_kz3_brb) return s_kz3_recv_orig(ctx);
    kz3_audio_tick_pre((uint32_t)ctx->gpr[26]);
    const int64_t r = s_kz3_recv_orig(ctx);
    kz3_audio_tick_post((uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[26]);
    return r;
}
static int64_t kz3_usleep_wrap(ppu_context* ctx)
{
    t_kz3_unit_sleeps++;
    return s_kz3_usleep_orig(ctx);
}
static void kz3_audio_brb_enter(void)
{
    if (!kz3_audio_tick_on()) return;
    t_kz3_brb = 1;
    static int installed;
    if (!installed) {
        installed = 1;
        s_kz3_recv_orig = g_lv2_syscalls.h[0x82]; g_lv2_syscalls.h[0x82] = kz3_recv_wrap;
        s_kz3_usleep_orig = g_lv2_syscalls.h[0x8d]; g_lv2_syscalls.h[0x8d] = kz3_usleep_wrap;
        fprintf(stderr, "[audio-tick] probe on: sc 0x82 / 0x8d wrapped on the brbDriver thread\n");
    }
}
static void kz3_audio_unit(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (!t_kz3_brb) { lifted(ctx); return; }
    t_kz3_unit_sleeps = 0;
    kz3_audio_unit_enter();
    lifted(ctx);
    kz3_audio_unit_exit(t_kz3_unit_sleeps);
}
#endif
#ifdef PPU_OVERRIDE_00D42C7C   /* brbDriver thread entry (r3 = driver object), 1.00 */
void func_00D42C7C(ppu_context* ctx) { kz3_audio_brb_enter(); func_00D42C7C__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_00D92638   /* 1.14 */
void func_00D92638(ppu_context* ctx) { kz3_audio_brb_enter(); func_00D92638__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_00D44810   /* mix step (per unit), 1.00 */
void func_00D44810(ppu_context* ctx) { kz3_audio_unit(ctx, func_00D44810__lifted); }
#endif
#ifdef PPU_OVERRIDE_00D941D0   /* 1.14 */
void func_00D941D0(ppu_context* ctx) { kz3_audio_unit(ctx, func_00D941D0__lifted); }
#endif
/* SPU pass 71 (the RE notes s.5.4): KZ3_AUDIO_VOICE=1, the mix pass's job append 0xD44AB8(owner, node,
 * items, taken, last) per batch (kz3_mixjob_probe.c): jobs and voices per unit, node ages, wire floats. */
#if defined(PPU_OVERRIDE_00D44AB8) || defined(PPU_OVERRIDE_00D94A2C)
extern "C" int kz3_mixjob_probe_on(void);
extern "C" void kz3_mixjob_probe_batch(uint32_t node, uint32_t items, uint32_t taken, uint32_t last);
static void kz3_mixjob_w(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (kz3_mixjob_probe_on())
        kz3_mixjob_probe_batch((uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);
    lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_00D44AB8   /* 1.00 */
void func_00D44AB8(ppu_context* ctx) { kz3_mixjob_w(ctx, func_00D44AB8__lifted); }
#endif
#ifdef PPU_OVERRIDE_00D94A2C   /* 1.14 */
void func_00D94A2C(ppu_context* ctx) { kz3_mixjob_w(ctx, func_00D94A2C__lifted); }
#endif
/* SPU pass 39 (the RE notes 5.3): KZ3_AUDIO_SPU=1, the wid 9 list kick and poll on the brbDriver thread
 * (kz3_audio_probe.c). The poll's workload-info buffer sits at the callee's frame (-0x180) + 0x70 on both versions. */
#if (defined(PPU_OVERRIDE_00A78818) || defined(PPU_OVERRIDE_00A8F728) || defined(PPU_OVERRIDE_00A78E54) || \
     defined(PPU_OVERRIDE_00A8FD64)) && (defined(PPU_OVERRIDE_00D42C7C) || defined(PPU_OVERRIDE_00D92638) || \
     defined(PPU_OVERRIDE_00D44810) || defined(PPU_OVERRIDE_00D941D0))
extern "C" void kz3_audio_kick(uint32_t handle);
extern "C" void kz3_audio_poll_pre(uint32_t info);
extern "C" void kz3_audio_poll_post(uint32_t handle, uint32_t t, uint32_t info, uint32_t ret);
static void kz3_audio_kick_w(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (t_kz3_brb) kz3_audio_kick((uint32_t)ctx->gpr[3]);
    lifted(ctx);
}
static void kz3_audio_poll_w(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (!t_kz3_brb) { lifted(ctx); return; }
    const uint32_t h = (uint32_t)ctx->gpr[3], t = (uint32_t)ctx->gpr[4], info = (uint32_t)ctx->gpr[1] - 0x180u + 0x70u;
    kz3_audio_poll_pre(info);
    lifted(ctx);
    kz3_audio_poll_post(h, t, info, (uint32_t)ctx->gpr[3]);
}
#ifdef PPU_OVERRIDE_00A78818   /* kick = cellSpursReadyCountStore(handle+4, handle+8, 1), 1.00 */
void func_00A78818(ppu_context* ctx) { kz3_audio_kick_w(ctx, func_00A78818__lifted); }
#endif
#ifdef PPU_OVERRIDE_00A8F728   /* 1.14 */
void func_00A8F728(ppu_context* ctx) { kz3_audio_kick_w(ctx, func_00A8F728__lifted); }
#endif
#ifdef PPU_OVERRIDE_00A78E54   /* list poll (handle, t) -> 1 when list t is done, 1.00 */
void func_00A78E54(ppu_context* ctx) { kz3_audio_poll_w(ctx, func_00A78E54__lifted); }
#endif
#ifdef PPU_OVERRIDE_00A8FD64   /* 1.14 */
void func_00A8FD64(ppu_context* ctx) { kz3_audio_poll_w(ctx, func_00A8FD64__lifted); }
#endif
#endif
/* KZ3_HUD_PROBE=1, pass 37: the HUD view-creation chain of the RE notes 7.8 (v1.00 rows; manager global 0x136351C). */
#if defined(PPU_OVERRIDE_00325AD4) || defined(PPU_OVERRIDE_007BBB70) || defined(PPU_OVERRIDE_007AF444) || \
    defined(PPU_OVERRIDE_0036C01C) || defined(PPU_OVERRIDE_007F2970) || defined(PPU_OVERRIDE_007F1A10) || \
    defined(PPU_OVERRIDE_007F0470) || defined(PPU_OVERRIDE_007F0058) || defined(PPU_OVERRIDE_00331D14) || \
    defined(PPU_OVERRIDE_007D2A30) || defined(PPU_OVERRIDE_007C558C) || defined(PPU_OVERRIDE_0037812C) || \
    defined(PPU_OVERRIDE_00814378) || defined(PPU_OVERRIDE_008133C8) || defined(PPU_OVERRIDE_00811D6C) || \
    defined(PPU_OVERRIDE_00811938) || defined(PPU_OVERRIDE_007EDF6C) || defined(PPU_OVERRIDE_0080F84C) || \
    defined(PPU_OVERRIDE_0038CA7C) || defined(PPU_OVERRIDE_00398BAC) || defined(PPU_OVERRIDE_007E5570) || \
    defined(PPU_OVERRIDE_00805B30) || defined(PPU_OVERRIDE_001BDF84) || defined(PPU_OVERRIDE_001BDCB0) || \
    defined(PPU_OVERRIDE_007ECB18) || defined(PPU_OVERRIDE_001C9524) || defined(PPU_OVERRIDE_001C9250) || \
    defined(PPU_OVERRIDE_0080E3D4)
extern "C" void kz3_hud_probe_ev(int ev, uint32_t r3, uint32_t r4, uint32_t hud_global);
extern "C" void kz3_hud_probe_view(int ev, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t hud_global, uint32_t voff);
/* 1.14 twins (the RE notes s.35): manager global 0x13D7C18, view count/array at +0xD8/+0xDC */
#define KZ3_HUD_EV114(addr, ev) \
    void func_##addr(ppu_context* ctx) { kz3_hud_probe_ev(ev, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], 0x13D7C18u); func_##addr##__lifted(ctx); }
/* pass 39 view-gate rows: entry event, or entry + exit event */
#define KZ3_HUD_VIEW(addr, ev, glob, voff) \
    void func_##addr(ppu_context* ctx) { kz3_hud_probe_view(ev, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], glob, voff); func_##addr##__lifted(ctx); }
#define KZ3_HUD_VIEW2(addr, ev, glob, voff) \
    void func_##addr(ppu_context* ctx) { const uint32_t r3 = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4]; \
        kz3_hud_probe_view(ev, r3, r4, (uint32_t)ctx->gpr[5], glob, voff); func_##addr##__lifted(ctx); kz3_hud_probe_view(ev + 1, r3, r4, 0, glob, voff); }
#define KZ3_HUD_EV(addr, ev) \
    void func_##addr(ppu_context* ctx) { kz3_hud_probe_ev(ev, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], 0x136351Cu); func_##addr##__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_00325AD4
KZ3_HUD_EV(00325AD4, 3)
#endif
#ifdef PPU_OVERRIDE_007BBB70
KZ3_HUD_EV(007BBB70, 4)
#endif
#ifdef PPU_OVERRIDE_007AF444
KZ3_HUD_EV(007AF444, 5)
#endif
#ifdef PPU_OVERRIDE_0036C01C   /* logged at exit: r3 = the return value */
void func_0036C01C(ppu_context* ctx) { func_0036C01C__lifted(ctx); kz3_hud_probe_ev(6, (uint32_t)ctx->gpr[3], 0, 0x136351Cu); }
#endif
#ifdef PPU_OVERRIDE_007F2970
KZ3_HUD_EV(007F2970, 7)
#endif
#ifdef PPU_OVERRIDE_007F1A10
KZ3_HUD_EV(007F1A10, 8)
#endif
#ifdef PPU_OVERRIDE_007F0470
KZ3_HUD_EV(007F0470, 9)
#endif
#ifdef PPU_OVERRIDE_007F0058   /* pass 39: also its exit (ev 14: HUD+4, the first view's entry count) */
void func_007F0058(ppu_context* ctx)
{
    kz3_hud_probe_ev(10, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], 0x136351Cu);
    func_007F0058__lifted(ctx);
    kz3_hud_probe_view(14, 0, 0, 0, 0x136351Cu, 0xC8);
}
#endif
#ifdef PPU_OVERRIDE_007EDF6C   /* HUD view draw (drawer, vector of views, base) */
KZ3_HUD_VIEW2(007EDF6C, 11, 0x136351Cu, 0xC8)
#endif
#ifdef PPU_OVERRIDE_0038CA7C   /* view viewport setter (view, &viewport, i) */
KZ3_HUD_VIEW(0038CA7C, 13, 0x136351Cu, 0xC8)
#endif
#ifdef PPU_OVERRIDE_007E5570   /* per-entry view draw (view, ...) */
KZ3_HUD_VIEW(007E5570, 15, 0x136351Cu, 0xC8)
#endif
/* 1.14 twins (rows only in the 1.14 override file; the RE notes s.35) */
#ifdef PPU_OVERRIDE_00331D14
KZ3_HUD_EV114(00331D14, 3)
#endif
#ifdef PPU_OVERRIDE_007D2A30
KZ3_HUD_EV114(007D2A30, 4)
#endif
#ifdef PPU_OVERRIDE_007C558C
KZ3_HUD_EV114(007C558C, 5)
#endif
#ifdef PPU_OVERRIDE_0037812C
void func_0037812C(ppu_context* ctx) { func_0037812C__lifted(ctx); kz3_hud_probe_ev(6, (uint32_t)ctx->gpr[3], 0, 0x13D7C18u); }
#endif
#ifdef PPU_OVERRIDE_00814378
KZ3_HUD_EV114(00814378, 7)
#endif
#ifdef PPU_OVERRIDE_008133C8
KZ3_HUD_EV114(008133C8, 8)
#endif
#ifdef PPU_OVERRIDE_00811D6C
KZ3_HUD_EV114(00811D6C, 9)
#endif
#ifdef PPU_OVERRIDE_00811938
void func_00811938(ppu_context* ctx)
{
    kz3_hud_probe_ev(10, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], 0x13D7C18u);
    func_00811938__lifted(ctx);
    kz3_hud_probe_view(14, 0, 0, 0, 0x13D7C18u, 0xD8);
}
#endif
#ifdef PPU_OVERRIDE_0080F84C
KZ3_HUD_VIEW2(0080F84C, 11, 0x13D7C18u, 0xD8)
#endif
#ifdef PPU_OVERRIDE_00398BAC
KZ3_HUD_VIEW(00398BAC, 13, 0x13D7C18u, 0xD8)
#endif
#ifdef PPU_OVERRIDE_00805B30
KZ3_HUD_VIEW(00805B30, 15, 0x13D7C18u, 0xD8)
#endif
/* pass 40 (the RE notes 7.10): the UI renderer draw 0x1BDF84 entry (r3 = U), 0x1BDCB0 count, HUD vf3 exit */
#define KZ3_HUD_VIEWX(addr, ev, glob) \
    void func_##addr(ppu_context* ctx) { const uint32_t r3 = (uint32_t)ctx->gpr[3]; func_##addr##__lifted(ctx); kz3_hud_probe_view(ev, r3, 0, 0, glob, 0); }
#ifdef PPU_OVERRIDE_001BDF84
KZ3_HUD_VIEW(001BDF84, 16, 0x136351Cu, 0xC8)
#endif
#ifdef PPU_OVERRIDE_001BDCB0
KZ3_HUD_VIEW(001BDCB0, 17, 0x136351Cu, 0xC8)
#endif
#ifdef PPU_OVERRIDE_007ECB18
KZ3_HUD_VIEWX(007ECB18, 18, 0x136351Cu)
#endif
#ifdef PPU_OVERRIDE_001C9524
KZ3_HUD_VIEW(001C9524, 16, 0x13D7C18u, 0xD8)
#endif
#ifdef PPU_OVERRIDE_001C9250
KZ3_HUD_VIEW(001C9250, 17, 0x13D7C18u, 0xD8)
#endif
#ifdef PPU_OVERRIDE_0080E3D4
KZ3_HUD_VIEWX(0080E3D4, 18, 0x13D7C18u)
#endif
/* KZ3_HUD_PROBE=1, pass 42: the HUD element path and the UI renderer's view pass (kz3_hud_probe.c kz3_hud_elem;
 * v1.00 rows, U pointer 0x14DDCD4; 1.14 twins not yet known: see the spu report's Requests). */
#if defined(PPU_OVERRIDE_007FAEC4) || defined(PPU_OVERRIDE_007FB9A0) || defined(PPU_OVERRIDE_007FBB44) || \
    defined(PPU_OVERRIDE_008119B8) || defined(PPU_OVERRIDE_00810280) || defined(PPU_OVERRIDE_007FD908) || \
    defined(PPU_OVERRIDE_007FC9A4) || defined(PPU_OVERRIDE_007FCE48) || defined(PPU_OVERRIDE_007FC328) || \
    defined(PPU_OVERRIDE_001B9884) || defined(PPU_OVERRIDE_001FF878) || defined(PPU_OVERRIDE_001FB4A4) || \
    defined(PPU_OVERRIDE_001F3350) || defined(PPU_OVERRIDE_00200204) || defined(PPU_OVERRIDE_007FB164) || \
    defined(PPU_OVERRIDE_007FB248) || defined(PPU_OVERRIDE_007FBCE4) || defined(PPU_OVERRIDE_007FBDB0) || \
    defined(PPU_OVERRIDE_00810E60) || defined(PPU_OVERRIDE_00811118) || defined(PPU_OVERRIDE_008124B0) || \
    defined(PPU_OVERRIDE_008112CC) || defined(PPU_OVERRIDE_0081BB34) || defined(PPU_OVERRIDE_0084094C) || \
    defined(PPU_OVERRIDE_00616250) || defined(PPU_OVERRIDE_0065D658) || defined(PPU_OVERRIDE_00616880)
extern "C" void kz3_hud_elem(int idx, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t u_ptr, uint32_t lr);
#define KZ3_HUD_ELEM(addr, idx) \
    void func_##addr(ppu_context* ctx) { kz3_hud_elem(idx, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], 0x14DDCD4u, (uint32_t)ctx->lr); func_##addr##__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_007FB164
KZ3_HUD_ELEM(007FB164, 14)
#endif
#ifdef PPU_OVERRIDE_007FB248
extern "C" void kz3_hud_vf3(int exit, uint32_t self, uint32_t mode);
void func_007FB248(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], mode = (uint32_t)ctx->gpr[5];
    kz3_hud_elem(15, self, (uint32_t)ctx->gpr[4], mode, 0x14DDCD4u, (uint32_t)ctx->lr);
    kz3_hud_vf3(0, self, mode);
    func_007FB248__lifted(ctx);
    kz3_hud_vf3(1, self, mode);
}
#endif
#ifdef PPU_OVERRIDE_007F49D0
extern "C" void kz3_hud_container(uint32_t c, uint32_t prof_ptr);
void func_007F49D0(ppu_context* ctx) { kz3_hud_container((uint32_t)ctx->gpr[3], 0x1587FECu); func_007F49D0__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_0081BB34
KZ3_HUD_ELEM(0081BB34, 22)
#endif
#ifdef PPU_OVERRIDE_0084094C
KZ3_HUD_ELEM(0084094C, 23)
#endif
#ifdef PPU_OVERRIDE_00616250
KZ3_HUD_ELEM(00616250, 24)
#endif
#ifdef PPU_OVERRIDE_0065D658
KZ3_HUD_ELEM(0065D658, 25)
#endif
#ifdef PPU_OVERRIDE_00616880
KZ3_HUD_ELEM(00616880, 26)
#endif
#ifdef PPU_OVERRIDE_007FBCE4
KZ3_HUD_ELEM(007FBCE4, 16)
#endif
#ifdef PPU_OVERRIDE_007FBDB0
KZ3_HUD_ELEM(007FBDB0, 17)
#endif
#ifdef PPU_OVERRIDE_00810E60
KZ3_HUD_ELEM(00810E60, 18)
#endif
#ifdef PPU_OVERRIDE_00811118
extern "C" void kz3_hud_xhair_vis(uint32_t self, uint32_t ret);
void func_00811118(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    kz3_hud_elem(19, self, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], 0x14DDCD4u, (uint32_t)ctx->lr);
    func_00811118__lifted(ctx);
    kz3_hud_xhair_vis(self, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_008124B0
KZ3_HUD_ELEM(008124B0, 20)
#endif
#ifdef PPU_OVERRIDE_008112CC
KZ3_HUD_ELEM(008112CC, 21)
#endif
#ifdef PPU_OVERRIDE_007FAEC4
KZ3_HUD_ELEM(007FAEC4, 0)
#endif
#ifdef PPU_OVERRIDE_007FB9A0
KZ3_HUD_ELEM(007FB9A0, 1)
#endif
#ifdef PPU_OVERRIDE_007FBB44
KZ3_HUD_ELEM(007FBB44, 2)
#endif
#ifdef PPU_OVERRIDE_008119B8
KZ3_HUD_ELEM(008119B8, 3)
#endif
#ifdef PPU_OVERRIDE_00810280
KZ3_HUD_ELEM(00810280, 4)
#endif
#ifdef PPU_OVERRIDE_007FD908
KZ3_HUD_ELEM(007FD908, 5)
#endif
#ifdef PPU_OVERRIDE_007FC9A4
KZ3_HUD_ELEM(007FC9A4, 6)
#endif
#ifdef PPU_OVERRIDE_007FCE48
KZ3_HUD_ELEM(007FCE48, 7)
#endif
#ifdef PPU_OVERRIDE_007FC328
KZ3_HUD_ELEM(007FC328, 8)
#endif
#ifdef PPU_OVERRIDE_001B9884
KZ3_HUD_ELEM(001B9884, 9)
#endif
#ifdef PPU_OVERRIDE_001FF878
KZ3_HUD_ELEM(001FF878, 10)
#endif
#ifdef PPU_OVERRIDE_001FB4A4
KZ3_HUD_ELEM(001FB4A4, 11)
#endif
#ifdef PPU_OVERRIDE_001F3350
KZ3_HUD_ELEM(001F3350, 12)
#endif
#ifdef PPU_OVERRIDE_00200204
KZ3_HUD_ELEM(00200204, 13)
#endif
/* s.36.4 step 2 for the pass 42 HUD element rows: the 1.14 twins (re/data/overrides tables, [114=...] tags) with
 * U = 0x1553354 (twin of 0x14DDCD4, g_he_ptr comment) and the profile pointer 0x15FD6FC (twin of 0x1587FEC,
 * the RE notes HUD probe globals). Diagnostic only; not run on 1.14 (no 1.14 build yet). */
#if defined(PPU_OVERRIDE_0081D14C) || defined(PPU_OVERRIDE_0081DC28) || defined(PPU_OVERRIDE_0081DDCC) || defined(PPU_OVERRIDE_00834220) || defined(PPU_OVERRIDE_00832ABC) || defined(PPU_OVERRIDE_0081FBEC) || defined(PPU_OVERRIDE_0081EC7C) || defined(PPU_OVERRIDE_0081F124) || defined(PPU_OVERRIDE_0081E5D8) || defined(PPU_OVERRIDE_001C4E24) || defined(PPU_OVERRIDE_0020B1C8) || defined(PPU_OVERRIDE_00206DEC) || defined(PPU_OVERRIDE_001FEAD0) || defined(PPU_OVERRIDE_0020BB54) || defined(PPU_OVERRIDE_0081D3EC) || defined(PPU_OVERRIDE_0081D4D0) || defined(PPU_OVERRIDE_0081DF6C) || defined(PPU_OVERRIDE_0081E038) || defined(PPU_OVERRIDE_008336A0) || defined(PPU_OVERRIDE_00833958) || defined(PPU_OVERRIDE_00834D34) || defined(PPU_OVERRIDE_00833B0C) || defined(PPU_OVERRIDE_008163D8) || defined(PPU_OVERRIDE_0083E368) || defined(PPU_OVERRIDE_008633DC) || defined(PPU_OVERRIDE_0062A990) || defined(PPU_OVERRIDE_00672238) || defined(PPU_OVERRIDE_0062AFB8)
extern "C" void kz3_hud_elem(int idx, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t u_ptr, uint32_t lr);
#define KZ3_HUD_ELEM_114(addr, idx) \
    void func_##addr(ppu_context* ctx) { kz3_hud_elem(idx, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], 0x1553354u, (uint32_t)ctx->lr); func_##addr##__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_0081D14C
KZ3_HUD_ELEM_114(0081D14C, 0)   /* 1.00 007FAEC4 */
#endif
#ifdef PPU_OVERRIDE_0081DC28
KZ3_HUD_ELEM_114(0081DC28, 1)   /* 1.00 007FB9A0 */
#endif
#ifdef PPU_OVERRIDE_0081DDCC
KZ3_HUD_ELEM_114(0081DDCC, 2)   /* 1.00 007FBB44 */
#endif
#ifdef PPU_OVERRIDE_00834220
KZ3_HUD_ELEM_114(00834220, 3)   /* 1.00 008119B8 */
#endif
#ifdef PPU_OVERRIDE_00832ABC
KZ3_HUD_ELEM_114(00832ABC, 4)   /* 1.00 00810280 */
#endif
#ifdef PPU_OVERRIDE_0081FBEC
KZ3_HUD_ELEM_114(0081FBEC, 5)   /* 1.00 007FD908 */
#endif
#ifdef PPU_OVERRIDE_0081EC7C
KZ3_HUD_ELEM_114(0081EC7C, 6)   /* 1.00 007FC9A4 */
#endif
#ifdef PPU_OVERRIDE_0081F124
KZ3_HUD_ELEM_114(0081F124, 7)   /* 1.00 007FCE48 */
#endif
#ifdef PPU_OVERRIDE_0081E5D8
KZ3_HUD_ELEM_114(0081E5D8, 8)   /* 1.00 007FC328 */
#endif
#ifdef PPU_OVERRIDE_001C4E24
KZ3_HUD_ELEM_114(001C4E24, 9)   /* 1.00 001B9884 */
#endif
#ifdef PPU_OVERRIDE_0020B1C8
KZ3_HUD_ELEM_114(0020B1C8, 10)   /* 1.00 001FF878 */
#endif
#ifdef PPU_OVERRIDE_00206DEC
KZ3_HUD_ELEM_114(00206DEC, 11)   /* 1.00 001FB4A4 */
#endif
#ifdef PPU_OVERRIDE_001FEAD0
KZ3_HUD_ELEM_114(001FEAD0, 12)   /* 1.00 001F3350 */
#endif
#ifdef PPU_OVERRIDE_0020BB54
KZ3_HUD_ELEM_114(0020BB54, 13)   /* 1.00 00200204 */
#endif
#ifdef PPU_OVERRIDE_0081D3EC
KZ3_HUD_ELEM_114(0081D3EC, 14)   /* 1.00 007FB164 */
#endif
#ifdef PPU_OVERRIDE_0081D4D0   /* 1.00 007FB248 */
extern "C" void kz3_hud_vf3(int exit, uint32_t self, uint32_t mode);
void func_0081D4D0(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], mode = (uint32_t)ctx->gpr[5];
    kz3_hud_elem(15, self, (uint32_t)ctx->gpr[4], mode, 0x1553354u, (uint32_t)ctx->lr);
    kz3_hud_vf3(0, self, mode);
    func_0081D4D0__lifted(ctx);
    kz3_hud_vf3(1, self, mode);
}
#endif
#ifdef PPU_OVERRIDE_0081DF6C
KZ3_HUD_ELEM_114(0081DF6C, 16)   /* 1.00 007FBCE4 */
#endif
#ifdef PPU_OVERRIDE_0081E038
KZ3_HUD_ELEM_114(0081E038, 17)   /* 1.00 007FBDB0 */
#endif
#ifdef PPU_OVERRIDE_008336A0
KZ3_HUD_ELEM_114(008336A0, 18)   /* 1.00 00810E60 */
#endif
#ifdef PPU_OVERRIDE_00833958   /* 1.00 00811118 */
extern "C" void kz3_hud_xhair_vis(uint32_t self, uint32_t ret);
void func_00833958(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    kz3_hud_elem(19, self, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], 0x1553354u, (uint32_t)ctx->lr);
    func_00833958__lifted(ctx);
    kz3_hud_xhair_vis(self, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_00834D34
KZ3_HUD_ELEM_114(00834D34, 20)   /* 1.00 008124B0 */
#endif
#ifdef PPU_OVERRIDE_00833B0C
KZ3_HUD_ELEM_114(00833B0C, 21)   /* 1.00 008112CC */
#endif
#ifdef PPU_OVERRIDE_008163D8   /* 1.00 007F49D0 */
extern "C" void kz3_hud_container(uint32_t c, uint32_t prof_ptr);
void func_008163D8(ppu_context* ctx) { kz3_hud_container((uint32_t)ctx->gpr[3], 0x15FD6FCu); func_008163D8__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_0083E368
KZ3_HUD_ELEM_114(0083E368, 22)   /* 1.00 0081BB34 */
#endif
#ifdef PPU_OVERRIDE_008633DC
KZ3_HUD_ELEM_114(008633DC, 23)   /* 1.00 0084094C */
#endif
#ifdef PPU_OVERRIDE_0062A990
KZ3_HUD_ELEM_114(0062A990, 24)   /* 1.00 00616250 */
#endif
#ifdef PPU_OVERRIDE_00672238
KZ3_HUD_ELEM_114(00672238, 25)   /* 1.00 0065D658 */
#endif
#ifdef PPU_OVERRIDE_0062AFB8
KZ3_HUD_ELEM_114(0062AFB8, 26)   /* 1.00 00616880 */
#endif
/* KZ3_HUD_PROBE=1, pass 41 (the RE notes 7.13): the session state machine 4 -> 5 and the movie state.
 * Globals {movie state, Game pending-mode pointer, Game flag byte}: v1.00 {0x13BC8D0, 0x1587FE4, 0x15880D5},
 * 1.14 {0x1432A40, 0x15FD6F4, 0x15FD7E9 (ASSUMPTION: lbz 0x869 off Game 0x15FCF80 in 0x7D1C7C)}. */
#if defined(PPU_OVERRIDE_007BB404) || defined(PPU_OVERRIDE_007BAF7C) || defined(PPU_OVERRIDE_008C5080) || \
    defined(PPU_OVERRIDE_007D2110) || defined(PPU_OVERRIDE_007D1C7C) || defined(PPU_OVERRIDE_008F1128)
extern "C" void kz3_hud_session(int ev, uint32_t s, uint32_t r4, uint32_t r5, const uint32_t* g);
static const uint32_t kz3_sess_g100[3] = {0x13BC8D0u, 0x1587FE4u, 0x15880D5u};
static const uint32_t kz3_sess_g114[3] = {0x1432A40u, 0x15FD6F4u, 0x15FD7E9u};
#define KZ3_SESS_UPD(addr, g) \
    void func_##addr(ppu_context* ctx) { const uint32_t s = (uint32_t)ctx->gpr[3]; kz3_hud_session(0, s, 0, 0, g); \
        func_##addr##__lifted(ctx); kz3_hud_session(1, s, 0, 0, g); }
#define KZ3_SESS_TRY(addr, g) \
    void func_##addr(ppu_context* ctx) { const uint32_t s = (uint32_t)ctx->gpr[3]; func_##addr##__lifted(ctx); \
        kz3_hud_session(2, s, 0, 0, g); }
#define KZ3_SESS_MOV(addr, g) \
    void func_##addr(ppu_context* ctx) { kz3_hud_session(3, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], \
        (uint32_t)ctx->gpr[5], g); func_##addr##__lifted(ctx); }
#endif
#ifdef PPU_OVERRIDE_007BB404
KZ3_SESS_UPD(007BB404, kz3_sess_g100)
#endif
#ifdef PPU_OVERRIDE_007BAF7C
KZ3_SESS_TRY(007BAF7C, kz3_sess_g100)
#endif
#ifdef PPU_OVERRIDE_008C5080
KZ3_SESS_MOV(008C5080, kz3_sess_g100)
#endif
#ifdef PPU_OVERRIDE_007D2110
KZ3_SESS_UPD(007D2110, kz3_sess_g114)
#endif
#ifdef PPU_OVERRIDE_007D1C7C
KZ3_SESS_TRY(007D1C7C, kz3_sess_g114)
#endif
#ifdef PPU_OVERRIDE_008F1128
KZ3_SESS_MOV(008F1128, kz3_sess_g114)
#endif
/* KZ3_CULL_PLANES=1, pass 41 (the RE notes s.13.3 candidate 2): 0x1D7E58 (1.14 0x1E35C8) sizes the
 * occlusion block r3 every frame; the probe remembers the block and checks its dilated planes at job 48's entry. */
#if defined(PPU_OVERRIDE_001D7E58) || defined(PPU_OVERRIDE_001E35C8)
extern "C" void kz3_cull_planes_block(uint32_t blk);
#define KZ3_CULL_BLK(addr) \
    void func_##addr(ppu_context* ctx) { const uint32_t b = (uint32_t)ctx->gpr[3]; func_##addr##__lifted(ctx); kz3_cull_planes_block(b); }
#endif
#ifdef PPU_OVERRIDE_001D7E58
KZ3_CULL_BLK(001D7E58)
#endif
#ifdef PPU_OVERRIDE_001E35C8
KZ3_CULL_BLK(001E35C8)
#endif

#if defined(PPU_OVERRIDE_00300250) || defined(PPU_OVERRIDE_0030BC20) /* 1.14 twin 0x30BC20 (M = *0x15FB668), pass 53 */
/* @0x00300250 Asset Streaming WaitUntilIdle/shutdown (RE pass 106; pass 48): KZ3_STREAM_PROBE=1 marks the wait so
 * kz3_stream_probe.c's thread can dump the request entries while it lasts. Guest-visible behaviour is unchanged. */
extern "C" void kz3_stream_idle_enter(uint32_t self, int v114);
extern "C" void kz3_stream_idle_exit(uint32_t self, uint32_t ret);
static void kz3_stream_idle_body(ppu_context* ctx, void (*orig)(ppu_context*), int v114)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    kz3_stream_idle_enter(self, v114);
    orig(ctx);
    kz3_stream_idle_exit(self, (uint32_t)ctx->gpr[3]);
}
#ifdef PPU_OVERRIDE_00300250
void func_00300250(ppu_context* ctx) { kz3_stream_idle_body(ctx, func_00300250__lifted, 0); }
#endif
#ifdef PPU_OVERRIDE_0030BC20
void func_0030BC20(ppu_context* ctx) { kz3_stream_idle_body(ctx, func_0030BC20__lifted, 1); }
#endif
#endif

#ifdef PPU_OVERRIDE_00597170
/* @0x00597170 ControlledFollowCameraEntity::vf18 (follow-camera update; also reached from DirectionElastic...::vf18
 * 0x599068): KZ3_CAM_PROBE=1 prints the camera and its target after the body (kz3_cam_probe.c, SPU pass 50, K2). */
extern "C" int kz3_cam_probe_on(void);
extern "C" void kz3_cam_probe_follow(uint32_t self, float dt, uint32_t lr);
void func_00597170(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    const float dt = (float)ctx->fpr[1];
    func_00597170__lifted(ctx);
    if (kz3_cam_probe_on()) kz3_cam_probe_follow(self, dt, lr);
}
#endif
#ifdef PPU_OVERRIDE_005B1558
/* @0x005B1558 CameraEntity FOV helper -> f1: KZ3_CAM_PROBE=1 prints the camera, the FOV and the view block. */
extern "C" int kz3_cam_probe_on(void);
extern "C" void kz3_cam_probe_fov(uint32_t self, float fov, uint32_t lr);
void func_005B1558(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    func_005B1558__lifted(ctx);
    if (kz3_cam_probe_on()) kz3_cam_probe_fov(self, (float)ctx->fpr[1], lr);
}
#endif
#ifdef PPU_OVERRIDE_00D9A908
/* @0x00D9A908 stream slot MakePacket(out, slot, arg) (RE pass 109 s.1): KZ3_PKT_PROBE=1 counts packets made per slot. */
extern "C" int kz3_pkt_probe_on(void);
extern "C" void kz3_pkt_probe_make(uint32_t out, uint32_t slot);
void func_00D9A908(ppu_context* ctx)
{
    const uint32_t out = (uint32_t)ctx->gpr[3], slot = (uint32_t)ctx->gpr[4];
    func_00D9A908__lifted(ctx);
    if (kz3_pkt_probe_on()) kz3_pkt_probe_make(out, slot);
}
#endif
#ifdef PPU_OVERRIDE_00D9A050
/* @0x00D9A050 Packet::Release(pkt) (last reference -> AdvanceReadPos, back to the pool): KZ3_PKT_PROBE=1 counts it. */
extern "C" int kz3_pkt_probe_on(void);
extern "C" void kz3_pkt_probe_release(uint32_t pkt);
void func_00D9A050(ppu_context* ctx)
{
    if (kz3_pkt_probe_on()) kz3_pkt_probe_release((uint32_t)ctx->gpr[3]);
    func_00D9A050__lifted(ctx);
}
#endif
/* Pass 50 second pass: KZ3_CAM_PROBE=1 counters on the K2 Hammer objects (kz3_cam_probe_mark kinds 0-3). */
#if defined(PPU_OVERRIDE_0059FDA0) || defined(PPU_OVERRIDE_0064043C) || defined(PPU_OVERRIDE_0063C784) || defined(PPU_OVERRIDE_0063CB40)
extern "C" int kz3_cam_probe_on(void);
extern "C" void kz3_cam_probe_mark(int kind, uint32_t self, uint32_t lr, uint32_t ret);
#define KZ3_CAM_MARK(ADDR, KIND)                                                                                       \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;                                           \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_cam_probe_on()) kz3_cam_probe_mark(KIND, self, lr, (uint32_t)ctx->gpr[3]);                             \
    }
#endif
#ifdef PPU_OVERRIDE_0059FDA0
KZ3_CAM_MARK(0059FDA0, 0)
#endif
#ifdef PPU_OVERRIDE_0064043C
KZ3_CAM_MARK(0064043C, 1)
#endif
#ifdef PPU_OVERRIDE_0063C784
KZ3_CAM_MARK(0063C784, 2)
#endif
#ifdef PPU_OVERRIDE_0063CB40
KZ3_CAM_MARK(0063CB40, 3)
#endif
/* Pass 51: KZ3_MOUNT_PROBE=1 gunner mount chain (kz3_mount_probe.c, RE pass 112 s.2/s.3, K2 black). */
#ifdef PPU_OVERRIDE_005CBAE4
extern "C" int kz3_mount_probe_on(void);
extern "C" void kz3_mount_probe_mount(uint32_t gun, uint32_t hum, uint32_t name, uint32_t b6, uint32_t b7, uint32_t lr);
/* @0x005CBAE4 MountableEntity::Mount(gun, humanoid, name, b6, b7): gates logged at entry. */
void func_005CBAE4(ppu_context* ctx)
{
    if (kz3_mount_probe_on())
        kz3_mount_probe_mount((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                              (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7], (uint32_t)ctx->lr);
    func_005CBAE4__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_00322940
extern "C" int kz3_mount_probe_on(void);
extern "C" void kz3_mount_probe_ready(uint32_t ent, uint32_t ret, uint32_t lr);
/* @0x00322940 readiness test(entity): result at Mount's call sites. */
void func_00322940(ppu_context* ctx)
{
    const uint32_t ent = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    func_00322940__lifted(ctx);
    if (kz3_mount_probe_on()) kz3_mount_probe_ready(ent, (uint32_t)ctx->gpr[3], lr);
}
#endif
#ifdef PPU_OVERRIDE_00331648
extern "C" int kz3_mount_probe_on(void);
extern "C" void kz3_mount_probe_evsend(uint32_t mgr, uint32_t ev, uint32_t lr);
/* @0x00331648 event send(mgr, event, ...): Mount's deferral event 0x2D. */
void func_00331648(ppu_context* ctx)
{
    if (kz3_mount_probe_on()) kz3_mount_probe_evsend((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->lr);
    func_00331648__lifted(ctx);
}
#endif
#if defined(PPU_OVERRIDE_005CB61C) || defined(PPU_OVERRIDE_005DCC6C) || defined(PPU_OVERRIDE_00694700) ||               \
    defined(PPU_OVERRIDE_005A5778) || defined(PPU_OVERRIDE_005C8C10) || defined(PPU_OVERRIDE_005A00CC)
extern "C" int kz3_mount_probe_on(void);
extern "C" void kz3_mount_probe_mark(int kind, uint32_t self, uint32_t r4, uint32_t lr, uint32_t ret);
#define KZ3_MOUNT_MARK(ADDR, KIND)                                                                                     \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;               \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_mount_probe_on()) kz3_mount_probe_mark(KIND, self, r4, lr, (uint32_t)ctx->gpr[3]);                     \
    }
#endif
#ifdef PPU_OVERRIDE_005CB61C
KZ3_MOUNT_MARK(005CB61C, 3)
#endif
#ifdef PPU_OVERRIDE_005DCC6C
KZ3_MOUNT_MARK(005DCC6C, 4)
#endif
#ifdef PPU_OVERRIDE_00694700
KZ3_MOUNT_MARK(00694700, 5)
#endif
#ifdef PPU_OVERRIDE_005A5778
KZ3_MOUNT_MARK(005A5778, 6)
#endif
#ifdef PPU_OVERRIDE_005C8C10
KZ3_MOUNT_MARK(005C8C10, 7)
#endif
#ifdef PPU_OVERRIDE_005A00CC
KZ3_MOUNT_MARK(005A00CC, 8)
#endif
/* Pass 51: KZ3_PKT_PROBE=1 stream voice ring (kz3_voice_probe.c, RE pass 112 s.4/s.5, quit hang). */
/* Pass 53: the 1.14 twins (RE pass 116 s.36.12, all identical bodies, +0x13B10): 0xA7A25C, 0xA7A438, 0xA7AA9C. */
#if defined(PPU_OVERRIDE_00A6674C) || defined(PPU_OVERRIDE_00A7A25C)
extern "C" void kz3_voice_probe_retire(uint32_t voice, uint32_t lr, int v114);
/* @0x00A6674C RetirePackets(voice). */
static void kz3_retire_body(ppu_context* ctx, void (*orig)(ppu_context*), int v114)
{
    kz3_voice_probe_retire((uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, v114);
    orig(ctx);
}
#ifdef PPU_OVERRIDE_00A6674C
void func_00A6674C(ppu_context* ctx) { kz3_retire_body(ctx, func_00A6674C__lifted, 0); }
#endif
#ifdef PPU_OVERRIDE_00A7A25C
void func_00A7A25C(ppu_context* ctx) { kz3_retire_body(ctx, func_00A7A25C__lifted, 1); }
#endif
#endif
#if defined(PPU_OVERRIDE_00A66928) || defined(PPU_OVERRIDE_00A7A438)
extern "C" void kz3_voice_probe_update(uint32_t voice);
/* @0x00A66928 stream voice Update(voice). */
static void kz3_vupdate_body(ppu_context* ctx, void (*orig)(ppu_context*))
{
    kz3_voice_probe_update((uint32_t)ctx->gpr[3]);
    orig(ctx);
}
#ifdef PPU_OVERRIDE_00A66928
void func_00A66928(ppu_context* ctx) { kz3_vupdate_body(ctx, func_00A66928__lifted); }
#endif
#ifdef PPU_OVERRIDE_00A7A438
void func_00A7A438(ppu_context* ctx) { kz3_vupdate_body(ctx, func_00A7A438__lifted); }
#endif
#endif
#if defined(PPU_OVERRIDE_00A66F8C) || defined(PPU_OVERRIDE_00A7AA9C)
extern "C" void kz3_voice_probe_push(uint32_t voice, uint32_t entry, uint32_t ret);
/* @0x00A66F8C PacketQueue::Push(voice, entry, flags) -> 1 pushed / 0 full. */
static void kz3_vpush_body(ppu_context* ctx, void (*orig)(ppu_context*))
{
    const uint32_t voice = (uint32_t)ctx->gpr[3], entry = (uint32_t)ctx->gpr[4];
    orig(ctx);
    kz3_voice_probe_push(voice, entry, (uint32_t)ctx->gpr[3]);
}
#ifdef PPU_OVERRIDE_00A66F8C
void func_00A66F8C(ppu_context* ctx) { kz3_vpush_body(ctx, func_00A66F8C__lifted); }
#endif
#ifdef PPU_OVERRIDE_00A7AA9C
void func_00A7AA9C(ppu_context* ctx) { kz3_vpush_body(ctx, func_00A7AA9C__lifted); }
#endif
#endif
#if defined(PPU_OVERRIDE_00A671A0) || defined(PPU_OVERRIDE_00A66D54) || defined(PPU_OVERRIDE_00A66B00) ||               \
    defined(PPU_OVERRIDE_00A69110) || defined(PPU_OVERRIDE_00A698A0) || defined(PPU_OVERRIDE_00A6F500) ||              \
    defined(PPU_OVERRIDE_00A456A8) || defined(PPU_OVERRIDE_00A69EE4) || defined(PPU_OVERRIDE_00A6A498) ||              \
    defined(PPU_OVERRIDE_00A65794)
extern "C" void kz3_voice_probe_mark(int kind, uint32_t self, uint32_t lr);
#define KZ3_VOICE_MARK(ADDR, KIND)                                                                                     \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        kz3_voice_probe_mark(KIND, (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr);                                          \
        func_##ADDR##__lifted(ctx);                                                                                    \
    }
#endif
#ifdef PPU_OVERRIDE_00A671A0
KZ3_VOICE_MARK(00A671A0, 0)
#endif
#ifdef PPU_OVERRIDE_00A66D54
KZ3_VOICE_MARK(00A66D54, 1)
#endif
#ifdef PPU_OVERRIDE_00A66B00
KZ3_VOICE_MARK(00A66B00, 2)
#endif
#ifdef PPU_OVERRIDE_00A69110
KZ3_VOICE_MARK(00A69110, 3)
#endif
#ifdef PPU_OVERRIDE_00A698A0
KZ3_VOICE_MARK(00A698A0, 4)
#endif
#ifdef PPU_OVERRIDE_00A6F500
KZ3_VOICE_MARK(00A6F500, 6)
#endif
#ifdef PPU_OVERRIDE_00A456A8
KZ3_VOICE_MARK(00A456A8, 7)
#endif
#ifdef PPU_OVERRIDE_00A69EE4
KZ3_VOICE_MARK(00A69EE4, 8)
#endif
#ifdef PPU_OVERRIDE_00A6A498
KZ3_VOICE_MARK(00A6A498, 9)
#endif
#ifdef PPU_OVERRIDE_00A65794
KZ3_VOICE_MARK(00A65794, 10)
#endif
#if (defined(PPU_OVERRIDE_00A57B64) && defined(PPU_OVERRIDE_00A6674C)) ||                                             \
    (defined(PPU_OVERRIDE_00A6B674) && defined(PPU_OVERRIDE_00A7A25C))
/* @0x00A57B64 stream manager request cancel (0x300250 calls it twice before its idle wait): KZ3_VOICE_DRAIN=1 (opt-in
 * workaround for the quit-to-menu hang, SPU pass 51) drains the stream voices that hold packets they never retire,
 * after the second call (the cancel-all with parameters), on the calling (main) thread, the whole context saved. */
extern "C" int kz3_voice_drain_on(void);
extern "C" int kz3_voice_drain_list(uint32_t* out, int max);
/* 1.14 (pass 53): twin 0xA6B674, caller window 0x30BC20..0x30C1D0, RetirePackets 0xA7A25C (RE pass 116 s.36.12). */
static void kz3_cancel_body(ppu_context* ctx, void (*orig)(ppu_context*), void (*retire)(ppu_context*), uint32_t lo,
                            uint32_t hi)
{
    const uint32_t lr = (uint32_t)ctx->lr, r4 = (uint32_t)ctx->gpr[4];
    orig(ctx);
    if (r4 && lr >= lo && lr < hi && kz3_voice_drain_on()) {
        uint32_t v[16];
        const int n = kz3_voice_drain_list(v, 16);
        ppu_context saved = *ctx;
        for (int i = 0; i < n; i++) {
            *ctx = saved;
            ctx->gpr[3] = v[i];
            retire(ctx);
        }
        *ctx = saved;
    }
}
#if defined(PPU_OVERRIDE_00A57B64) && defined(PPU_OVERRIDE_00A6674C)
void func_00A57B64(ppu_context* ctx)
{
    kz3_cancel_body(ctx, func_00A57B64__lifted, func_00A6674C, 0x00300250u, 0x00300800u);
}
#endif
#if defined(PPU_OVERRIDE_00A6B674) && defined(PPU_OVERRIDE_00A7A25C)
void func_00A6B674(ppu_context* ctx)
{
    kz3_cancel_body(ctx, func_00A6B674__lifted, func_00A7A25C, 0x0030BC20u, 0x0030C1D0u);
}
#endif
#endif

/* ---- SPU pass 52: KZ3_AMA_PROBE (kz3_ama_probe.c) and KZ3_RAIL_PROBE (kz3_rail_probe.c); 1.00 only. ---- */
#ifdef PPU_OVERRIDE_00089D7C
extern "C" void kz3_ama_wait_enter(uint32_t e, uint32_t lr);
extern "C" void kz3_ama_wait_exit(uint32_t e, uint32_t lr);
/* @0x00089D7C WaitForJobGroup(E, poll_us) (RE pass 114 s.1). */
void func_00089D7C(ppu_context* ctx)
{
    const uint32_t e = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    kz3_ama_wait_enter(e, lr);
    func_00089D7C__lifted(ctx);
    kz3_ama_wait_exit(e, lr);
}
#endif
#ifdef PPU_OVERRIDE_00A798B8
extern "C" void kz3_ama_poll(uint32_t d, uint32_t ret);
/* @0x00A798B8 job-list finished poll(D) -> 1 done. */
void func_00A798B8(ppu_context* ctx)
{
    const uint32_t d = (uint32_t)ctx->gpr[3];
    func_00A798B8__lifted(ctx);
    kz3_ama_poll(d, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_0008978C
extern "C" void kz3_ama_push(uint32_t g, uint64_t hdr, uint64_t swap, uint32_t lr, int after);
/* @0x0008978C job-group push(G, r4, r5): A78648(*G, r5) then A78568(spurs, wid, r4). */
void func_0008978C(ppu_context* ctx)
{
    const uint32_t g = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    const uint64_t r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    kz3_ama_push(g, r5, r4, lr, 0);
    func_0008978C__lifted(ctx);
    kz3_ama_push(g, r5, r4, lr, 1);
}
#endif
#if defined(PPU_OVERRIDE_0079B3F0) || defined(PPU_OVERRIDE_0079B570) || defined(PPU_OVERRIDE_0079A740) ||               \
    defined(PPU_OVERRIDE_007B3598) || defined(PPU_OVERRIDE_0079B4D8)
extern "C" void kz3_rail_probe_hit(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr);
#define KZ3_RAIL_HIT(ADDR, KIND, AFTER_F)                                                                              \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;               \
        const float f_in = (float)ctx->fpr[1];                                                                         \
        if (KIND == 0 || KIND == 5) {                                                                                  \
            func_##ADDR##__lifted(ctx);                                                                                \
            kz3_rail_probe_hit(KIND, self, r4, AFTER_F ? (float)ctx->fpr[1] : f_in, lr);                               \
        } else {                                                                                                       \
            kz3_rail_probe_hit(KIND, self, r4, f_in, lr);                                                              \
            func_##ADDR##__lifted(ctx);                                                                                \
        }                                                                                                              \
    }
#endif
#ifdef PPU_OVERRIDE_0079B3F0
KZ3_RAIL_HIT(0079B3F0, 0, 0)
#endif
#ifdef PPU_OVERRIDE_0079B570
KZ3_RAIL_HIT(0079B570, 1, 0)
#endif
#ifdef PPU_OVERRIDE_0079A740
KZ3_RAIL_HIT(0079A740, 2, 0)
#endif
#if defined(PPU_OVERRIDE_00384D10) || defined(PPU_OVERRIDE_003859D0) || defined(PPU_OVERRIDE_00385E14) ||               \
    defined(PPU_OVERRIDE_00385E58) || defined(PPU_OVERRIDE_00B9B078) || defined(PPU_OVERRIDE_0031FB8C) ||               \
    defined(PPU_OVERRIDE_00845D50)
/* SPU pass 65 (RE pass 123, the RE notes s.7.5): KZ3_RAIL_PROBE=1 on PathMover's own Lua rows (the Hammer is an
 * InertiaPathMover, which uses them), vf48's movement record, the path-name lookup and CreateMover. kind: 0 SetPath,
 * 1 Teleport, 2 BlendToPath, 3 GetTime, 4 vf48, 5 lookup, 6 CreateMover; logged on return (r4 = the entry value). */
extern "C" void kz3_rail_probe_pm(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr, uint32_t ret);
#define KZ3_RAIL_PM(ADDR, KIND)                                                                                        \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;               \
        func_##ADDR##__lifted(ctx);                                                                                    \
        kz3_rail_probe_pm(KIND, self, r4, (float)ctx->fpr[1], lr, (uint32_t)ctx->gpr[3]);                              \
    }
#endif
#ifdef PPU_OVERRIDE_00384D10
extern "C" void kz3_rail_probe_hit(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr);
/* @0x00384D10 PathMover vf48(this, out): count at entry (kind 3 of kz3_rail_probe_hit), record dump on return. */
void func_00384D10(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    kz3_rail_probe_hit(3, self, r4, (float)ctx->fpr[1], lr);
    func_00384D10__lifted(ctx);
    kz3_rail_probe_pm(4, self, r4, 0.0f, lr, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_003859D0
KZ3_RAIL_PM(003859D0, 0)
#endif
#ifdef PPU_OVERRIDE_00385E14
KZ3_RAIL_PM(00385E14, 1)
#endif
#ifdef PPU_OVERRIDE_00385E58
KZ3_RAIL_PM(00385E58, 2)
#endif
#ifdef PPU_OVERRIDE_00B9B078
KZ3_RAIL_PM(00B9B078, 3)
#endif
#ifdef PPU_OVERRIDE_0031FB8C
/* @0x0031FB8C name-table lookup(table, &name, 1): hot (14 callers); only the SetPath (lr 0x385A10) and CreateMover
 * (lr 0x845D88) calls reach the probe. */
void func_0031FB8C(ppu_context* ctx)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    if (lr != 0x385A10u && lr != 0x845D88u) {
        func_0031FB8C__lifted(ctx);
        return;
    }
    const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4];
    func_0031FB8C__lifted(ctx);
    kz3_rail_probe_pm(5, self, r4, 0.0f, lr, (uint32_t)ctx->gpr[3]);
}
#endif
#ifdef PPU_OVERRIDE_00845D50
KZ3_RAIL_PM(00845D50, 6)
#endif
#ifdef PPU_OVERRIDE_007B3598
KZ3_RAIL_HIT(007B3598, 4, 0)
#endif
#ifdef PPU_OVERRIDE_0079B4D8
KZ3_RAIL_HIT(0079B4D8, 5, 1)
#endif
#ifdef PPU_OVERRIDE_007B3CD0
extern "C" void kz3_rail_probe_frame(uint32_t self, uint32_t lr);
/* @0x007B3CD0 the frame function that calls GameWorld::LogicStep (KZ3_RAIL_PROBE=1: its gates). */
void func_007B3CD0(ppu_context* ctx)
{
    kz3_rail_probe_frame((uint32_t)ctx->gpr[3], (uint32_t)ctx->lr);
    func_007B3CD0__lifted(ctx);
}
#endif
#if defined(PPU_OVERRIDE_007B1C00) || defined(PPU_OVERRIDE_007B1D30) || defined(PPU_OVERRIDE_00849FB8) ||               \
    defined(PPU_OVERRIDE_007BA8C8) || defined(PPU_OVERRIDE_008C4E7C) || defined(PPU_OVERRIDE_008C5808)
extern "C" void kz3_rail_probe_pause(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr, int after);
/* SPU pass 53 (RE pass 117, pacing s.8): KZ3_RAIL_PROBE=1 logs entry and return of the world pause/resume paths. */
#define KZ3_PAUSE_HIT(ADDR, KIND)                                                                                      \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], r4 = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;               \
        const float f_in = (float)ctx->fpr[1];                                                                         \
        kz3_rail_probe_pause(KIND, self, r4, f_in, lr, 0);                                                             \
        func_##ADDR##__lifted(ctx);                                                                                    \
        kz3_rail_probe_pause(KIND, self, r4, f_in, lr, 1);                                                             \
    }
#endif
#ifdef PPU_OVERRIDE_007B1C00
KZ3_PAUSE_HIT(007B1C00, 0) /* @0x007B1C00 GameModule vf18 Pause(W, r4) */
#endif
#ifdef PPU_OVERRIDE_007B1D30
KZ3_PAUSE_HIT(007B1D30, 1) /* @0x007B1D30 GameModule vf19 Resume(W) */
#endif
#ifdef PPU_OVERRIDE_00849FB8
KZ3_PAUSE_HIT(00849FB8, 2) /* @0x00849FB8 SetGameTimeScale script native */
#endif
#ifdef PPU_OVERRIDE_007BA8C8
KZ3_PAUSE_HIT(007BA8C8, 3) /* @0x007BA8C8 level-start state machine (state 5 stores +0x88 at 0x7BADEC) */
#endif
#ifdef PPU_OVERRIDE_008C4E7C
KZ3_PAUSE_HIT(008C4E7C, 4) /* @0x008C4E7C loading/intro movie start (pauses at lr 0x8C5030) */
#endif
#ifdef PPU_OVERRIDE_008C5808
KZ3_PAUSE_HIT(008C5808, 5) /* @0x008C5808 movie stop (resumes at lr 0x8C58C0, state 3) */
#endif
#ifdef PPU_OVERRIDE_001C56C0
extern "C" void kz3_rail_probe_count(uint32_t obj, uint32_t r4, uint32_t r31, uint32_t lr);
/* @0x001C56C0 atomic +-1 of [r3+0x2060] (r4 = 0: +1); KZ3_RAIL_PROBE=1 logs the caller's object (pass 53). */
void func_001C56C0(ppu_context* ctx)
{
    if ((uint32_t)ctx->gpr[3] == 0x01514AD0u)
        kz3_rail_probe_count((uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[31], (uint32_t)ctx->lr);
    func_001C56C0__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_000F7418
extern "C" void kz3_rail_probe_ready(uint32_t item, uint32_t ret, uint32_t lr);
/* @0x000F7418 ready(item) -> 1 when every resource id of item+0x80 is loaded (pass 53). */
void func_000F7418(ppu_context* ctx)
{
    const uint32_t item = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    func_000F7418__lifted(ctx);
    kz3_rail_probe_ready(item, (uint32_t)ctx->gpr[3], lr);
}
#endif
/* ---- SPU pass 54: KZ3_POOL_PROBE=1 (kz3_pool_probe.c): GPU-pool handle registration / allocation; 1.00 only. ---- */
extern "C" int kz3_pool_probe_on(void);
#ifdef PPU_OVERRIDE_0004BC70
extern "C" void kz3_pool_probe_reg(uint32_t mgr, uint32_t handle, uint32_t lr);
/* @0x0004BC70 register(mgr, handle): table[id] = handle, handle+0xC = id (RE pass 120 s.9.1). */
void func_0004BC70(ppu_context* ctx)
{
    const uint32_t mgr = (uint32_t)ctx->gpr[3], h = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    func_0004BC70__lifted(ctx);
    if (kz3_pool_probe_on()) kz3_pool_probe_reg(mgr, h, lr);
}
#endif
#ifdef PPU_OVERRIDE_000497F4
extern "C" unsigned long long kz3_pool_probe_alloc(int after, unsigned long long t0, uint32_t mgr, uint32_t handle,
                                                   uint32_t size, uint32_t align, uint32_t ret, uint32_t r13, uint32_t lr);
/* @0x000497F4 pool alloc(mgr, handle, size, align) -> 1 ok / 0 (writes res[0]; RE pass 120 s.9.2). */
void func_000497F4(ppu_context* ctx)
{
    if (!kz3_pool_probe_on()) {
        func_000497F4__lifted(ctx);
        return;
    }
    const uint32_t mgr = (uint32_t)ctx->gpr[3], h = (uint32_t)ctx->gpr[4], sz = (uint32_t)ctx->gpr[5],
                   al = (uint32_t)ctx->gpr[6], r13 = (uint32_t)ctx->gpr[13], lr = (uint32_t)ctx->lr;
    const unsigned long long t0 = kz3_pool_probe_alloc(0, 0, mgr, h, sz, al, 0, r13, lr);
    func_000497F4__lifted(ctx);
    kz3_pool_probe_alloc(1, t0, mgr, h, sz, al, (uint32_t)ctx->gpr[3], r13, lr);
}
#endif
#ifdef PPU_OVERRIDE_0004CA24
extern "C" void kz3_pool_probe_vb(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t ret, uint32_t lr);
/* @0x0004CA24 vertex-buffer create from a read stream (-> 0 failed). */
void func_0004CA24(ppu_context* ctx)
{
    const uint32_t a = (uint32_t)ctx->gpr[3], b = (uint32_t)ctx->gpr[4], c = (uint32_t)ctx->gpr[5],
                   d = (uint32_t)ctx->gpr[6], lr = (uint32_t)ctx->lr;
    func_0004CA24__lifted(ctx);
    if (kz3_pool_probe_on()) kz3_pool_probe_vb(a, b, c, d, (uint32_t)ctx->gpr[3], lr);
}
#endif
extern "C" void kz3_pool_probe_load(int kind, uint32_t self, uint32_t lr);
#define KZ3_POOL_LOAD(ADDR, KIND)                                                                                       \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        const uint32_t self = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;                                           \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_pool_probe_on()) kz3_pool_probe_load(KIND, self, lr);                                                  \
    }
#ifdef PPU_OVERRIDE_0004A200
extern "C" void kz3_pool_probe_svc(void);
/* @0x0004A200 stream-request service(S = 0x13F5040), called per frame from Renderer::FrameTick. */
void func_0004A200(ppu_context* ctx)
{
    if (kz3_pool_probe_on()) kz3_pool_probe_svc();
    func_0004A200__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_00049D2C
extern "C" void kz3_pool_probe_req(uint32_t handle, uint32_t ret, uint32_t lr);
/* @0x00049D2C request(S, handle): alloc through 0x49C74 and queue the read 0x48FB8 (-> 1). */
void func_00049D2C(ppu_context* ctx)
{
    const uint32_t h = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    func_00049D2C__lifted(ctx);
    if (kz3_pool_probe_on()) kz3_pool_probe_req(h, (uint32_t)ctx->gpr[3], lr);
}
#endif
#ifdef PPU_OVERRIDE_0031E328
extern "C" void kz3_pool_probe_words(const char* tag, uint32_t ea, unsigned nw, long call);
/* @0x0031E328 frustum build(out r3, camera r4): pass 55, the stream views' planes read 0xFFF50000. */
void func_0031E328(ppu_context* ctx)
{
    static long n;
    const uint32_t out = (uint32_t)ctx->gpr[3], in = (uint32_t)ctx->gpr[4];
    const long c = ++n;
    const int show = kz3_pool_probe_on() && (c <= 2 || c == 300 || c == 3000);
    if (show) kz3_pool_probe_words("frustum-in", in, 24, c);
    func_0031E328__lifted(ctx);
    if (show) {
        kz3_pool_probe_words("frustum-out", out, 32, c);
        kz3_pool_probe_words("frustum-out2", out + 0x80, 32, c);
    }
}
#endif
#ifdef PPU_OVERRIDE_001D0D50
extern "C" void kz3_pool_probe_words(const char* tag, uint32_t ea, unsigned nw, long call);
/* @0x001D0D50 stream-view copy from a frustum (r3 view, r4 frustum): pass 55. */
void func_001D0D50(ppu_context* ctx)
{
    static long n;
    const long c = ++n;
    if (kz3_pool_probe_on() && (c <= 2 || c == 300 || c == 3000))
        kz3_pool_probe_words("sview-src", (uint32_t)ctx->gpr[4], 32, c);
    func_001D0D50__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_007F0948
extern "C" void kz3_pool_probe_cam(uint32_t rview, long call);
/* @0x007F0948 texture-streaming view build (r4 = render view; its camera [r4+0x10] fills the frustum input through
 * vt+0x44): pass 55, the camera matrix there reads 0xFFF50000. */
void func_007F0948(ppu_context* ctx)
{
    static long n;
    const long c = ++n;
    /* pass 56: every 25th call too (the clamp runs reach ~200 calls), to see whether the camera ever gets a matrix */
    if (kz3_pool_probe_on() && (c <= 2 || (c % 25 == 0 && c <= 1000) || c == 3000)) kz3_pool_probe_cam((uint32_t)ctx->gpr[4], c);
    func_007F0948__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_0031EB04
extern "C" void kz3_cam_watch_arm(uint32_t cam, uint32_t lr);
/* @0x0031EB04 camera (vt 0xEBA1D8) ctor (r4 = this): KZ3_CAM_WATCH=1 write watch on its matrix (pass 56). */
void func_0031EB04(ppu_context* ctx)
{
    const uint32_t cam = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    func_0031EB04__lifted(ctx);
    kz3_cam_watch_arm(cam, lr);
}
#endif
#ifdef PPU_OVERRIDE_0038C3D8
extern "C" void kz3_cam_watch_comp(uint32_t comp, uint32_t lr);
/* @0x0038C3D8 render-camera description from a camera component (r3 out, r4 comp; matrix comp+0x60..0x9F, vt+0x68/
 * 0x70/0x74 floats, comp+0x1DC..0x1FF): KZ3_CAM_WATCH=1 dump + watch of an exponent-255 component (pass 56). */
void func_0038C3D8(ppu_context* ctx)
{
    const uint32_t comp = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    kz3_cam_watch_comp(comp, lr);
    func_0038C3D8__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_005BCB90
extern "C" void kz3_cam_watch_part(uint32_t self, uint32_t lr);
/* @0x005BCB90 rewrites a camera component's matrix (comp+0x60) from a vt+0x68 getter: KZ3_CAM_WATCH=1 logs this
 * (pass 56). */
void func_005BCB90(ppu_context* ctx)
{
    kz3_cam_watch_part((uint32_t)ctx->gpr[3], (uint32_t)ctx->lr);
    func_005BCB90__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_005E0C88
extern "C" void kz3_cam_watch_view(uint32_t part, uint32_t out, int after, long call);
/* @0x005E0C88 part vt+0x68: gunner view matrix from two skeleton bones + smoothed yaw/pitch (pass 56 probe). */
void func_005E0C88(ppu_context* ctx)
{
    static long n;
    const long c = ++n;
    const uint32_t part = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    func_005E0C88__lifted(ctx);
    kz3_cam_watch_view(part, out, 1, c);
}
#endif
#ifdef PPU_OVERRIDE_0031DFB8
extern "C" void kz3_pool_probe_camset(uint32_t cam, uint32_t src, uint32_t lr, long call);
/* @0x0031DFB8 camera (vt 0xEBA1D8) vt+0x40: matrix cam+0x10..0x4F from the description r4 (pass 56 probe). */
void func_0031DFB8(ppu_context* ctx)
{
    static long n;
    const long c = ++n;
    const uint32_t cam = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    func_0031DFB8__lifted(ctx);
    if (kz3_pool_probe_on()) kz3_pool_probe_camset(cam, src, lr, c);
}
#endif
#ifdef PPU_OVERRIDE_001C5208
extern "C" void kz3_pool_probe_job(uint32_t obj, int after);
extern "C" void kz3_pool_probe_deep(uint32_t obj);
/* @0x001C5208 build + push the Gfx.TextureStreaming SPU job (module 42) for obj 0x1514AD0. */
void func_001C5208(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    if (kz3_pool_probe_on()) kz3_pool_probe_job(obj, 0);
    /* pass 58: the round-55 view clamp (KZ3_TS_CLAMP) is gone: patch 1071 computes the NaN-free SPU lanes itself. */
    func_001C5208__lifted(ctx);
    if (kz3_pool_probe_on()) kz3_pool_probe_deep(obj); /* pass 55: after the user data is written */
}
#endif
#ifdef PPU_OVERRIDE_001B4CB8
KZ3_POOL_LOAD(001B4CB8, 0) /* @0x001B4CB8 VertexArrayResource load vf15 */
#endif
#ifdef PPU_OVERRIDE_001B4E14
KZ3_POOL_LOAD(001B4E14, 1) /* @0x001B4E14 IndexArrayResource load vf15 */
#endif

/* ---- SPU pass 57: KZ3_JOIN2_PROBE=1 (kz3_hkquit_probe.c + patch 1072): Havok pool teardown at the T7 quit; 1.00 only. ---- */
extern "C" int kz3_hkq_on(void);
#ifdef PPU_OVERRIDE_008E3DAC
extern "C" void kz3_hkq_process(uint32_t pool, uint32_t queue, uint32_t type, uint32_t lr);
/* @0x008E3DAC hkSpuJobThreadPool::processAllJobs(pool, queue, type) (the RE notes s.2). */
void func_008E3DAC(ppu_context* ctx)
{
    if (kz3_hkq_on()) kz3_hkq_process((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->lr);
    func_008E3DAC__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_008E3E8C
extern "C" void kz3_hkq_wait(uint32_t pool, int after);
/* @0x008E3E8C hkSpuJobThreadPool::waitForCompletion(pool). */
void func_008E3E8C(ppu_context* ctx)
{
    const uint32_t pool = (uint32_t)ctx->gpr[3];
    const int on = kz3_hkq_on();
    if (on) kz3_hkq_wait(pool, 0);
    func_008E3E8C__lifted(ctx);
    if (on) kz3_hkq_wait(pool, 1);
}
#endif
#ifdef PPU_OVERRIDE_008E3C10
extern "C" void kz3_hkq_dtor(uint32_t pool, uint32_t lr, int after);
/* @0x008E3C10 hkSpuJobThreadPool::~hkSpuJobThreadPool(pool). */
void func_008E3C10(ppu_context* ctx)
{
    const uint32_t pool = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    const int on = kz3_hkq_on();
    if (on) kz3_hkq_dtor(pool, lr, 0);
    func_008E3C10__lifted(ctx);
    if (on) kz3_hkq_dtor(pool, lr, 1);
}
#endif
#ifdef PPU_OVERRIDE_008E0334
extern "C" void kz3_hkq_destroy(uint32_t util, uint32_t lr, int after);
/* @0x008E0334 hkPs3SpuUtil::destroyTaskset(util): JoinTask2(util+8, 0..127), DestroyTaskset2. */
void func_008E0334(ppu_context* ctx)
{
    const uint32_t util = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    const int on = kz3_hkq_on();
    if (on) kz3_hkq_destroy(util, lr, 0);
    func_008E0334__lifted(ctx);
    if (on) kz3_hkq_destroy(util, lr, 1);
}
#endif
#ifdef PPU_OVERRIDE_008E89A0
extern "C" void kz3_hkq_sem(int release);
/* @0x008E89A0 Havok semaphore release (lwarx/stwcx on the count word). */
void func_008E89A0(ppu_context* ctx)
{
    if (kz3_hkq_on()) kz3_hkq_sem(1);
    func_008E89A0__lifted(ctx);
}
#endif
#ifdef PPU_OVERRIDE_008E88E4
extern "C" void kz3_hkq_sem(int release);
/* @0x008E88E4 Havok semaphore acquire. */
void func_008E88E4(ppu_context* ctx)
{
    if (kz3_hkq_on()) kz3_hkq_sem(0);
    func_008E88E4__lifted(ctx);
}
#endif

#ifdef PPU_OVERRIDE_00220A78
/* @0x00220A78 wait(obj, flag): lwsync-polls *flag until it reads 1, with usleep(10) (0xC2FE4) per poll; when
 * [obj+0x5D44] equals [0x13362B8] it calls 0xC383C(obj, flag, 1) instead (ASSUMPTION: a help-while-waiting path).
 * SPU pass 62's spu-62-new stall sat here for good. KZ3_STALL_DIAG=1 registers the wait so the SPURS watcher can
 * print it after 3 s (kz3_stall_diag.c); the guest body runs unchanged. */
extern "C" void kz3_stall_diag_ppu_wait(int on, uint32_t obj, uint32_t flag, uint32_t lr);
void func_00220A78(ppu_context* ctx)
{
    kz3_stall_diag_ppu_wait(1, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->lr);
    func_00220A78__lifted(ctx);
    kz3_stall_diag_ppu_wait(0, 0, 0, 0);
}
#endif
