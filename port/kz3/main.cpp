/* Killzone 3 port host. Based on ps3recomp templates/project (MIT licence:
 * third_party/ps3recomp/LICENSE). */
/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <atomic>
#ifdef _WIN32
#include <direct.h>    /* _mkdir: the Keyboard page's GUM folder */
#else
#include <sys/stat.h>
#endif
#include "kz3_ini.h"   /* kz3.ini next to the exe: no launch options needed (release pass 26) */
#include "../net/kz3_discord.h" /* Discord Rich Presence, opt-in (network pass 33) */
#include "../net/kz3_host_composed.h" /* every .gum key the port rewrites or generates (network pass 40) */
#include "kz3_embed.h" /* the player's EBOOT.elf linked into kz3.exe (release step 9s) */
#include "kz3_log.h"   /* file log, levels, crash files (also force-included with the stdio redirect) */
#include "kz3_vfs_root.h" /* the game root from the EBOOT path (disc layout, else PS3_GAME above it, else ".") */

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
uint32_t ppu_load_elf_image(const uint8_t* data, uint64_t size);   /* the same from memory (patch 0005) */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
void     cellSaveData_register_ctx_handlers(void); /* 9-argument User*Auto{Load,Save} (patch 3001) */
void     cellGame_init_from_paramsfo(const char* sfo_path); /* title id etc. from PARAM.SFO */
extern uint32_t ppu_hle_inject_base;           /* guest EA of the runtime's GCM window (labels, control) */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * Native mouse look (RSX pass 65, patch 2069; the RE notes s.12.3, the RE notes "Pass 65")
 * -----------------------------------------------------------------------*/
/* A pre-hook at the entry of ControllerBase::SetViewAngles(C, f1 yaw delta, f2 pitch) @0x005A3350 (1.14 @0x005B7760),
 * called from the override in port/spu/kz3_ppu_overrides.cpp. It acts only on the soldier's normal on-foot call
 * (return address 0x00685FA4 / 1.14 0x0069B120, the instruction before it a `bl` to SetViewAngles) for the first
 * local player's entity, and only when the mouse moved (or a KZ3_MOUSE_SCRIPT event is due): it adds the counts to the
 * arguments the game's UpdateLook computed from the stick, so the game's own writer stores them in its own update.
 * The weapon's look scale is the game's: weapon = GetCurrentInventoryItem(E) @0x0061A1E8 (1.14 @0x0061F8F8),
 * scale = weapon->vf163() (vtable +0x28C), zoomed = weapon->vf224() (vtable +0x380) == 0, as UpdateLook does at
 * 0x6854BC / 0x684FD0. The body turns by the yaw RATE UpdateLook stores after the call, so that is adjusted too. Player list and Game/M facts: the RE notes s.12.1 (chains from KAMI, isJuhn/KAMI,
 * MIT; checked against both EBOOTs there). */
#include "../input/pad_mouse.h"
#include "../input/cam_interp.h"
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {
struct Kz3MouseVer {
    uint32_t lr, callee, get_item, players, game, game_m, world;
};
constexpr Kz3MouseVer kMouse100 = {0x00685FA4u, 0x005A3350u, 0x0061A1E8u, 0x013A6D7Cu, 0x01587880u, 0x760u, 0x013A6D7Cu};
constexpr Kz3MouseVer kMouse114 = {0x0069B120u, 0x005B7760u, 0x0061F8F8u, 0x0141C030u, 0x015FCF80u, 0x770u, 0x0141C030u};
inline bool ml_ok(uint32_t a) { return a >= 0x10000u && a < 0xF0000000u; }
inline uint32_t ml_r32(uint32_t a) { return __builtin_bswap32(*(const uint32_t*)(vm_base + a)); }
inline float ml_rf(uint32_t a) { uint32_t v = ml_r32(a); float f; memcpy(&f, &v, 4); return f; }
/* A guest call on the hook's context (the caller restores the whole context afterwards). */
uint32_t ml_call(ppu_context* ctx, uint32_t code, uint32_t r3, double* f1)
{
    ctx->gpr[3] = r3;
    ctx->ctr = code;
    ctx->lr = 0;
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f(ctx); }
    if (f1) *f1 = ctx->fpr[1];
    return (uint32_t)ctx->gpr[3];
}
/* Entity of the first local, non-bot Player (+0x70 local, +0x71 bot, +0x68 entity; list count +0x10, data +0x14). */
uint32_t ml_local_entity(uint32_t players_ptr)
{
    uint32_t mgr = ml_r32(players_ptr);
    if (!ml_ok(mgr)) return 0;
    uint32_t n = ml_r32(mgr + 0x10), data = ml_r32(mgr + 0x14);
    if (!ml_ok(data) || n > 64) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = ml_r32(data + 4 * i);
        if (ml_ok(p) && vm_base[p + 0x70] && !vm_base[p + 0x71]) return ml_r32(p + 0x68);
    }
    return 0;
}
/* KZ3_MOUSE_RATE=0: leave the yaw rate alone (pass 65 diagnosis: the aim point turns, the body does not). */
int kz3_mouse_rate_on(void)
{
    static const int on = [] { const char* e = getenv("KZ3_MOUSE_RATE"); return !(e && e[0] == '0'); }();
    return on;
}
inline uint64_t ml_r64(uint32_t a) { return (uint64_t)ml_r32(a) << 32 | ml_r32(a + 4); }
/* Patch 2108 (RSX pass 103) + RSX pass 107: the slot clock. Each look update gets only the counts that arrived up to
 * a cut on the game's time line, one 1/30 s slot T before the END of the logic slot it simulates (kz3_mouse_cut), so
 * a slot of length L gets L of counts and the yaw rate UpdateLook stores (counts / (K x L), C+0x10C) is the mouse speed
 * whatever the slot length. Default ON since pass 107; KZ3_MOUSE_CLOCK=0 = every count since the last update (rounds
 * 65-106). Why: GameWorld::Update runs one 1/30 s or 2/30 s slot per frame (the RE notes s.50.1), the body turns
 * by the stored rate over game time, and the first-person camera Rep's pose follows the FRAME's game time W+0x78
 * (pass 107 logging: per-frame yaw / game-time step cv 0.075-0.086 with this cut, 0.19 without). Taking every count
 * made the rate count/(K x L) swing with the slot length (33 ms slots over-turn, 67 ms slots under-turn, pass 105).
 * Pass 103's variant cut at the slot START (a lag that changed with the slot length) and measured worse; that cut is
 * gone. Measured: the RE notes "Pass 107". */
int kz3_mouse_clock_on(void)
{
    static const int on = [] { const char* e = getenv("KZ3_MOUSE_CLOCK"); return !(e && e[0] == '0'); }();
    return on;
}
/* The host time (guest timebase, ppu_timebase_now) the look update's logic slot starts at. GameWorld::Update
 * (0x7B3CD0, 1.14 twin with the same W offsets at 0x7C9FFC/0x7CA244; the RE notes s.6.2) keeps the world
 * clock at W+0x70: W+0x70 = the timebase stamp of the frame's Clock::Advance, W+0x78 = game ticks at that stamp,
 * W+0x88 = the clock scale (float, 1 normally); it advances the slot target W+0xE8 by the slot length (1/30 s, 2/30 s when behind) and
 * only then runs LogicStep, so during the look update W+0xE8 is the END of the slot being simulated. The cut in host
 * time = stamp + (W+0xE8 - T - W+0x78) / scale with T = 1/30 s (RSX pass 107; was the slot start). Counts up to that time go
 * to this update; each slot then gets its own length of wall time, whatever frame the slot runs in. Returns 0 (caller takes everything) when the fields look
 * wrong (logged once). lat_ms / slot_ms for the log. */
extern "C" uint64_t ppu_timebase_now(void);
int kz3_mouse_cut(uint32_t world_ptr, uint64_t* cut, double* lat_ms, double* slot_ms)
{
    static uint64_t s_prev_target;
    static int s_bad;
    const uint32_t W = ml_r32(world_ptr);
    if (!ml_ok(W)) return 0;
    const uint64_t stamp = ml_r64(W + 0x70), gnow = ml_r64(W + 0x78), target = ml_r64(W + 0xE8);
    const float scale = ml_rf(W + 0x88);
    const uint64_t tb = ppu_timebase_now();
    const uint64_t T2 = 2ull * 2660000ull;   /* [0x1588F0C] = 1/30 s; one retail slot = 2 T */
    uint64_t slot = target - s_prev_target;
    if (!s_prev_target || target <= s_prev_target || slot > 2 * T2) slot = T2;
    s_prev_target = target;
    /* RSX pass 107: the cut is the slot END minus one 1/30 s slot T, a constant lag, not the slot start (pass 103): a
     * 2T slot (r27 = 2, only when the clock is a whole slot behind: now >= end - T, so the cut is never in the future)
     * then gets 2T of counts, the stored rate counts / (K x dt_slot) stays the mouse speed whatever the slot length,
     * and the slot states are the mouse yaw sampled on the game's time line, which kz3_cam_interp shows at game time. */
    const double lead = (double)(int64_t)(target - 2660000ull - gnow);   /* game ticks from the stamp to the cut */
    if (!(scale > 0.01f && scale < 100.0f) || stamp > tb || tb - stamp > 2ull * 79800000ull || fabs(lead) > 79800000.0) {
        if (s_bad++ < 3)
            fprintf(stderr, "[mouse] clock fields refused (W=0x%08X stamp=%llu now=%llu game=%llu target=%llu scale=%f): "
                            "this update takes every count\n", W, (unsigned long long)stamp, (unsigned long long)tb,
                    (unsigned long long)gnow, (unsigned long long)target, scale);
        return 0;
    }
    const double c = (double)stamp + lead / (double)scale;
    *cut = c <= 0.0 ? 0 : (uint64_t)c;
    *lat_ms = ((double)tb - c) / 79800.0;
    *slot_ms = (double)slot / 79800.0 / (double)scale;
    return 1;
}
/* Mounted guns (RSX pass 103; K4 Frozen Shores' start). The player's view on a mounted gun is the gun's
 * MountedGunJoystickController C (C+0x20 = the gun entity; the soldier's own controller is idle, pass 102 saw only its
 * reset call 0x6950B0). Its look update 0x5F7970 (1.00 0x5E35xx; called from 0x5F7D20 = 1.00 0x5E36E8 region) turns the
 * right stick into ABSOLUTE angles: yaw = wrap(C+0x9C + stick*dt*k) clamped to [r1+0x78, r1+0x7C] and pitch = C+0xA0 +
 * stick*dt*k clamped to [r1+0x80, r1+0x84] (limits in the caller's frame, from the gun entity +0x1BC..+0x1C8), and the
 * caller passes them to SetViewAngles (bl 0x5F83CC, 1.00 0x5E3D94), optionally through the resource's basis change
 * (resource +0x234 != 0: the angles are re-derived with atan2 in another frame). SetViewAngles stores them back to
 * C+0x9C/+0xA0, so the next update starts from what the mouse made. vf1 = MountedGunJoystickController::vf1 (slot 1 of
 * the controller's vtable): only the human-controlled kind takes the mouse. ASSUMPTION: in split-screen the second
 * player's gun would take it too (only one mouse; not run). */
/* Late-latch view rotation (RSX pass 105, patch 2112, ENHANCEMENT, display only; the RE notes s.43): what the
 * last look update took and applied, for the camera writer hook (kz3_cam_latch) to add the counts not taken yet.
 * mode 1 on foot, 2 mounted gun; take_flip = flip of the last take (gate: within 4 flips); k = rad per count incl.
 * ADS/zoom (the last move's); y0/p0 = the angles the update left (logic), with their limits (min > max = free);
 * gun_cam = the mounted look took the 0x5F7028 variant (angles from the Camera: counts would be shown twice). */
extern "C" void kz3_latch_trace(char c);   /* below, with the camera writer hook (RSX pass 105 diagnostics) */
struct Kz3Latch {
    unsigned take_flip;
    int      mode, invert, gun_cam, basis;
    float    k, y0, ymin, ymax, p0, pmin, pmax;
    /* RSX pass 110 render-rate fill (patch 2116): the yaw/pitch this update applied (0 when it took nothing) and the end of
     * the logic slot it ran in (W+0xE8; the camera Rep shows that slot when its +0x110 holds the same value). */
    float    tdy, tdp;
    uint64_t slot_end;
};
uint32_t s_foot_C;   /* the local player's on-foot controller (last look update; RSX pass 107 log) */
Kz3Latch s_lat = {0u, 0, 0, 0, 0, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, -1.0f};

struct Kz3MountVer { uint32_t lr, vf1; };
constexpr Kz3MountVer kMount100 = {0x005E3D98u, 0x005E2128u};
constexpr Kz3MountVer kMount114 = {0x005F83D0u, 0x005F6760u};
void kz3_mouse_mounted(ppu_context* ctx, const Kz3MouseVer* v, const Kz3MountVer* m)
{
    const uint32_t lr = (uint32_t)ctx->lr, C = (uint32_t)ctx->gpr[3];
    static uint64_t s_refused[3];
    auto refuse = [&](int g, const char* why, uint32_t a) {
        if (s_refused[g]++ == 0) fprintf(stderr, "[mouse] mounted gun gate %d refused the first time: %s (lr=0x%08X C=0x%08X 0x%08X)\n",
                                         g, why, lr, C, a);
    };
    const uint32_t w = ml_r32(lr - 4);
    if ((w & 0xFC000003u) != 0x48000001u || (uint32_t)(lr - 4 + (((int32_t)(w << 6) >> 6) & ~3)) != v->callee) {
        refuse(0, "the word before lr is not bl SetViewAngles", w); return;
    }
    const uint32_t vt = ml_ok(C) ? ml_r32(C) : 0, o1 = ml_ok(vt) ? ml_r32(vt + 4) : 0;
    if (!ml_ok(o1) || ml_r32(o1) != m->vf1) { refuse(1, "not a MountedGunJoystickController (vtable slot 1)", vt); return; }
    const uint32_t E = ml_r32(C + 0x20), res = ml_ok(E) ? ml_r32(E + 0xE4) : 0;
    const double now = (double)GetTickCount64() / 1000.0;
    int dx = 0, dy = 0;
    uint64_t cut = 0;
    double lat = 0.0, slot = 0.0;
    const int clk = kz3_mouse_clock_on() && kz3_mouse_cut(v->world, &cut, &lat, &slot);
    const int moved = clk ? pad_mouse_take_until(now, cut, &dx, &dy) : pad_mouse_take(now, &dx, &dy);
    static int s_first = 1;
    if (s_first) {
        s_first = 0;
        fprintf(stderr, "[mouse] mounted gun look update: lr=0x%08X C=0x%08X gun=0x%08X basis change=%u\n", lr, C, E,
                ml_ok(res) ? (unsigned)vm_base[res + 0x234] : 255u);
    }
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    float lim[4] = {1.f, -1.f, 1.f, -1.f};   /* yaw min/max, pitch min/max; min > max = no clamp */
    if (ml_ok(sp) && ml_ok(res) && !vm_base[res + 0x234])
        for (int k = 0; k < 4; k++) lim[k] = ml_rf(sp + 0x78 + 4 * k);
    for (int k = 0; k < 4; k += 2)
        if (!(isfinite(lim[k]) && isfinite(lim[k + 1]) && lim[k] <= lim[k + 1] && fabsf(lim[k]) < 7.f && fabsf(lim[k + 1]) < 7.f))
            { lim[k] = 1.f; lim[k + 1] = -1.f; }
    /* RSX pass 105, gate (f): the caller picked the look variant at 0x5F8240 (1.14): r27 = 1 when [r29+0xEC] and [r29+0x180]
     * are both set -> bl 0x5F7028 at 0x5F829C (angles re-derived from the Camera), else bl 0x5F7970 at 0x5F827C; r27 and
     * r29 are not written between 0x5F82A0 and this call (0x5F83CC), so they still hold the choice here. */
    const int gun_cam = m == &kMount114 ? (int)(ctx->gpr[27] != 0) : -1;
    {
        static int s_var_said = -2;
        if (gun_cam != s_var_said) {
            s_var_said = gun_cam;
            const uint32_t r29 = (uint32_t)ctx->gpr[29];
            fprintf(stderr, "[mouse] mounted gun look variant: %s (r27=%u [r29+0xEC]=%u [r29+0x180]=%u) flip=%u\n",
                    gun_cam < 0 ? "unknown (1.00 layout not read)" : gun_cam ? "0x5F7028 (lr 0x5F829C, from the Camera)"
                                                                         : "0x5F7970 (lr 0x5F827C, stick only)",
                    (unsigned)ctx->gpr[27], ml_ok(r29) ? (unsigned)vm_base[r29 + 0xEC] : 255u,
                    ml_ok(r29) ? (unsigned)vm_base[r29 + 0x180] : 255u, cellGcm_flip_request_count());
        }
    }
    float yaw = (float)ctx->fpr[1], pitch = (float)ctx->fpr[2];
    const float y0 = yaw, p0 = pitch;
    if (moved) {
        pad_mouse_look_apply(pad_mouse_cfg(), dx, dy, 1.0f, 0, lim[2], lim[3], &yaw, &pitch);
        if (lim[0] <= lim[1] && lim[1] - lim[0] < 6.27f) yaw = yaw < lim[0] ? lim[0] : yaw > lim[1] ? lim[1] : yaw;
        else if (lim[0] <= lim[1]) yaw = (float)remainder((double)yaw, 6.283185307179586);   /* a full circle: wrap as the game does */
        ctx->fpr[1] = yaw;
        ctx->fpr[2] = pitch;
    }
    s_lat.take_flip = cellGcm_flip_request_count();
    kz3_latch_trace('T');
    s_lat.mode = 2;
    s_lat.tdy = s_lat.tdp = 0.0f;   /* RSX pass 110: the fill is on foot only (the gun Rep not measured) */
    s_lat.slot_end = 0;
    s_lat.k = pad_mouse_cfg()->sens;
    s_lat.invert = pad_mouse_cfg()->invert;
    s_lat.gun_cam = gun_cam != 0;
    s_lat.basis = ml_ok(res) ? vm_base[res + 0x234] != 0 : 1;
    s_lat.y0 = yaw; s_lat.p0 = pitch;
    s_lat.ymin = lim[1] - lim[0] < 6.27f ? lim[0] : 1.f; s_lat.ymax = lim[1] - lim[0] < 6.27f ? lim[1] : -1.f;
    s_lat.pmin = lim[2]; s_lat.pmax = lim[3];
    if (!moved) return;
    static double s_last_line = -1e9;
    if (pad_mouse_log_each() || now - s_last_line >= 1.0) {
        s_last_line = now;
        LARGE_INTEGER qf, qc;
        QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        fprintf(stderr, "[mouse] mounted flip=%u qpc=%.2f lat=%.1f slot=%.1f d=%d,%d yaw=%.5f->%.5f pitch=%.5f->%.5f "
                        "lim=%.4f,%.4f,%.4f,%.4f\n", cellGcm_flip_request_count(), (double)qc.QuadPart * 1e3 / (double)qf.QuadPart,
                clk ? lat : -1.0, slot, dx, dy, y0, yaw, p0, pitch, lim[0], lim[1], lim[2], lim[3]);
    }
}

/* RSX pass 103 probe (KZ3_MOUSE_LOG=1): every distinct caller of SetViewAngles, once, with what it carried, and a count
 * per caller every 10 s, so a log shows which path turns the local player's view (K4's start: no on-foot call). */
void kz3_view_caller_note(uint32_t lr, uint32_t C, const ppu_context* ctx, uint32_t players)
{
    static uint32_t s_lr[32];
    static uint64_t s_n[32];
    static int s_k;
    static double s_last = 0.0;
    int i = 0;
    while (i < s_k && s_lr[i] != lr) i++;
    if (i == s_k && s_k < 32) {
        s_lr[s_k] = lr; s_n[s_k] = 0; s_k++;
        const uint32_t E = ml_ok(C) ? ml_r32(C + 0x20) : 0, loc = players ? ml_local_entity(players) : 0;
        fprintf(stderr, "[mouse] view caller lr=0x%08X C=0x%08X vtC=0x%08X E=0x%08X vtE=0x%08X local=0x%08X loc.C=0x%08X "
                        "f1=%.5f f2=%.5f C9C=%.5f CA0=%.5f\n", lr, C, ml_ok(C) ? ml_r32(C) : 0, E, ml_ok(E) ? ml_r32(E) : 0, loc,
                ml_ok(loc) ? ml_r32(loc + 0x150) : 0, ctx->fpr[1], ctx->fpr[2], ml_ok(C) ? ml_rf(C + 0x9C) : 0.f,
                ml_ok(C) ? ml_rf(C + 0xA0) : 0.f);
    }
    if (i < 32) s_n[i]++;
    const double now = (double)GetTickCount64() / 1000.0;
    if (now - s_last >= 10.0) {
        s_last = now;
        char b[640];
        int o = 0;
        for (int k = 0; k < s_k && o < 560; k++) o += snprintf(b + o, sizeof b - o, " %06X:%llu", s_lr[k], (unsigned long long)s_n[k]);
        fprintf(stderr, "[mouse] view callers (lr:calls):%s\n", s_k ? b : " none");
    }
}
} // namespace

extern "C" void kz3_mouse_look_pre(void* vctx)
{
    static int s_on = -1;
    if (s_on == 0) return;
    ppu_context* ctx = (ppu_context*)vctx;
    if (s_on < 0) {
        s_on = pad_mouse_enabled() && vm_base;
        if (s_on && kz3_mouse_clock_on()) pad_mouse_set_time_clock(ppu_timebase_now);   /* patch 2108 */
        fprintf(stderr, "[mouse] look clock: %s\n", kz3_mouse_clock_on() ? "game slot (counts up to one slot before the "
                        "slot end; KZ3_MOUSE_CLOCK=0 = wall time)" : "wall time (all counts since the last update; KZ3_MOUSE_CLOCK=0)");
    }
    if (!s_on) return;
    const uint32_t lr = (uint32_t)ctx->lr;
    const uint32_t C = (uint32_t)ctx->gpr[3];
    static const Kz3MouseVer* s_eboot = nullptr;   /* the table whose on-foot call site holds `bl SetViewAngles` */
    if (!s_eboot) for (const Kz3MouseVer* t : {&kMouse100, &kMouse114}) {
        const uint32_t w = ml_r32(t->lr - 4);
        if ((w & 0xFC000003u) == 0x48000001u && (uint32_t)(t->lr - 4 + (((int32_t)(w << 6) >> 6) & ~3)) == t->callee)
            s_eboot = t;
    }
    if (pad_mouse_log_each()) kz3_view_caller_note(lr, C, ctx, s_eboot ? s_eboot->players : 0);
    /* RSX pass 103: the mounted gun (K4 Frozen Shores' start rides one: runs r103-k4p1/p2). */
    if (s_eboot && lr == (s_eboot == &kMouse114 ? kMount114 : kMount100).lr) {
        kz3_mouse_mounted(ctx, s_eboot, s_eboot == &kMouse114 ? &kMount114 : &kMount100);
        return;
    }
    /* RSX pass 103 test switch KZ3_MOUSE_ABS_LR=hex[,hex]: SetViewAngles calls from these return addresses get the mouse
     * added to both arguments (no ADS scale, no clamp), to try a vehicle/turret path before it gets a proper entry. */
    {
        static uint32_t s_abs[8];
        static int s_nabs = -1;
        if (s_nabs < 0) {
            s_nabs = 0;
            const char* e = getenv("KZ3_MOUSE_ABS_LR");
            while (e && *e && s_nabs < 8) { char* x; const unsigned long a = strtoul(e, &x, 16); if (x == e) break;
                                           s_abs[s_nabs++] = (uint32_t)a; e = *x == ',' ? x + 1 : x; }
        }
        for (int i = 0; i < s_nabs; i++)
            if (lr == s_abs[i] && s_eboot) {
                const double tnow = (double)GetTickCount64() / 1000.0;
                int dx = 0, dy = 0;
                uint64_t cut = 0;
                double lat = 0.0, slot = 0.0;
                const int clk = kz3_mouse_clock_on() && kz3_mouse_cut(s_eboot->world, &cut, &lat, &slot);
                if (!(clk ? pad_mouse_take_until(tnow, cut, &dx, &dy) : pad_mouse_take(tnow, &dx, &dy))) return;
                float yaw = (float)ctx->fpr[1], pitch = (float)ctx->fpr[2];
                const float y0 = yaw, p0 = pitch;
                pad_mouse_look_apply(pad_mouse_cfg(), dx, dy, 1.0f, 0, 1.0f, -1.0f, &yaw, &pitch);
                ctx->fpr[1] = yaw;
                ctx->fpr[2] = pitch;
                LARGE_INTEGER qf, qc;
                QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
                fprintf(stderr, "[mouse] abs flip=%u qpc=%.2f lat=%.1f slot=%.1f lr=0x%08X C=0x%08X d=%d,%d yaw=%.5f->%.5f "
                                "pitch=%.5f->%.5f\n", cellGcm_flip_request_count(), (double)qc.QuadPart * 1e3 / (double)qf.QuadPart,
                        clk ? lat : -1.0, slot, lr, C, dx, dy, y0, yaw, p0, pitch);
                return;
            }
    }
    /* RSX pass 93 (hand test 2026-10-21: no "on-foot look update" line in pyrrhus_outskirts): each gate says once why it
     * refused, with what the call carried, so a hand log names the cause. ml_gate_note: once per gate, plus once per
     * distinct return address of a call whose controller belongs to the local player (up to 8). */
    static uint64_t s_gate_n[6];
    auto gate_fail = [&](int g, const char* why, uint32_t a, uint32_t b) {
        if (s_gate_n[g]++ == 0)
            fprintf(stderr, "[mouse] look hook gate %d refused the first time: %s (lr=0x%08X C=0x%08X 0x%08X 0x%08X)\n", g,
                    why, lr, C, a, b);
    };
    { static int s_reached = 0;
      if (!s_reached) { s_reached = 1; fprintf(stderr, "[mouse] look hook reached (SetViewAngles entry, lr=0x%08X)\n", lr); } }
    const Kz3MouseVer* v = lr == kMouse100.lr ? &kMouse100 : lr == kMouse114.lr ? &kMouse114 : nullptr;
    if (!v) {
        /* Not the normal on-foot call. Name the other calls of the local player's controller once each: the locked
         * branch of UpdateLook (0x685F08 / C+0x460 set: the yaw rate comes from C+0x4B4) is the first suspect. */
        static uint32_t s_seen[8];
        static int s_nseen = 0;
        if (ml_ok(C) && s_nseen < 8) {
            const uint32_t E2 = ml_r32(C + 0x20);
            /* the EBOOT's table: the one whose on-foot call site holds `bl SetViewAngles` */
            static const Kz3MouseVer* s_ver = nullptr;
            if (!s_ver)
                for (const Kz3MouseVer* t : {&kMouse100, &kMouse114}) {
                    const uint32_t w = ml_r32(t->lr - 4);
                    if ((w & 0xFC000003u) == 0x48000001u &&
                        (uint32_t)(t->lr - 4 + (((int32_t)(w << 6) >> 6) & ~3)) == t->callee) s_ver = t;
                }
            if (s_ver && ml_ok(E2) && E2 == ml_local_entity(s_ver->players)) {
                int k = 0;
                while (k < s_nseen && s_seen[k] != lr) k++;
                if (k == s_nseen) {
                    s_seen[s_nseen++] = lr;
                    fprintf(stderr, "[mouse] look hook: the local player's controller C=0x%08X calls SetViewAngles from lr=0x%08X "
                                    "(not the on-foot call 0x%08X/0x%08X; C+0x454=0x%08X C+0x460=0x%08X): no mouse there\n",
                            C, lr, kMouse100.lr, kMouse114.lr, ml_r32(C + 0x454), ml_r32(C + 0x460));
                }
            }
        }
        gate_fail(0, "return address is not the on-foot call", 0, 0);
        return;
    }
    /* The call before the return address must be `bl SetViewAngles` (so the table matches the EBOOT). */
    uint32_t ins = ml_r32(lr - 4);
    int32_t off = ((int32_t)(ins << 6) >> 6) & ~3;
    if ((ins & 0xFC000003u) != 0x48000001u || (uint32_t)(lr - 4 + off) != v->callee) {
        gate_fail(1, "the word before lr is not bl SetViewAngles (version table mismatch)", ins, v->callee);
        return;
    }
    if (!ml_ok(C)) { gate_fail(2, "controller pointer invalid", 0, 0); return; }
    const uint32_t E = ml_r32(C + 0x20);
    if (!ml_ok(E)) { gate_fail(3, "controller entity C+0x20 invalid", E, 0); return; }
    if (E != ml_local_entity(v->players)) {
        gate_fail(4, "C+0x20 is not the first local player's entity (E, local)", E, ml_local_entity(v->players));
        return;
    }

    static int s_first = 1;
    if (s_first) {
        s_first = 0;
        /* the RE notes s.12.4 (1), (4), (7): the return address, the pitch limits, Game+0x760 vs the world. */
        fprintf(stderr, "[mouse] on-foot look update: lr=0x%08X C=0x%08X E=0x%08X limits=[%.4f, %.4f] "
                        "game_m=0x%08X world=0x%08X\n", lr, C, E, ml_rf(E + 0x1C4), ml_rf(E + 0x1C8),
                ml_r32(v->game + v->game_m), ml_r32(v->world));
    }
    const double now = (double)GetTickCount64() / 1000.0;
    int dx = 0, dy = 0;
    uint64_t cut = 0;
    double lat_ms = 0.0, slot_ms = 0.0;
    const int on_clock = kz3_mouse_clock_on() && kz3_mouse_cut(v->world, &cut, &lat_ms, &slot_ms);
    const int moved = on_clock ? pad_mouse_take_until(now, cut, &dx, &dy) : pad_mouse_take(now, &dx, &dy);
    static double s_last_move = -1e9, s_last_line = -1e9;
    const int log_each = pad_mouse_log_each();
    const int log_follow = log_each && now - s_last_move < 3.0;   /* the body follows the aim point after a move */
    /* RSX pass 105 late latch: this update's take and the pitch it leaves (k stays the last move's ADS/zoom scale) */
    s_foot_C = C;   /* RSX pass 107 [rep] log: C+0x10C */
    s_lat.take_flip = cellGcm_flip_request_count();
    kz3_latch_trace('T');
    if (s_lat.mode != 1) s_lat.k = pad_mouse_cfg()->sens;
    s_lat.mode = 1;
    s_lat.invert = pad_mouse_cfg()->invert;
    s_lat.gun_cam = 0;
    s_lat.basis = 0;
    s_lat.y0 = 0.0f; s_lat.ymin = 1.0f; s_lat.ymax = -1.0f;   /* on foot: yaw free */
    s_lat.p0 = (float)ctx->fpr[2];
    s_lat.pmin = ml_rf(E + 0x1C4); s_lat.pmax = ml_rf(E + 0x1C8);
    s_lat.tdy = s_lat.tdp = 0.0f;
    {
        const uint32_t W = ml_r32(v->world);
        s_lat.slot_end = ml_ok(W) ? ml_r64(W + 0xE8) : 0;
    }
    if (!moved && !log_follow) return;

    float yaw = (float)ctx->fpr[1], pitch = (float)ctx->fpr[2];
    const float yaw0 = yaw, pitch0 = pitch;
    float ads = 1.0f, pmin = ml_rf(E + 0x1C4), pmax = ml_rf(E + 0x1C8);
    uint32_t weapon = 0, z = 0;
    int zoomed = 0;
    static double s_kdt;
    const double rate0 = ctx->fpr[29];
    if (moved) {
        static ppu_context s_save;   /* the look update runs on one PPU thread; static keeps 1 KB off its host stack */
        s_save = *ctx;
        weapon = ml_call(ctx, v->get_item, E, nullptr);
        if (ml_ok(weapon)) {
            uint32_t vt = ml_r32(weapon);
            if (ml_ok(vt)) {
                uint32_t opd = ml_r32(vt + 0x28C), opdz = ml_r32(vt + 0x380);
                double f1 = 1.0;
                if (ml_ok(opd)) { ml_call(ctx, ml_r32(opd), weapon, &f1); ads = (float)f1; }
                if (ml_ok(opdz)) { z = ml_call(ctx, ml_r32(opdz), weapon, nullptr); zoomed = z == 0; }
            }
        }
        *ctx = s_save;
        /* Zoomed = vf224() == 0, as UpdateLook's r28 (0x684FE0 cntlzw; rlwinm 27,31,31): runs r65-m5/m6, R3 zoom
         * on the starting rifle: vf224 1 -> 0 and vf163 1.0 -> 0.45. */
        pad_mouse_look_apply(pad_mouse_cfg(), dx, dy, ads, zoomed, pmin, pmax, &yaw, &pitch);
        ctx->fpr[1] = yaw;
        ctx->fpr[2] = pitch;
        s_lat.p0 = pitch;
        s_lat.tdy = yaw - yaw0;
        s_lat.tdp = pitch - pitch0;
        s_lat.k = pad_mouse_cfg()->sens * ((ads > 0.0f && isfinite(ads)) ? ads : 1.0f) * (zoomed ? pad_mouse_cfg()->zoom_sens : 1.0f);
        /* The body's turn is not this argument: right after the call UpdateLook stores the yaw RATE f29 in C+0x10C
         * (0x685FA4 `stfs f29,0x10C(r21)`), and the argument was f29 * K * dt + snap with K = res+0x2C0 (r27) * f31
         * (pi/180) and dt = f30 (0x685F84..0x685F9C). f29 is non-volatile, so the lifted body hands our value back to
         * the caller: rate += dYaw / (K * dt) makes the stored rate turn the body by the same dYaw. */
        const uint32_t res = (uint32_t)ctx->gpr[27];
        s_kdt = ml_ok(res) ? (double)ml_rf(res + 0x2C0) * ctx->fpr[31] * ctx->fpr[30] : 0.0;
        if (fabs(s_kdt) > 1e-9 && kz3_mouse_rate_on()) ctx->fpr[29] += (double)(yaw - yaw0) / s_kdt;
        s_last_move = now;
    }
    if (log_each || now - s_last_line >= 1.0) {
        s_last_line = now;
        /* What turned: the basis SetViewAngles rotates (E+0xA0 row 0, E+0xB0 row 1), the entity's world matrix
         * (vf136 = E+0x270 on the Soldier: rows +0x270/+0x280/+0x290, position +0x2A0), and the aim point the last
         * call stored (C+0xA4..0xAC = position + 30 x the view direction). */
        /* RSX pass 102: flip = guest flip requests so far, qpc = ms on the performance counter (GetTickCount64 steps
         * in 10-16 ms on Windows), so a log names how many look updates each presented frame got. */
        LARGE_INTEGER qf, qc;
        QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        fprintf(stderr, "[mouse] flip=%u qpc=%.2f lat=%.1f slot=%.1f t=%.3f d=%d,%d weapon=0x%08X ads=%.4f vf224=%u yaw=%.5f->%.5f pitch=%.5f->%.5f "
                        "rate=%.4f->%.4f kdt=%.6f c10c=%.4f "
                        "b0=%.4f,%.4f,%.4f b1=%.4f,%.4f,%.4f w0=%.4f,%.4f,%.4f w1=%.4f,%.4f,%.4f w2=%.4f,%.4f,%.4f "
                        "pos=%.3f,%.3f,%.3f aim=%.3f,%.3f,%.3f\n", cellGcm_flip_request_count(),
                (double)qc.QuadPart * 1e3 / (double)qf.QuadPart, on_clock ? lat_ms : -1.0, slot_ms, now, dx, dy, weapon, ads, z, yaw0, yaw, pitch0, pitch,
                rate0, ctx->fpr[29], s_kdt, ml_rf(C + 0x10C), ml_rf(E + 0xA0), ml_rf(E + 0xA4), ml_rf(E + 0xA8), ml_rf(E + 0xB0), ml_rf(E + 0xB4), ml_rf(E + 0xB8),
                ml_rf(E + 0x270), ml_rf(E + 0x274), ml_rf(E + 0x278), ml_rf(E + 0x280), ml_rf(E + 0x284),
                ml_rf(E + 0x288), ml_rf(E + 0x290), ml_rf(E + 0x294), ml_rf(E + 0x298), ml_rf(E + 0x2A0),
                ml_rf(E + 0x2A4), ml_rf(E + 0x2A8), ml_rf(C + 0xA4), ml_rf(C + 0xA8), ml_rf(C + 0xAC));
    }
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* KZ3: ps3recomp's second D3D12 path, the live-draw engine (libs/video/rsx_live_draw.c: per-surface depth, MRT,
 * mip chains), opt-in with RSX_LIVE_DRAW=1 for the renderer probe. The null backend opens the window and the
 * engine presents into it; unset, the rsx_d3d12_backend path below is unchanged. Bring-up follows the toolkit's
 * runtime/ppu/tests/boot_main.cpp. */
extern "C" int      rsx_live_draw_enabled(void);
extern "C" int      rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                       const uint8_t* (*guest_ptr)(void*, uint32_t, uint32_t, uint32_t), void* user);
extern "C" void     rsx_live_draw_present(uint32_t buffer_id);
extern "C" void     rsx_live_draw_shutdown(void);
extern "C" int      rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int      rsx_null_backend_pump_messages(void);
extern "C" void*    rsx_null_backend_get_hwnd(void);
extern "C" void     rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
static int s_rsx_live = 0;

/* (location, offset) -> host memory: location 0 = RSX local memory, 1 = main memory through the IO table. */
static const uint8_t* rsx_live_guest_ptr(void*, uint32_t location, uint32_t offset, uint32_t min_bytes)
{
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) ea = cellGcmResolveLocated(1, offset);
    else { ea = cellGcmResolveIO(offset); if (!ea) ea = cellGcmResolveLocated(0, offset); }
    if (!ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

static void kz3_keycaps_register(void);   /* below, with the notice fonts */

/* Display modes (patch 2095, RSX pass 90; ENHANCEMENT): the chosen mode is saved in display.ini beside keybinds.ini
 * ([display] mode = windowed|borderless|fullscreen; rsx_display_mode.h), read here before the window opens, written by
 * the window's thread when the player picks a mode (Options > Display > Display mode, Alt+Enter) or the Image scaling
 * (fit|stretch) / Scaling filter (bilinear|nearest) rows. KZ3_DISPLAY, KZ3_DISPLAY_SCALING, KZ3_DISPLAY_FILTER win for
 * one run, and then nothing is saved (rsx_null_backend.c). */
#include "rsx_display_mode.h"
#ifdef _WIN32
extern "C" int  pad_kb_sibling_path(const wchar_t* name, wchar_t* out, size_t n);   /* cellPad.c (patch 2095) */
extern "C" void rsx_display_configure(int saved_mode, int saved_scaling, int saved_filter,
                                      int saved_vsync, void (*save)(int mode, int scaling, int filter, int vsync));   /* rsx_null_backend.c */
extern "C" int  rsx_display_set_mode(int mode);
extern "C" int  rsx_display_get_mode(void);
extern "C" int  rsx_display_set_scaling(int scaling, int filter);   /* -1 = unchanged */
extern "C" int  rsx_display_get_scaling(void);
extern "C" int  rsx_display_get_filter(void);
extern "C" int  rsx_display_set_vsync(int on);   /* patch 2103: Options > Display > VSync */
extern "C" int  rsx_display_get_vsync(void);

/* Patch 2107 (RSX pass 102): Motion blur and the frame rate limit, display.ini motion_blur / fps_limit beside the window
 * settings. The window's thread saves through kz3_display_save (its four values); the two rows here save through it
 * too, with the last saved window values. *_eff = in force this run (KZ3_MOTION_BLUR / KZ3_FPS_LIMIT win for one run),
 * *_saved = what the file holds. */
/* Patch 2121 (RSX pass 114): Motion blur and Bloom are percentages, 0..100 in steps of 10 (100 = the game's look; the
 * values the rows hold are s_mb_* / s_bloom_*; KZ3_MOTION_BLUR on|off|0..100 and KZ3_BLOOM 0..100 win for one run). */
static volatile LONG s_mb_eff = 100, s_fps_eff = RSX_DM_FPS_30, s_bloom_eff = 100;
static int s_mb_saved = 100, s_fps_saved = RSX_DM_FPS_30, s_bloom_saved = 100;
/* Test aid (patch 2121): KZ3_BLOOM_SEQ / KZ3_MOTION_BLUR_SEQ = "T:V,T:V,..." switch the strength to V (0..100) from T seconds
 * after the process start (the pad script's clock), so one run, with the game paused, shows one frozen frame at several
 * strengths. Only read when set; the rows and the ini are not touched. */
struct Kz3Seq { int n; double t[12]; int v[12]; };
static Kz3Seq s_bloom_seq, s_mb_seq;
static ULONGLONG s_seq_t0;
static int s_mb_diag;   /* KZ3_MOTION_BLUR set: the MotionBlur job follows it (off garbles the picture) */
static void kz3_seq_parse(const char* e, Kz3Seq* q)
{
    q->n = 0;
    while (e && *e && q->n < 12) {
        char* end = nullptr;
        const double t = strtod(e, &end);
        if (end == e || *end != ':') break;
        const long v = strtol(end + 1, &end, 10);
        q->t[q->n] = t; q->v[q->n] = v < 0 ? 0 : v > 100 ? 100 : (int)v; q->n++;
        if (*end != ',') break;
        e = end + 1;
    }
}
static int kz3_seq_now(const Kz3Seq* q, int dflt)
{
    if (!q->n) return dflt;
    const double now = (double)(GetTickCount64() - s_seq_t0) / 1000.0;
    int r = dflt;
    for (int i = 0; i < q->n; i++) if (now >= q->t[i]) r = q->v[i];
    return r;
}
/* Patch 2109 (RSX pass 103): Field of view, display.ini fov (0 = default = the game's own; KZ3_FOV for one run). */
static volatile LONG s_fov_deg;   /* in force */
static int s_fov_saved;
static int s_fov_ok;              /* the camera hook is installed (the Options row shows only then) */
static int s_h169_saved = 1;      /* patch 2113: display.ini hud_169 (2D layer kept 16:9), default on */
extern "C" void rsx_live_draw_set_hud169(int on);   /* rsx_live_draw.c */
static int s_dm_saved[4] = { RSX_DM_WINDOWED, RSX_DM_STRETCH, RSX_DM_BILINEAR, RSX_DM_VSYNC_ON };
static CRITICAL_SECTION s_dm_save_lock;
static INIT_ONCE s_dm_save_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK kz3_dm_lock_init(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&s_dm_save_lock); return TRUE; }

static void kz3_display_save(int mode, int scaling, int filter, int vsync)
{
    wchar_t p[1100];
    char t[2048];
    InitOnceExecuteOnce(&s_dm_save_once, kz3_dm_lock_init, NULL, NULL);
    EnterCriticalSection(&s_dm_save_lock);
    s_dm_saved[0] = mode; s_dm_saved[1] = scaling; s_dm_saved[2] = filter; s_dm_saved[3] = vsync;
    const int n = rsx_dm_ini_write_ext4(t, sizeof t, mode, scaling, filter, vsync, s_mb_saved, s_fps_saved, s_fov_saved,
                                        s_h169_saved, s_bloom_saved);
    char fb[16];
    bool ok = false;
    if (pad_kb_sibling_path(L"display.ini", p, 1100)) {
        FILE* f = _wfopen(p, L"wb");
        ok = f && fwrite(t, 1, (size_t)n, f) == (size_t)n;
        if (f) fclose(f);
        printf("[display] %ls: mode %s, scaling %s, filter %s, vsync %s, motion blur %d, bloom %d, fps limit %s, fov %s %s\n", p,
               rsx_dm_name(mode), rsx_dm_scaling_name(scaling), rsx_dm_filter_name(filter), rsx_dm_vsync_name(vsync),
               s_mb_saved, s_bloom_saved, rsx_dm_fps_name(s_fps_saved), rsx_dm_fov_name(s_fov_saved, fb, sizeof fb),
               ok ? "saved" : "NOT saved");
    }
    LeaveCriticalSection(&s_dm_save_lock);
    fflush(stdout);
}
/* The rows (kb:motionblur, kb:fps): applied at once, saved with the window values last saved or read. */
static void kz3_display_save_extra(void)
{
    kz3_display_save(s_dm_saved[0], s_dm_saved[1], s_dm_saved[2], s_dm_saved[3]);
}

static void kz3_display_load(void)
{
    wchar_t p[1100];
    char t[2048];
    int mode = -1, scaling = -1, filter = -1, vsync = -1;
    if (pad_kb_sibling_path(L"display.ini", p, 1100)) {
        FILE* f = _wfopen(p, L"rb");
        if (f) {
            const size_t n = fread(t, 1, sizeof t - 1, f);
            fclose(f);
            t[n] = 0;
            mode = rsx_dm_ini_parse(t);
            scaling = rsx_dm_ini_scaling(t);
            filter = rsx_dm_ini_filter(t);
            vsync = rsx_dm_ini_vsync(t);
            const int mb = rsx_dm_ini_motion_blur(t), fps = rsx_dm_ini_fps(t);
            if (mb >= 0) s_mb_saved = mb;
            const int bl = rsx_dm_ini_bloom(t);   /* patch 2121 */
            if (bl >= 0) s_bloom_saved = bl;
            if (fps > 0) s_fps_saved = fps;
            const int fv = rsx_dm_ini_fov(t);   /* patch 2109 */
            if (fv >= 0) s_fov_saved = fv;
            const int hv = rsx_dm_ini_hud169(t);   /* patch 2113 */
            if (hv >= 0) s_h169_saved = hv;
            printf("[display] %ls: mode %s, scaling %s, filter %s, vsync %s, motion blur %d, fps limit %s\n", p,
                   mode >= 0 ? rsx_dm_name(mode) : "(none)", scaling >= 0 ? rsx_dm_scaling_name(scaling) : "(none)",
                   filter >= 0 ? rsx_dm_filter_name(filter) : "(none)", vsync >= 0 ? rsx_dm_vsync_name(vsync) : "(none)",
                   mb, fps > 0 ? rsx_dm_fps_name(fps) : "(none)");
        }
    }
    if (mode >= 0) s_dm_saved[0] = mode;
    if (scaling >= 0) s_dm_saved[1] = scaling;
    if (filter >= 0) s_dm_saved[2] = filter;
    if (vsync >= 0) s_dm_saved[3] = vsync;
    /* patch 2107: one-run overrides (tests); the rows still save their own choice */
    const char* e = getenv("KZ3_MOTION_BLUR");
    const int mbe = e && *e ? rsx_dm_parse_pct(e) : -1;
    e = getenv("KZ3_BLOOM");
    const int ble = e && *e ? rsx_dm_parse_pct(e) : -1;
    s_seq_t0 = GetTickCount64();
    kz3_seq_parse(getenv("KZ3_BLOOM_SEQ"), &s_bloom_seq);
    kz3_seq_parse(getenv("KZ3_MOTION_BLUR_SEQ"), &s_mb_seq);
    if (s_bloom_seq.n || s_mb_seq.n) printf("[display] test sequence: bloom %d steps, motion blur %d steps\n", s_bloom_seq.n, s_mb_seq.n);
    InterlockedExchange(&s_bloom_eff, ble >= 0 ? ble : s_bloom_saved);
    e = getenv("KZ3_FPS_LIMIT");
    const int fpe = e && *e ? rsx_dm_parse_fps(e) : -1;
    /* Patch 2121: the saved motion_blur value is kept (read and written back) but does not act: skipping the post chain's
     * MotionBlur job garbles the picture (RSX pass 114, K4: 2 of 2 runs with the skip show false-colour frames, 2 of 2
     * without it are normal), so there is no row and only the diagnostic KZ3_MOTION_BLUR / KZ3_MOTION_BLUR_SEQ switch it. */
    s_mb_diag = mbe >= 0;
    InterlockedExchange(&s_mb_eff, mbe >= 0 ? mbe : 100);
    InterlockedExchange(&s_fps_eff, fpe > 0 ? fpe : s_fps_saved);
    e = getenv("KZ3_FOV");
    const int fve = e && *e ? rsx_dm_parse_fov(e) : -1;
    InterlockedExchange(&s_fov_deg, fve >= 0 ? fve : s_fov_saved);
    { char fb[16]; printf("[display] field of view %s%s\n", rsx_dm_fov_name((int)s_fov_deg, fb, sizeof fb), fve >= 0 ? " (KZ3_FOV)" : ""); }
    printf("[display] motion blur %s%s, bloom %d%s, fps limit %s%s\n", mbe >= 0 ? (mbe ? "on" : "OFF (diagnostic: garbles the picture)") : "always on (no row)", mbe >= 0 ? " (KZ3_MOTION_BLUR)" : "",
           (int)s_bloom_eff, ble >= 0 ? " (KZ3_BLOOM)" : "",
           rsx_dm_fps_name((int)s_fps_eff), fpe > 0 ? " (KZ3_FPS_LIMIT)" : "");
    rsx_live_draw_set_hud169(s_h169_saved);   /* patch 2113; KZ3_HUD_169 wins for one run (rsx_live_draw.c) */
    rsx_display_configure(mode, scaling, filter, vsync, kz3_display_save);
}
#endif

/* Patch 2107: is the post chain's MotionBlur job wanted (Options > Display > Motion blur)? Asked per frame by the
 * MotionBlur wrapper's override (@0x001FDA60, 1.14 @0x002093B0; port/spu/kz3_ppu_overrides.cpp), which also calls
 * kz3_motion_blur_override_register from a static initialiser, so the Options row appears only in an exe that has it. */
static int s_mb_override = 0;
/* Patch 2121: called by the MotionBlur wrapper override's static initialiser (SPU's file). It no longer shows the
 * Options row: no clean off exists yet (see kz3_display_load). Set s_mb_override = 1 here once there is one. */
extern "C" void kz3_motion_blur_override_register(void) { }
extern "C" int kz3_motion_blur_on(void)
{
#ifdef _WIN32
    return !s_mb_diag && !s_mb_seq.n ? 1 : kz3_seq_now(&s_mb_seq, (int)s_mb_eff) != 0;   /* patch 2121: diagnostics only */
#else
    return 1;
#endif
}

/* Patch 2121: Bloom strength. BuildPost (1.14 @0x00207248, 1.00 @0x001FB8F8; the RE notes s.51.2) stores
 * vp+0x230 = BloomSettings.Strength / 9 per view per frame (vp = r5); the SPU job BloomIntegrate reads that float by
 * address when it runs. The override (CMake defines KZ3_BLOOM_OVERRIDE when the lift announces it; the lift's override
 * table needs the BuildPost row) calls the original and scales the stored float by bloom / 100; nothing at 100. */
static int s_bloom_override = 0;
extern "C" void kz3_bloom_override_register(void) { s_bloom_override = 1; }
#ifdef KZ3_BLOOM_OVERRIDE
[[maybe_unused]] static const int s_kz3_bloom_reg = (kz3_bloom_override_register(), 1);
static void kz3_bloom_body(ppu_context* ctx, void (*orig)(ppu_context*), uint32_t addr)
{
    const uint32_t vp = (uint32_t)ctx->gpr[5];
    orig(ctx);
#ifdef _WIN32
    const int k = kz3_seq_now(&s_bloom_seq, (int)s_bloom_eff);
#else
    const int k = 100;
#endif
    if (k >= 100 || !vp) return;
    uint8_t* q = vm_base + vp + 0x230;   /* big-endian guest float */
    uint32_t w = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
    float f;
    memcpy(&f, &w, 4);
    const float before = f;
    f = rsx_dm_pct_scale(f, k);
    memcpy(&w, &f, 4);
    q[0] = (uint8_t)(w >> 24); q[1] = (uint8_t)(w >> 16); q[2] = (uint8_t)(w >> 8); q[3] = (uint8_t)w;
    static int s_logged = 0;
    if (s_logged++ < 3) fprintf(stderr, "[bloom] vp 0x%08X +0x230 %g -> %g (%d %%, @0x%08X)\n", vp, before, f, k, addr);
}
#ifdef KZ3_BLOOM_OVERRIDE_114
void func_00207248__lifted(ppu_context* ctx);
void func_00207248(ppu_context* ctx) { kz3_bloom_body(ctx, func_00207248__lifted, 0x00207248u); }
#endif
#ifdef KZ3_BLOOM_OVERRIDE_100
void func_001FB8F8__lifted(ppu_context* ctx);
void func_001FB8F8(ppu_context* ctx) { kz3_bloom_body(ctx, func_001FB8F8__lifted, 0x001FB8F8u); }
#endif
#endif

static int kz3_rsx_init(uint32_t w, uint32_t h, const char* title)
{
#ifdef _WIN32
    kz3_display_load();   /* patch 2095 */
    if (rsx_live_draw_enabled()) {
        if (rsx_null_backend_init(w, h, title) == 0 &&
            rsx_live_draw_init(rsx_null_backend_get_hwnd(), w, h, rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            kz3_keycaps_register();   /* patch 2039: keycap prompts */
            fprintf(stderr, "[rsx] live-draw engine up (RSX_LIVE_DRAW=1)\n");
            return 0;
        }
        fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the D3D12 backend\n");
    }
#endif
    return rsx_backend_init(w, h, title);
}
static void kz3_rsx_present(void)
{
    if (s_rsx_live) rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
    else            rsx_backend_present();
}
static int kz3_rsx_pump(void) { return s_rsx_live ? rsx_null_backend_pump_messages() : rsx_backend_pump(); }

/* Window closed (X button, Alt+F4, a scripted WM_CLOSE such as tools/release/shaders.py's; RSX pass 35): end the
 * process cleanly from the frame thread, which owns the window and the live-draw walker. Live-draw shutdown first
 * (GPU drain, PSO preload workers joined, pso_library.bin serialized), then the log is flushed and closed, then the
 * process ends with code 0. TerminateProcess rather than exit(): guest PPU/SPU threads are still running and may
 * hold CRT or loader locks, which exit()'s handlers and DLL detach would wait on. A watchdog ends the process with
 * code 3 if the shutdown itself hangs (KZ3_CLOSE_WATCHDOG=<s>, default 15). Before this the frame loop only
 * stopped presenting and kz3.exe kept running without a window. */
static DWORD WINAPI kz3_close_watchdog(LPVOID)
{
    const char* e = getenv("KZ3_CLOSE_WATCHDOG");
    const int s = (e && atoi(e) > 0) ? atoi(e) : 15;
    Sleep((DWORD)s * 1000u);
    KZ3_LOGE("exit", "window-close shutdown did not finish in %d s: terminating (exit code 3)", s);
    kz3_log_shutdown();
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}
static void kz3_exit_on_window_close(void)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    KZ3_LOGI("exit", "window closed: shutting down (live draw %s)", s_rsx_live ? "on" : "off");
    HANDLE wd = CreateThread(NULL, 0, kz3_close_watchdog, NULL, 0, NULL);
    (void)wd;
    if (s_rsx_live) rsx_live_draw_shutdown();
    QueryPerformanceCounter(&t1);
    KZ3_LOGI("exit", "clean exit after %.0f ms (exit code 0)", (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)f.QuadPart);
    kz3_log_shutdown();
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 0);
}

/* KZ3_TEST_CLOSE=<s>: after s seconds, post WM_CLOSE to every top-level window of this process, as
 * tools/release/shaders.py's close_politely does from outside (Proton test of the clean exit, RSX pass 35). */
static BOOL CALLBACK kz3_test_close_enum(HWND h, LPARAM n)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) { PostMessageW(h, WM_CLOSE, 0, 0); ++*(int*)n; }
    return TRUE;
}
static DWORD WINAPI kz3_test_close(LPVOID)
{
    const char* e = getenv("KZ3_TEST_CLOSE");
    Sleep((DWORD)(atoi(e) * 1000));
    kz3_log_set_thread_label("test");
    int n = 0;
    EnumWindows(kz3_test_close_enum, (LPARAM)&n);
    KZ3_LOGI("test", "KZ3_TEST_CLOSE=%s: WM_CLOSE posted to %d window(s)", e, n);
    return 0;
}

static void present_guest_frame(void)
{
    kz3_rsx_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

/* Patch 2107: the vblank tick for the frame rate limit (rsx_display_mode.h): 16 ms for 30 (as before), 8 for 60, 4 for 120. */
static unsigned kz3_vblank_tick_ms(void)
{
#ifdef _WIN32
    return rsx_dm_fps_vblank_ms((int)s_fps_eff);
#else
    return 16;
#endif
}

static DWORD WINAPI frame_clock(LPVOID)
{
    kz3_log_set_thread_label("frame");
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Project Helghast";

    int rsx_ok = (kz3_rsx_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();
    /* Patch 2107: the frame rate limit (Options > Display, display.ini fps_limit; ENHANCEMENT). What holds KZ3 at 30
     * is the guest itself: its vblank handler 0x64A1C sets RSX label 72 on every 2nd vblank (the RE notes
     * s.2) and every frame's command stream waits on label 72 (NV406E 0x064 = 0x480, the RE notes pass 15), so a
     * frame is released every 2nd vblank: the vblank rate decides the cap (RPCS3 players: "Vblank 120 Hz allows
     * 60 FPS"). 30 = this ticker's 16 ms (62.5 Hz, unchanged), 60 = 8 ms, 120 = 4 ms. The game clock, logic slots and
     * physics run on the timebase, not on vblanks (the RE notes s.6-7). */
    for (;;) {
        const unsigned tick_ms = kz3_vblank_tick_ms();
        Sleep(tick_ms < 16 ? 1 : 4);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += tick_ms;        /* ~60 Hz (patch 2107: 120 / 240 Hz for the 60 / 120 limits) */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            if (kz3_rsx_pump() != 0) {
                kz3_exit_on_window_close();   /* does not return */
                rsx_ok = 0;
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                /* Consume the pending flip too, or the next pass presents the same
                 * flip again (RSX pass 6: 3,904 presents for 2,483 flips). */
                cellGcm_take_flip_pending();
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                kz3_rsx_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* The directory holding PS3_GAME, from the EBOOT path (kz3_vfs_root.h). $PS3_VFS_ROOT (kz3.ini vfs_root) overrides. */
static char s_vfs_root[1024];

/* Read-only game root / writable root (release round KZ3-03): ppu_vfs_root is where /dev_bdvd and /app_home read (the
 * dump, in place, no link); KZ3_CACHE_ROOT (kz3.ini cache_root) is where this port writes: psarc-cache, the UI pages,
 * /dev_hdd1 (-> <cache>/cache) and, unless PS3_HDD0_ROOT is set, /dev_hdd0 (-> the working folder's gamedata/dev_hdd0,
 * where the trophies already land). Unset: everything stays under ppu_vfs_root as before. */
static void kz3_setenv(const char* k, const char* v);
static const char* kz3_cache_root(void)
{
    const char* e = getenv("KZ3_CACHE_ROOT");
    return (e && *e) ? e : ppu_vfs_root;
}

static void kz3_split_roots(void)
{
    const char* e = getenv("KZ3_CACHE_ROOT");
    if (!e || !*e) return;
    std::string c = std::string(e) + "/cache";
    for (char& ch : c) if (ch == '\\') ch = '/';
    if (!getenv("PS3_HDD1_ROOT") || !*getenv("PS3_HDD1_ROOT")) kz3_setenv("PS3_HDD1_ROOT", c.c_str());
    if (!getenv("PS3_HDD0_ROOT") || !*getenv("PS3_HDD0_ROOT")) kz3_setenv("PS3_HDD0_ROOT", "gamedata/dev_hdd0");
#ifdef _WIN32
    _mkdir(e);
    _mkdir(c.c_str());
#else
    mkdir(e, 0755);
    mkdir(c.c_str(), 0755);
#endif
    printf("[boot] cache root (all writes): %s\n", e);
}

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    const char* how = "";
    std::string r = kz3_game_root(eboot, [](const std::string& p) {
#ifdef _WIN32
        DWORD a = GetFileAttributesA(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
        struct stat st;
        return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }, &how);
    strncpy(s_vfs_root, r.c_str(), sizeof s_vfs_root - 1);
    ppu_vfs_root = s_vfs_root;
    printf("[boot] game root %s (%s)\n", s_vfs_root, how);
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Boot diagnostics (opt-in, environment)
 * -----------------------------------------------------------------------*/
/* KZ3_WATCHDOG="s1,s2,...": at each of those seconds after start, print which
 * HLE call every guest thread is inside and each thread's guest lr. A thread
 * parked in sys_lwmutex_lock / sys_lwcond_wait is invisible to the sampler
 * (it shows as ntdll); this names it. Taken from the toolkit's boot_main.cpp
 * watchdog, minus the thread suspension. KZ3_PEEK="EA[+off],..." adds guest
 * words to each report: "0x0159493C+8" prints the u32 at EA and the u32 at
 * (that value + 8), i.e. one pointer hop; "EA+o1+o2+..." follows up to 8 hops (each reads the u32 at the
 * previous value + offset, a null stops it). PS3_SAMPLE=<ms> starts the toolkit's
 * guest-PC sampler as well. */
extern "C" const char* g_hle_inflight[];      /* ppu_hle.cpp; 64 entries */
extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
extern "C" void        ppu_report_guest_lrs(void);
extern void            ps3_sampler_start(void);

static DWORD WINAPI kz3_watchdog(LPVOID p)
{
    const char* spec = (const char*)p;
    ULONGLONG t0 = GetTickCount64();
    for (const char* s = spec; *s; ) {
        unsigned at = (unsigned)strtoul(s, (char**)&s, 10);
        while (*s == ',' || *s == ' ') s++;
        ULONGLONG due = t0 + (ULONGLONG)at * 1000;
        ULONGLONG now = GetTickCount64();
        if (due > now) Sleep((DWORD)(due - now));
        fprintf(stderr, "[WATCHDOG] t=%us; last HLE call = 0x%08X (%s)\n",
                at, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
        for (unsigned t = 0; t < 64u; t++)
            if (g_hle_inflight[t])
                fprintf(stderr, "[WATCHDOG]   guest-tid %-3u in %s\n", t, g_hle_inflight[t]);
        ppu_report_guest_lrs();
        if (const char* pk = getenv("KZ3_PEEK")) {
            for (const char* q = pk; *q; ) {
                uint32_t ea = (uint32_t)strtoul(q, (char**)&q, 0), off = 0; int hop = 0;
                uint32_t offs[8]; int n = 0;
                while (*q == '+') {
                    off = (uint32_t)strtoul(q + 1, (char**)&q, 0);
                    if (n < 8) offs[n++] = off;
                }
                hop = n > 0;
                while (*q == ',' || *q == ' ') q++;
                uint32_t v = __builtin_bswap32(*(uint32_t*)(vm_base + ea));
                if (n > 1) {
                    /* "EA+o1+o2+...": every hop reads the u32 at (previous value + offset); a null pointer
                     * stops the chain (RE pass 151: the loading manager and the async-job byte sit 2-4 hops deep). */
                    fprintf(stderr, "[WATCHDOG]   peek [0x%08X]=0x%08X", ea, v);
                    for (int k = 0; k < n; k++) {
                        if (v == 0) { fprintf(stderr, " +0x%X=null", offs[k]); break; }
                        v = __builtin_bswap32(*(uint32_t*)(vm_base + (uint32_t)(v + offs[k])));
                        fprintf(stderr, " +0x%X=0x%08X", offs[k], v);
                    }
                    fprintf(stderr, "\n");
                }
                else if (hop) fprintf(stderr, "[WATCHDOG]   peek [0x%08X]=0x%08X  [0x%08X+0x%X]=0x%08X\n", ea, v, v, off,
                                      __builtin_bswap32(*(uint32_t*)(vm_base + (uint32_t)(v + off))));
                else          fprintf(stderr, "[WATCHDOG]   peek [0x%08X]=0x%08X\n", ea, v);
            }
        }
        fflush(stderr);
    }
    return 0;
}

extern "C" int kz3_psarc_vfs_init(const char* archive, const char* cache_dir);  /* psarc_vfs.cpp */
extern "C" void kz3_hle_register(void);                                         /* hle_kz3.cpp */

/* ---------------------------------------------------------------------------
 * Keyboard page (hooks A and B, RSX pass 37; ENHANCEMENT)
 *
 * A: the page's Lua talks to the host through two PlayerProfile script methods no Lua or GUM file calls
 *    (the RE notes 8.2): SetTimeZone(s) and GetTimeZone()->s. Their OPD targets are re-registered in the
 *    indirect-call map (ppu_register_function overwrites), so only calls through the method table (the Lua binder)
 *    reach the host functions; direct `bl` callers keep the lifted bodies. A string starting with "kb:" is a page
 *    command (pad_kb_ui_cmd, cellPad.c patch 2038); its reply is what the next GetTimeZone() returns. Anything else
 *    goes to the original body, so the profile's time zone keeps working. Binder convention (verified static on
 *    1.00: SetTimeZone 0x4D1A08 strcpy's r4 into this+0xE94; GetTimeZone 0xBA5744 / GetClanTag 0xBA5698 return
 *    `this + field` in r3): r3 = this, r4 = first argument, a string is a guest char*, a string return is a guest
 *    char* in r3. The reply lives in a host-owned guest buffer above the GCM window (ASSUMPTION: nothing else uses
 *    inject_base + 0x80000).
 * B: the page itself (generated here: the same widgets as CONTROLLERSETTINGS.GUM) and a "Keyboard" row after the
 *    controller row of the four option menus (patched from the player's own archive at open time), both served by
 *    a miss hook chained in front of psarc_vfs.cpp's.
 * KZ3_KB_PAGE=0 turns both off (and the main menu EXIT GAME row, pass 61); KZ3_KB_PAGE_TEST=1 also serves the page in place of the main menu (tests: Xvfb
 * runs reach it with one CROSS on the title screen).
 * -----------------------------------------------------------------------*/
extern "C" int  pad_kb_ui_cmd(const char* cmd, char* out, size_t n);           /* cellPad.c (patch 2038) */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));  /* ppu_loader.cpp */
extern "C" void (*g_ps3_vfs_miss_hook)(char* path, size_t cap);                 /* sys_fs.c (patch 0004) */

static void (*s_kb_set_orig)(ppu_context*) = nullptr;
static void (*s_kb_get_orig)(ppu_context*) = nullptr;
static char s_kb_reply[1024];
static int  s_kb_pending = 0;
static uint32_t s_kb_buf = 0;                 /* guest EA of the reply buffer */

/* EXIT GAME (main menu, RSX pass 61, ENHANCEMENT): the confirmed row ends the game the way the window's X button
 * does: WM_CLOSE to this process's windows, so the frame thread runs kz3_exit_on_window_close (live-draw shutdown,
 * log flush, exit code 0). Posted, not called: the Lua bridge runs on a guest PPU thread. */
static void kz3_exit_game_request(void)
{
    int n = 0;
    EnumWindows(kz3_test_close_enum, (LPARAM)&n);
    KZ3_LOGI("exit", "EXIT GAME / QUIT TO DESKTOP confirmed: WM_CLOSE posted to %d window(s)", n);
}

static uint32_t kz3_be32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
[[maybe_unused]] static float kz3_bef(uint32_t ea) { const uint32_t v = kz3_be32(ea); float f; memcpy(&f, &v, 4); return f; }

/* Options > Mouse (RSX pass 96, patch 2101; ENHANCEMENT): "kb:mouse sens|ads [v]", "kb:mouse invert [true|false]",
 * "kb:mouse reset" from the page's Lua (mousesettings.gum, kz3_mouse_page_gum). A value applies to the next look
 * update (pad_mouse_opts_set) and is written to kz3.ini [input] (mouse_sensitivity, mouse_ads_sensitivity,
 * mouse_invert_y). A KZ3_MOUSE_* variable set in the environment still wins at the next start (kz3.ini rule). */
static void kz3_mouse_opts_save(void);
static void kz3_mouse_opts_cmd(const char* a, char* reply, size_t cap)
{
    float u, z; int inv;
    pad_mouse_opts_get(&u, &z, &inv);
    const bool sens = !strncmp(a, "sens", 4), ads = !strncmp(a, "ads", 3), invert = !strncmp(a, "invert", 6);
    const char* v = a;
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    bool set = false;
    if (!strncmp(a, "reset", 5)) {
        u = PAD_MOUSE_USER_SENS_DEF; z = 1.0f; inv = 0; set = true;
    } else if (*v && sens) {
        u = pad_mouse_opt_clamp(strtof(v, nullptr), u); set = true;
    } else if (*v && ads) {
        z = pad_mouse_opt_clamp(strtof(v, nullptr), z); set = true;
    } else if (*v && invert) {
        inv = !strcmp(v, "true") || !strcmp(v, "1"); set = true;
    }
    if (set) {
        pad_mouse_opts_set(u, z, inv);
        pad_mouse_opts_get(&u, &z, &inv);
        kz3_mouse_opts_save();
    }
    if (invert) snprintf(reply, cap, "%s", inv ? "true" : "false");
    else pad_mouse_opt_fmt(reply, cap, ads ? z : u);
}

static void kz3_kb_set(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[4];
    if (ea >= 0x10000u && ea < 0xF0000000u && !memcmp(vm_base + ea, "kb:", 3)) {
        char cmd[128];
        size_t i = 0;
        for (; i < sizeof cmd - 1 && vm_base[ea + 3 + i]; i++) cmd[i] = (char)vm_base[ea + 3 + i];
        cmd[i] = 0;
        if (!strncmp(cmd, "mouse", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {   /* Options > Mouse rows (patch 2101) */
            kz3_mouse_opts_cmd(cmd + (cmd[5] ? 6 : 5), s_kb_reply, sizeof s_kb_reply);
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
#ifdef _WIN32
        if (!strncmp(cmd, "display", 7) && (cmd[7] == 0 || cmd[7] == ' ')) {   /* Options > Display row (patch 2095) */
            const int m = cmd[7] ? rsx_dm_parse(cmd + 8) : -1;
            if (m >= 0) rsx_display_set_mode(m);      /* applied on the window's thread, which also saves it */
            snprintf(s_kb_reply, sizeof s_kb_reply, "%s", rsx_dm_name(m >= 0 ? m : rsx_display_get_mode()));
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if (!strncmp(cmd, "vsync", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {   /* VSync row (patch 2103) */
            const int v = cmd[5] ? rsx_dm_parse_vsync(cmd + 6) : -1;
            if (v >= 0) rsx_display_set_vsync(v);     /* applied and saved on the window's thread */
            snprintf(s_kb_reply, sizeof s_kb_reply, "%s", rsx_dm_vsync_name(v >= 0 ? v : rsx_display_get_vsync()));
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if (!strncmp(cmd, "motionblur", 10) && (cmd[10] == 0 || cmd[10] == ' ')) {   /* Motion blur row (patch 2107) */
            const int v = cmd[10] ? rsx_dm_parse_pct(cmd + 11) : -1;
            if (v >= 0) {
                InterlockedExchange(&s_mb_eff, v);
                s_mb_saved = v;
                kz3_display_save_extra();
            }
            snprintf(s_kb_reply, sizeof s_kb_reply, "%d", (int)s_mb_eff);
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if (!strncmp(cmd, "bloom", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {   /* Bloom row (patch 2121) */
            const int v = cmd[5] ? rsx_dm_parse_pct(cmd + 6) : -1;
            if (v >= 0) {
                InterlockedExchange(&s_bloom_eff, v);
                s_bloom_saved = v;
                kz3_display_save_extra();
            }
            snprintf(s_kb_reply, sizeof s_kb_reply, "%d", (int)s_bloom_eff);
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if (!strncmp(cmd, "fov", 3) && (cmd[3] == 0 || cmd[3] == ' ')) {   /* Field of view row (patch 2109) */
            const int v = cmd[3] ? rsx_dm_parse_fov(cmd + 4) : -1;
            if (v >= 0) {
                InterlockedExchange(&s_fov_deg, v);
                s_fov_saved = v;
                kz3_display_save_extra();
            }
            char fb[16];
            snprintf(s_kb_reply, sizeof s_kb_reply, "%s", rsx_dm_fov_name((int)s_fov_deg, fb, sizeof fb));
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if (!strncmp(cmd, "fps", 3) && (cmd[3] == 0 || cmd[3] == ' ')) {   /* Frame rate limit row (patch 2107) */
            const int v = cmd[3] ? rsx_dm_parse_fps(cmd + 4) : -1;
            if (v > 0) {
                InterlockedExchange(&s_fps_eff, v);
                s_fps_saved = v;
                kz3_display_save_extra();
            }
            snprintf(s_kb_reply, sizeof s_kb_reply, "%s", rsx_dm_fps_name((int)s_fps_eff));
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
        if ((!strncmp(cmd, "scaling", 7) && (cmd[7] == 0 || cmd[7] == ' ')) ||
            (!strncmp(cmd, "filter", 6) && (cmd[6] == 0 || cmd[6] == ' '))) {   /* Image scaling / Scaling filter rows */
            const bool sc = cmd[0] == 's';
            const char* v = cmd + (sc ? 7 : 6);
            const int x = *v ? (sc ? rsx_dm_parse_scaling(v + 1) : rsx_dm_parse_filter(v + 1)) : -1;
            if (x >= 0) rsx_display_set_scaling(sc ? x : -1, sc ? -1 : x);
            snprintf(s_kb_reply, sizeof s_kb_reply, "%s",
                     sc ? rsx_dm_scaling_name(x >= 0 ? x : rsx_display_get_scaling())
                        : rsx_dm_filter_name(x >= 0 ? x : rsx_display_get_filter()));
            s_kb_pending = 1;
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
            return;
        }
#endif
        if (!strcmp(cmd, "exit")) {   /* main menu EXIT GAME, confirmed (RSX pass 61) */
            kz3_exit_game_request();
            snprintf(s_kb_reply, sizeof s_kb_reply, "ok");
            s_kb_pending = 1;
            return;
        }
        pad_kb_ui_cmd(cmd, s_kb_reply, sizeof s_kb_reply);
        s_kb_pending = 1;
        static int n = 0;
        if (n++ < 6 || (strncmp(cmd, "key", 3) && strncmp(cmd, "poll", 4) && strncmp(cmd, "status", 6) && strncmp(cmd, "label", 5)))
            printf("[kb-page] '%s' -> '%s'\n", cmd, s_kb_reply);
        return;
    }
    if (s_kb_set_orig) s_kb_set_orig(ctx);
}

static void kz3_kb_get(ppu_context* ctx)
{
    if (s_kb_pending) {
        size_t n = strlen(s_kb_reply);
        s_kb_pending = 0;
        memcpy(vm_base + s_kb_buf, s_kb_reply, n + 1);
        ctx->gpr[3] = s_kb_buf;
        return;
    }
    if (s_kb_get_orig) s_kb_get_orig(ctx);
}

static void (*kz3_lifted_fn(uint32_t addr))(ppu_context*)
{
    for (uint64_t i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) return function_table[i].func;
    return nullptr;
}

/* The bridge on 1.00 or 1.14 (script_methods.py on each EBOOT): checked by the first instruction words. */
static int kz3_kb_bridge_install(void)
{
    static const struct { uint32_t set, get; const char* ver; } v[] = {
        { 0x004D1A08u, 0x00BA5744u, "1.00" }, { 0x004DF120u, 0x00BB0F14u, "1.14" } };
    for (const auto& c : v) {
        if ((kz3_be32(c.set) >> 16) != 0xF821u || (kz3_be32(c.get) >> 16) != 0x3063u) continue;   /* stdu r1 / addic r3,r3 */
        s_kb_set_orig = kz3_lifted_fn(c.set);
        s_kb_get_orig = kz3_lifted_fn(c.get);
        if (!s_kb_set_orig || !s_kb_get_orig) continue;
        s_kb_buf = ppu_hle_inject_base + 0x80000u;
        ppu_register_function(c.set, kz3_kb_set);
        ppu_register_function(c.get, kz3_kb_get);
        printf("[kb-page] Lua bridge on PlayerProfile SetTimeZone 0x%08X / GetTimeZone 0x%08X (%s), reply buffer 0x%08X\n",
               c.set, c.get, c.ver, s_kb_buf);
        return 1;
    }
    printf("[kb-page] Lua bridge NOT installed (no known EBOOT layout): the Keyboard page stays off\n");
    return 0;
}

static std::string kz3_kb_cmd(const char* c) { char o[512]; pad_kb_ui_cmd(c, o, sizeof o); return o; }

/* Keyboard page layout (pass 38, ui-review rows 1-4/16/17): the list is 760 wide with 720-wide rows, so the selection
 * bar ends before the scrollbar and the bracket hugs it; key cells and their CAPS headers are centred in 150-wide
 * columns (a header stands 77 px left of its cell's list x: the list's own inset), the active slot is marked on the
 * selected cell ("[ W ]", centred, so nothing moves), a key bound twice is shown in a twin text with the game's own
 * highlight class (`mystats`, the leaderboards' "this is you" cell; 2 px lower to match the plain cell's baseline;
 * SetVisible swaps them, because SetClass adds a class and nothing takes it off again: r38-e2), and the footer sits
 * alone between the list and the legend. */
static std::string kz3_kb_page_gum(void)
{
    int rows = atoi(kz3_kb_cmd("rows").c_str());
    std::string g;
    g += "<gum default-focus=\"lst_keys\">\n\n"
         "\t<texture class=\"background\"/>\n"
         "\t<movie src=\"source:movies/MenuBackground.bik\" persistid=\"movie_back\"/>\n"
         "\t<timer id=\"kb_timer\" reset-on-input=\"false\" countdown=\"150\" on-timer-end=\"script(kbscript, Tick)\"/>\n\n"
         "\t<panel id=\"pnl_main\" class=\"safearea\" scrollable=\"false\"\n"
         "\t\toncancel=\"back()\" labelcancel=\"@key{menu_legend_back}\" onstart=\"back()\"\n"
         "\t\tonoptions=\"script(kbscript, SetToDefault)\" labeloptions=\"@key{menu_legend_default}\">\n\n"
         "\t\t@include{file:///hudelements/type3.gum}\n\n"
         "\t\t<text class=\"pagetitle\">Change the keyboard controls</text>\n"   /* RSX pass 104: as the sibling Options pages */
         "\t\t<text class=\"sectiontitle\">@key{menu_options}</text>\n\n"
         "\t\t<text id=\"hdr_0\" x=\"457\" y=\"66\" width=\"150\" text-align=\"center\">PRIMARY</text>\n"
         "\t\t<text id=\"hdr_1\" x=\"617\" y=\"66\" width=\"150\" text-align=\"center\">SECONDARY</text>\n"
         "\t\t<list id=\"lst_keys\" class=\"PEDS\" width=\"760\" height=\"360\" labelaccept=\"Change\"\n"
         "\t\t\tonaccept=\"script(kbscript, Bind)\" onleft=\"script(kbscript, SlotLeft)\" onright=\"script(kbscript, SlotRight)\">\n";
    for (int r = 0; r < rows; r++) {
        char line[768];
        std::string src = kz3_kb_cmd(("src " + std::to_string(r)).c_str());
        /* Pad button rows: the game's own action name for that button in the player's active preset and language,
         * read at run time (no game text in the port). Stick directions and Start: the port's words. */
        std::string lab = src.empty() ? kz3_kb_cmd(("label " + std::to_string(r)).c_str())
                                      : "@data{settings:controllerconfigs/activeconfig/" + src + "/text()}";
        snprintf(line, sizeof line,
                 "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"%s\" width=\"720\">\n"
                 "\t\t\t\t<text id=\"k0_%d\" x=\"380\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t\t<text id=\"k1_%d\" x=\"540\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t\t<text id=\"c0_%d\" class=\"mystats\" visible=\"false\" x=\"380\" y=\"2\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t\t<text id=\"c1_%d\" class=\"mystats\" visible=\"false\" x=\"540\" y=\"2\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t</listitem>\n",
                 r, r, lab.c_str(), r, r, r, r);
        g += line;
    }
    g += "\t\t</list>\n\n"
         "\t\t<text id=\"txt_status\" x=\"60\" y=\"532\" width=\"900\"> </text>\n\n"
         "\t\t@include{file:///common/includes/dialogconfirm.gum}\n"
         "\t</panel>\n\n"
         "\t<data id=\"settings\" src=\"object://settings\"/>\n\n"
         "\t<script id=\"kbscript\">\n<![CDATA[\n"
         "\t\tgKbSlot = 0\n"
         "\t\tgKbSel = -1\n"
         "\t\tgKbRows = " + std::to_string(rows) + "\n\n"
         "\t\tfunction Cmd(c)\n"
         "\t\t\tgPlayerProfile:SetTimeZone(\"kb:\"..c)\n"
         "\t\t\treturn gPlayerProfile:GetTimeZone()\n"
         "\t\tend\n\n"
         "\t\tfunction Lab(r)\n"
         "\t\t\tlocal t = gRoot:GetTag(\"row_\"..r)\n"
         "\t\t\tif (t == nil) then\n"
         "\t\t\t\treturn \"\"\n"
         "\t\t\tend\n"
         "\t\t\treturn t:GetLabel()\n"
         "\t\tend\n\n"
         "\t\tfunction Cell(r, s)\n"
         "\t\t\tif (r < 0) then\n"
         "\t\t\t\treturn\n"
         "\t\t\tend\n"
         "\t\t\tlocal t = Cmd(\"key \"..r..\" \"..s)\n"
         "\t\t\tlocal k = gRoot:GetTag(\"k\"..s..\"_\"..r)\n"
         "\t\t\tlocal c = gRoot:GetTag(\"c\"..s..\"_\"..r)\n"
         "\t\t\tlocal d = Cmd(\"dup \"..r..\" \"..s) == \"1\"\n"
         "\t\t\tif (r == gKbSel and s == gKbSlot) then\n"
         "\t\t\t\tt = \"[ \"..t..\" ]\"\n"
         "\t\t\tend\n"
         "\t\t\tk:SetValue(t)\n"
         "\t\t\tc:SetValue(t)\n"
         "\t\t\tk:SetVisible(not d)\n"
         "\t\t\tc:SetVisible(d)\n"
         "\t\tend\n\n"
         "\t\tfunction Status()\n"
         "\t\t\tlocal st = Cmd(\"state\")\n"
         "\t\t\tlocal t = Cmd(\"hint\")\n"
         "\t\t\tif (st == \"capture\") then\n"
         "\t\t\t\tt = \"Press a key for \"..Lab(Cmd(\"cap\"))..\". \"..Cmd(\"caphint\")\n"
         "\t\t\telseif (st == \"conflict\") then\n"
         "\t\t\t\tt = Cmd(\"cf key\")..\" is bound to both \"..Lab(Cmd(\"cf a\"))..\" and \"..Lab(Cmd(\"cf b\"))..\".\"\n"
         "\t\t\telseif (st == \"timeout\") then\n"
         "\t\t\t\tt = Cmd(\"status\")\n"
         "\t\t\tend\n"
         "\t\t\tgRoot:GetTag(\"txt_status\"):SetValue(t)\n"
         "\t\tend\n\n"
         "\t\tfunction Refresh()\n"
         "\t\t\tfor r = 0, gKbRows - 1 do\n"
         "\t\t\t\tCell(r, 0)\n"
         "\t\t\t\tCell(r, 1)\n"
         "\t\t\tend\n"
         "\t\t\tStatus()\n"
         "\t\tend\n\n"
         "\t\tfunction Select()\n"
         "\t\t\tlocal item = gRoot:GetTag(\"lst_keys\"):GetSelectedItem()\n"
         "\t\t\tlocal v = -1\n"
         "\t\t\tif (item ~= nil) then\n"
         "\t\t\t\tv = 0+item:GetValue()\n"
         "\t\t\tend\n"
         "\t\t\tif (v ~= gKbSel) then\n"
         "\t\t\t\tlocal old = gKbSel\n"
         "\t\t\t\tgKbSel = v\n"
         "\t\t\t\tCell(old, 0)\n"
         "\t\t\t\tCell(old, 1)\n"
         "\t\t\t\tCell(v, 0)\n"
         "\t\t\t\tCell(v, 1)\n"
         "\t\t\tend\n"
         "\t\t\tCmd(\"sel \"..gKbSel..\" \"..gKbSlot)\n"
         "\t\tend\n\n"
         "\t\tfunction OnPageOn()\n"
         "\t\t\tSelect()\n"
         "\t\t\tRefresh()\n"
         "\t\tend\n\n"
         "\t\tfunction OnPageOff()\n"
         "\t\t\tCmd(\"cancel\")\n"
         "\t\tend\n\n"
         "\t\tfunction Tick()\n"
         "\t\t\tSelect()\n"
         "\t\t\tif (Cmd(\"poll\") == \"changed\") then\n"
         "\t\t\t\tRefresh()\n"
         "\t\t\tend\n"
         "\t\t\tgRoot:GetTag(\"kb_timer\"):Reset()\n"
         "\t\tend\n\n"
         "\t\tfunction Bind(e)\n"
         "\t\t\tif (e ~= nil) then\n"
         "\t\t\t\te:SetHandled(true)\n"
         "\t\t\tend\n"
         "\t\t\tlocal item = gRoot:GetTag(\"lst_keys\"):GetSelectedItem()\n"
         "\t\t\tif (item ~= nil) then\n"
         "\t\t\t\tCmd(\"capture \"..item:GetValue()..\" \"..gKbSlot)\n"
         "\t\t\t\tRefresh()\n"
         "\t\t\tend\n"
         "\t\tend\n\n"
         "\t\tfunction SlotLeft(e)\n"
         "\t\t\tgKbSlot = 0\n"
         "\t\t\tCell(gKbSel, 0)\n"
         "\t\t\tCell(gKbSel, 1)\n"
         "\t\t\tSelect()\n"
         "\t\tend\n\n"
         "\t\tfunction SlotRight(e)\n"
         "\t\t\tgKbSlot = 1\n"
         "\t\t\tCell(gKbSel, 0)\n"
         "\t\t\tCell(gKbSel, 1)\n"
         "\t\t\tSelect()\n"
         "\t\tend\n\n"
         "\t\tfunction SetToDefault()\n"
         "\t\t\tgMenu:DoAction(\"dialog(dlg_confirmdefaults, kbscript, OnDialogClose)\")\n"
         "\t\tend\n\n"
         "\t\tfunction OnDialogClose(e)\n"
         "\t\t\te:SetHandled(true)\n"
         "\t\t\tif (e:GetSender():GetValue() == \"yes\") then\n"
         "\t\t\t\tCmd(\"reset\")\n"
         "\t\t\t\tRefresh()\n"
         "\t\t\tend\n"
         "\t\tend\n"
         "]]>\n\t</script>\n\n</gum>\n";
    return g;
}

/* Options > Mouse page (patch 2101; ENHANCEMENT), laid out like the game's own common/controllersettings.gum: the same
 * widths, PEDS list, selects with numeric items (the game's Advanced controller page does the same for its
 * sensitivities), the Default legend with the confirm dialog. Labels: the game's own keys where it has them (Y-axis,
 * Normal/Inverted, Options, Back, Default); "Mouse", "Mouse sensitivity" and "Zoom sensitivity" are the port's own
 * English words (the game has no mouse text; docs/guides/ui-style.md). Values go through the Lua bridge ("kb:mouse ...",
 * kz3_mouse_opts_cmd), which applies and saves them. */
static std::string kz3_mouse_page_gum(void)
{
    std::string items;
    for (int i = 1; i <= 20; i++) {
        char v[16];
        pad_mouse_opt_fmt(v, sizeof v, (float)i * PAD_MOUSE_OPT_STEP);
        items += std::string("\t\t\t\t<selectitem value=\"") + v + "\">" + v + "</selectitem>\n";
    }
    std::string g =
        "<gum default-focus=\"slc_kz3_msens\">\n\n"
        "\t<texture class=\"background\"/>\n"
        "\t<movie src=\"source:movies/MenuBackground.bik\" persistid=\"movie_back\"/>\n\n"
        "\t<panel id=\"pnl_main\" class=\"safearea\" scrollable=\"false\"\n"
        "\t\toncancel=\"back()\" labelcancel=\"@key{menu_legend_back}\"\n"
        "\t\tonoptions=\"script(settingscript, SetToDefault)\" labeloptions=\"@key{menu_legend_default}\">\n\n"
        "\t\t@include{file:///hudelements/type3.gum}\n\n"
        "\t\t<text class=\"pagetitle\">Change the mouse settings</text>\n"   /* RSX pass 104: the sibling pages' subtitle form */
        "\t\t<text class=\"sectiontitle\">@key{menu_options}</text>\n\n"
        "\t\t<list id=\"lst_options\" class=\"PEDS\" width=\"800\" onchange=\"script(settingscript,KZ3Tooltip)\">\n"
        "\t\t\t<select id=\"slc_kz3_msens\" label=\"Mouse sensitivity\" onchange=\"script(settingscript,KZ3ChangeSens)\" width=\"555\" optionwidth=\"250\"\n"
        "\t\t\t\tdefault-value=\"2.5\">\n" + items +
        "\t\t\t</select>\n"
        "\t\t\t<select id=\"slc_kz3_mads\" label=\"Zoom sensitivity\" onchange=\"script(settingscript,KZ3ChangeAds)\" width=\"555\" optionwidth=\"250\"\n"
        "\t\t\t\tdefault-value=\"1\">\n" + items +
        "\t\t\t</select>\n"
        "\t\t\t<select id=\"slc_kz3_minv\" label=\"@key{menu_y_axis}\" onchange=\"script(settingscript,KZ3ChangeInvert)\" width=\"555\" optionwidth=\"250\"\n"
        "\t\t\t\tdefault-value=\"false\">\n"
        "\t\t\t\t<selectitem value=\"false\">@key{menu_controller_normal}</selectitem>\n"
        "\t\t\t\t<selectitem value=\"true\">@key{menu_controller_inverted}</selectitem>\n"
        "\t\t\t</select>\n"
        "\t\t</list>\n\n"
        /* RSX pass 104: row descriptions in the game's tooltip panel (common/advancedcontroller.gum's pattern) */
        "\t\t<panel id=\"pnl_kz3_tooltip\" class=\"tooltip\" y=\"440\">\n"
        "\t\t\t<text id=\"txt_kz3_tooltip\"></text>\n"
        "\t\t</panel>\n\n"
        "\t\t@include{file:///common/includes/dialogconfirm.gum}\n"
        "\t</panel>\n\n"
        "\t<script id=\"settingscript\">\n<![CDATA[\n"
        "\t\tgKz3M = 0\n\n"
        "\t\tgKz3Tip = {\n"
        "\t\t\tslc_kz3_msens = \"How far the view turns for each movement of the mouse.\",\n"
        "\t\t\tslc_kz3_mads = \"Multiplies the mouse sensitivity while you aim down the sights or through a scope.\",\n"
        "\t\t\tslc_kz3_minv = \"Inverted looks down when you push the mouse forward.\"\n"
        "\t\t}\n\n"
        "\t\tfunction KZ3Tooltip(e)\n"
        "\t\t\tlocal item = gRoot:GetTag(\"lst_options\"):GetSelectedItem()\n"
        "\t\t\tlocal t = nil\n"
        "\t\t\tif (item ~= nil) then t = gKz3Tip[item:GetID()] end\n"
        "\t\t\tif (t == nil) then t = \"\" end\n"
        "\t\t\tgRoot:GetTag(\"txt_kz3_tooltip\"):SetValue(t)\n"
        "\t\tend\n\n"
        "\t\tfunction KZ3MouseCmd(c)\n"
        "\t\t\tgPlayerProfile:SetTimeZone(\"kb:mouse \"..c)\n"
        "\t\t\treturn gPlayerProfile:GetTimeZone()\n"
        "\t\tend\n\n"
        "\t\tfunction KZ3MouseShow()\n"
        "\t\t\tgRoot:GetTag(\"slc_kz3_msens\"):SetValue(KZ3MouseCmd(\"sens\"))\n"
        "\t\t\tgRoot:GetTag(\"slc_kz3_mads\"):SetValue(KZ3MouseCmd(\"ads\"))\n"
        "\t\t\tgRoot:GetTag(\"slc_kz3_minv\"):SetValue(KZ3MouseCmd(\"invert\"))\n"
        "\t\tend\n\n"
        "\t\tfunction OnPageOn()\n"
        "\t\t\tKZ3MouseShow()\n"
        "\t\t\tKZ3Tooltip(nil)\n"
        "\t\t\tgKz3M = 1\n"
        "\t\tend\n\n"
        "\t\tfunction KZ3ChangeSens(e)\n"
        "\t\t\te:SetHandled(true)\n"
        "\t\t\tif (gKz3M == 1) then KZ3MouseCmd(\"sens \"..e:GetSender():GetValue()) end\n"
        "\t\tend\n\n"
        "\t\tfunction KZ3ChangeAds(e)\n"
        "\t\t\te:SetHandled(true)\n"
        "\t\t\tif (gKz3M == 1) then KZ3MouseCmd(\"ads \"..e:GetSender():GetValue()) end\n"
        "\t\tend\n\n"
        "\t\tfunction KZ3ChangeInvert(e)\n"
        "\t\t\te:SetHandled(true)\n"
        "\t\t\tif (gKz3M == 1) then KZ3MouseCmd(\"invert \"..e:GetSender():GetValue()) end\n"
        "\t\tend\n\n"
        "\t\tfunction SetToDefault()\n"
        "\t\t\tgMenu:DoAction(\"dialog(dlg_confirmdefaults, settingscript, OnDialogClose)\")\n"
        "\t\tend\n\n"
        "\t\tfunction OnDialogClose(e)\n"
        "\t\t\te:SetHandled(true)\n"
        "\t\t\tif (e:GetSender():GetValue() == \"yes\") then\n"
        "\t\t\t\tgKz3M = 0\n"
        "\t\t\t\tKZ3MouseCmd(\"reset\")\n"
        "\t\t\t\tKZ3MouseShow()\n"
        "\t\t\t\tgKz3M = 1\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "]]>\n\t</script>\n\n</gum>\n";
    return g;
}

/* The controller row of an options menu, followed by a copy pointing at the Keyboard page and one at the Mouse page. */
static std::string kz3_kb_link_rows(const std::string& in, int* added)
{
    std::string out = in, low = in;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    *added = 0;
    size_t at = low.find("controllersettings.gum)");
    while (at != std::string::npos) {
        size_t b = low.rfind("<listitem", at), e = low.find("/>", at);
        if (b == std::string::npos || e == std::string::npos) break;
        std::string row = out.substr(b, e + 2 - b), lrow = low.substr(b, e + 2 - b);
        size_t ls = lrow.find("label=\"");
        size_t le = ls == std::string::npos ? ls : lrow.find('"', ls + 7);
        size_t us = lrow.find("controllersettings.gum");
        if (ls == std::string::npos || le == std::string::npos || us == std::string::npos) break;
        std::string copy = row.substr(0, ls) + "label=\"Keyboard\"" + row.substr(le + 1);
        size_t u2 = copy.find(row.substr(us, 22));
        if (u2 != std::string::npos) copy.replace(u2, 22, "keyboardsettings.gum");
        size_t id = copy.find(" id=\"");
        if (id != std::string::npos) copy.erase(id, copy.find('"', id + 5) + 1 - id);
        size_t ls2 = out.rfind('\n', b);
        std::string indent = ls2 == std::string::npos ? "" : out.substr(ls2 + 1, b - ls2 - 1);
        std::string mcopy = copy;   /* patch 2101: the Mouse page row, right after Keyboard */
        size_t kb = mcopy.find("keyboardsettings.gum"), kl = mcopy.find("label=\"Keyboard\"");
        if (kb != std::string::npos && kl != std::string::npos) {
            mcopy.replace(kb, 20, "mousesettings.gum");   /* the later offset first */
            mcopy.replace(kl, 17, "label=\"Mouse\"");
        }
        std::string ins = "\n" + indent + copy + "\n" + indent + mcopy;
        out.insert(e + 2, ins);
        low.insert(e + 2, std::string(ins.size(), ' '));
        (*added)++;
        at = low.find("controllersettings.gum)", e + 2 + ins.size());
    }
    return out;
}

/* EXIT GAME (RSX pass 61): MAINMENU.GUM gets a row after CREDITS, a confirm dialog shaped like
 * the game's own quit dialog (common/includes/dialogquit.gum: Yes/No from the game's strings, No focused, cancelable)
 * and two functions in its mainmenuscript; Yes calls the Lua bridge with "kb:exit". Returns the text unchanged
 * (*added = 0) when any anchor is missing. */
static std::string kz3_exit_rows(const std::string& in, int* added)
{
    *added = 0;
    std::string low = in;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    size_t at = low.find("endcreditsmain.gum)");
    size_t e = at == std::string::npos ? at : low.find("/>", at);
    size_t b = at == std::string::npos ? at : low.rfind("<listitem", at);
    size_t sc = low.find("<script id=\"mainmenuscript\">");
    size_t cd = sc == std::string::npos ? sc : low.rfind("]]>");
    size_t dl = low.rfind("</dialog>");
    if (e == std::string::npos || b == std::string::npos || cd == std::string::npos || cd < sc ||
        dl == std::string::npos || dl > sc) return in;
    size_t ls = in.rfind('\n', b);
    std::string indent = ls == std::string::npos ? "" : in.substr(ls + 1, b - ls - 1);
    const std::string row = "\n" + indent + "<listitem id=\"ltm_kz3_exit\" label=\"EXIT GAME\" onaccept=\"script(mainmenuscript, KZ3ExitGame)\"/>";
    const std::string dlg =
        "\n\n\t<dialog id=\"dlg_kz3_exit\" label=\"Exit Killzone 3 and return to the desktop?\" default-focus=\"dlg_kz3_exit_no\"\n"
        "\t\tcancelable=\"true\">\n"
        "\t\t<dialogoption id=\"dlg_kz3_exit_yes\" label=\"@key{menu_dialog_yes}\" value=\"yes\"/>\n"
        "\t\t<dialogoption id=\"dlg_kz3_exit_no\" label=\"@key{menu_dialog_no}\" value=\"no\"/>\n"
        "\t</dialog>";
    const std::string fns =
        "\n\t\tfunction KZ3ExitGame(event)\n"
        "\t\t\tgMenu:DoAction(\"dialog(dlg_kz3_exit, mainmenuscript, OnKZ3ExitClose)\")\n"
        "\t\tend\n\n"
        "\t\tfunction OnKZ3ExitClose(event)\n"
        "\t\t\tif (event:GetSender():GetValue() == \"yes\") then\n"
        "\t\t\t\tgPlayerProfile:SetTimeZone(\"kb:exit\")\n"
        "\t\t\tend\n"
        "\t\tend\n\n\t";
    std::string out = in;                       /* back to front, so the earlier offsets stay valid */
    out.insert(cd, fns);
    out.insert(dl + 9, dlg);
    out.insert(e + 2, row);
    *added = 1;
    return out;
}

/* QUIT TO DESKTOP (RSX pass 109): a row right after the pause menu's own Quit row (campaign/campaignoptions.gum and
 * multiplayer/ingame/options.gum), a confirm dialog with the game's own quit-dialog text (menu_quitgame_confirmation,
 * Yes/No from the game's strings, No focused, cancelable) and its own small script (neither file has a spare script
 * the row could use: options.gum has none). Yes calls the Lua bridge with "kb:exit" like the main menu EXIT GAME.
 * Returns the text unchanged (*added = 0) when an anchor is missing. */
static std::string kz3_pause_exit_rows(const std::string& in, int* added)
{
    *added = 0;
    std::string low = in;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    size_t at = low.find("label=\"@key{menu_quit}\"");
    size_t e = at == std::string::npos ? at : low.find("/>", at);
    size_t b = at == std::string::npos ? at : low.rfind("<listitem", at);
    size_t g = low.rfind("</gum>");
    if (e == std::string::npos || b == std::string::npos || g == std::string::npos || b > at || e < at) return in;
    size_t ls = in.rfind('\n', b);
    std::string indent = ls == std::string::npos ? "" : in.substr(ls + 1, b - ls - 1);
    std::string row = in.substr(b, e + 2 - b);          /* the game's Quit row: same class, so same look */
    size_t la = row.find("label=\"@key{menu_quit}\"");
    if (la == std::string::npos) return in;
    row.replace(la, 23, "label=\"QUIT TO DESKTOP\"");
    size_t oa = row.find("onaccept=\"");
    size_t ob = oa == std::string::npos ? oa : row.find('"', oa + 10);
    if (ob == std::string::npos) return in;
    row.replace(oa, ob + 1 - oa, "onaccept=\"script(kz3exitscript, KZ3QuitDesktop)\"");
    for (const char* a : { " value=\"", " id=\"" }) {   /* the original's dialog hook and id do not carry over */
        size_t v = row.find(a);
        if (v != std::string::npos) row.erase(v, row.find('"', v + strlen(a)) + 1 - v);
    }
    const std::string add =
        "\n\t<dialog id=\"dlg_kz3_exit\" label=\"@key{menu_quitgame_confirmation}\" default-focus=\"dlg_kz3_exit_no\"\n"
        "\t\tcancelable=\"true\">\n"
        "\t\t<dialogoption id=\"dlg_kz3_exit_yes\" label=\"@key{menu_dialog_yes}\" value=\"yes\"/>\n"
        "\t\t<dialogoption id=\"dlg_kz3_exit_no\" label=\"@key{menu_dialog_no}\" value=\"no\"/>\n"
        "\t</dialog>\n\n"
        "\t<script id=\"kz3exitscript\">\n<![CDATA[\n"
        "\t\tfunction KZ3QuitDesktop(event)\n"
        "\t\t\tgMenu:DoAction(\"dialog(dlg_kz3_exit, kz3exitscript, OnKZ3ExitClose)\")\n"
        "\t\tend\n\n"
        "\t\tfunction OnKZ3ExitClose(event)\n"
        "\t\t\tevent:SetHandled(true)\n"
        "\t\t\tif (event:GetSender():GetValue() == \"yes\") then\n"
        "\t\t\t\tgPlayerProfile:SetTimeZone(\"kb:exit\")\n"
        "\t\t\tend\n"
        "\t\tend\n"
        "]]>\n\t</script>\n\n";
    std::string out = in;
    out.insert(g, add);
    out.insert(e + 2, "\n" + indent + row);
    *added = 1;
    return out;
}

static void (*s_kb_prev_miss)(char*, size_t) = nullptr;
static std::string s_kb_dir;                  /* <vfs root>/psarc-cache/kz3-ui */
static int s_kb_page_test = 0;
static CRITICAL_SECTION s_kb_vfs_lock;

static bool kz3_write_file(const std::string& path, const std::string& text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return (fclose(f) == 0) && ok;
}

static void kz3_kb_miss_hook(char* path, size_t cap)
{
    std::string p = path;
    for (char& c : p) c = (char)tolower((unsigned char)c), c = c == '\\' ? '/' : c;
    size_t u = p.rfind("usrdir/");
    std::string key = u == std::string::npos ? "" : p.substr(u + 7);
    static const char* links[] = { KZ3_HC_SETTINGS, KZ3_HC_MAINSETTINGS, KZ3_HC_CAMPAIGNSETTINGS, KZ3_HC_MP_OPTIONS };
    if (key == KZ3_GEN_MOUSE) {   /* Options > Mouse (patch 2101) */
        EnterCriticalSection(&s_kb_vfs_lock);
        static bool mdone = false;
        const std::string mout = s_kb_dir + "/mousesettings.gum";
        if (!mdone) {
            mdone = kz3_write_file(mout, kz3_mouse_page_gum());
            printf("[kb-page] mousesettings.gum %s\n", mdone ? "written" : "NOT written");
        }
        LeaveCriticalSection(&s_kb_vfs_lock);
        if (mdone) snprintf(path, cap, "%s", mout.c_str());
        else if (s_kb_prev_miss) s_kb_prev_miss(path, cap);
        fflush(stdout);
        return;
    }
    bool page = key == KZ3_GEN_KEYBOARD ||
                (s_kb_page_test && key == KZ3_HC_MAINMENU);
    int link = -1;
    for (int i = 0; i < 4; i++) if (key == links[i]) link = i;
    if (!page && key == KZ3_HC_MAINMENU) link = 5;   /* EXIT GAME row (pass 61) */
    if (!page && key == KZ3_HC_DISPLAY) link = 6;   /* Display mode row (patch 2095) */
    if (!page && key == KZ3_HC_CAMPAIGNOPTIONS) link = 7;   /* QUIT TO DESKTOP (pass 109) */
    if (!page && link < 0) { if (s_kb_prev_miss) s_kb_prev_miss(path, cap); return; }
    EnterCriticalSection(&s_kb_vfs_lock);
    static bool done[8] = { false, false, false, false, false, false, false, false };
    std::string out = s_kb_dir + "/" + (page ? std::string("keyboardsettings.gum") : key.substr(key.rfind('/') + 1));
    int slot = page ? 4 : link;
    if (!done[slot]) {
        if (page) {
            std::string g = kz3_kb_page_gum();
            /* KZ3_KB_PAGE_FILE=<path>: serve that GUM instead of the generated one (layout trials without a rebuild). */
            const char* pf = getenv("KZ3_KB_PAGE_FILE");
            if (pf && *pf) {
                FILE* f = fopen(pf, "rb");
                if (f) { g.clear(); char b[4096]; size_t m; while ((m = fread(b, 1, sizeof b, f)) > 0) g.append(b, m); fclose(f); }
                printf("[kb-page] KZ3_KB_PAGE_FILE=%s %s\n", pf, f ? "served" : "NOT readable (generated page)");
            }
            done[slot] = kz3_write_file(out, g);
            printf("[kb-page] page %s (%zu bytes)%s\n", done[slot] ? "written" : "NOT written", g.size(),
                   key.find("mainmenu") != std::string::npos ? " in place of the main menu (KZ3_KB_PAGE_TEST)" : "");
        } else {
            kz3_host_composed_check(key.c_str(), "kz3_kb_miss_hook");   /* net 40: the rewritten file must be host-composed */
            char src[1100];
            snprintf(src, sizeof src, "%s", path);
            if (s_kb_prev_miss) s_kb_prev_miss(src, sizeof src);
            FILE* f = fopen(src, "rb");
            std::string text;
            if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n); fclose(f); }
            int added = 0;
            std::string patched;
            if (link == 6) {   /* patch 2095 (rsx_display_mode.h): the Display mode row */
                size_t pn = 0;
                char* pg = text.empty() ? nullptr
                                        : rsx_dm_patch_gum_opts(text.data(), text.size(), &pn,
                                                                (s_mb_override ? RSX_DM_GUM_MBLUR : 0u) |   /* 2107 */
                                                                (s_bloom_override ? RSX_DM_GUM_BLOOM : 0u) |   /* 2121 */
                                                                (s_fov_ok ? RSX_DM_GUM_FOV : 0u));          /* 2109 */
                if (pg) { patched.assign(pg, pn); free(pg); added = 1; }
            } else
                patched = text.empty() ? text : link == 5 ? kz3_exit_rows(text, &added)
                          : link == 7 ? kz3_pause_exit_rows(text, &added) : kz3_kb_link_rows(text, &added);
            if (link == 3 && added > 0) { int qa = 0; patched = kz3_pause_exit_rows(patched, &qa); printf("[kb-page] %s: QUIT TO DESKTOP %s\n", key.c_str(), qa ? "added" : "NOT added"); }
            if (link == 7) printf("[kb-page] %s: QUIT TO DESKTOP %s\n", key.c_str(), added ? "added" : "NOT added");
            /* KZ3_TEST_ONLINE_COOP=1 (test only, 2026-10-22): the main menu's Co-op row opens the game's own unreachable
             * online co-op page (coop/coopmodeselect.gum; the RE notes "Online co-op campaign path") instead of
             * the split-screen level select. */
            if (link == 5 && added > 0) {
                const char* oc = getenv("KZ3_TEST_ONLINE_COOP");
                const std::string from = "coop/coopsplitlevelselect.gum?mode=2", to = "coop/coopmodeselect.gum";
                int n = 0;   /* the menu script opens the split level select from two places */
                for (size_t at = (oc && *oc == '1') ? patched.find(from) : std::string::npos; at != std::string::npos;
                     at = patched.find(from, at + to.size()), n++)
                    patched.replace(at, from.size(), to);
                if (n) printf("[kb-page] KZ3_TEST_ONLINE_COOP: Co-op row -> %s (%d site(s))\n", to.c_str(), n);
            }
            done[slot] = added > 0 && kz3_write_file(out, patched);
            printf("[kb-page] %s: %d %s row(s) added%s\n", key.c_str(), added,
                   link == 6 ? "Display" : link == 5 ? "EXIT GAME" : link == 7 ? "pause-menu" : "Keyboard + Mouse",
                   done[slot] ? "" : " (served unchanged)");
            if (!done[slot]) { LeaveCriticalSection(&s_kb_vfs_lock); snprintf(path, cap, "%s", src); return; }
        }
    }
    LeaveCriticalSection(&s_kb_vfs_lock);
    if (done[slot]) snprintf(path, cap, "%s", out.c_str());
    else if (s_kb_prev_miss) s_kb_prev_miss(path, cap);
    fflush(stdout);
}

/* ---------------------------------------------------------------------------
 * View aspect (RSX pass 91, patch 2096; ENHANCEMENT, KZ3_ASPECT=screen default, =16:9 off)
 *
 * The camera aspect has one source (the RE notes s.2 and s.4): GameViewGame::vf22 @0x007E5908 returns
 * 16/9 * AspectRatioMultiplier (G+0x2188; literal 0x3FE38E39 @0x007E5900). The per-frame camera writer @0x0038BB08
 * calls it twice through the vtable (slot 0x58): lr 0x0038BB80 = the divisor of FieldOfView = 2*atan(tan(hfov/2)/a)
 * (the authored FOVs are horizontal at 16:9) and lr 0x0038BC10 = the frustum width aspect stored to Camera+0x68
 * @0x0038BC14 (Camera::vf17 @0x0031E120 -> right = top*aspect @0x00036428). Every other caller seen in the binary
 * (0x005B15F0, 0x005B1798: atan(tan/a); 0x0064D66C, 0x007F1034, 0x007F122C: handed to 0x007EF960, which divides
 * the same way and reads the width aspect back from the camera's frustum) uses it as the divisor.
 * The hook re-registers 0x007E5908 in the indirect-call map (vf22 has no direct `bl` callers in the lift) and scales
 * the lifted result by r = client aspect / (16/9): the width aspect by r, the divisor by min(r, 1). Wider than 16:9:
 * the PS3's vertical FOV, a wider horizontal one (Hor+); narrower (16:10): the PS3's horizontal FOV, a taller view
 * (Vert+). Culling, LOD and streaming follow (they read the camera's frustum). The guest still draws 1280x720 and
 * the present stretches each frame drawn with that camera to the client (rsx_display_note_wide, patch 2096).
 * ASSUMPTION: no caller outside the seven needs the unscaled 16:9 (the first call from each lr is logged).
 * 1.00 and 1.14 (kAspect100/kAspect114): installed for the layout whose literal and store word match.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
extern "C" double rsx_display_view_ratio(void);   /* rsx_live_draw.c (patch 2096): 1.0 = 16:9 or KZ3_ASPECT=16:9 */
extern "C" void   rsx_display_note_wide(void);
extern "C" int    rsx_display_get_aspect(void);   /* rsx_null_backend.c (patch 2096) */
static void (*s_vf22_orig)(ppu_context*);
/* Per-version addresses (RE pass 144, tools/assets/twin_verify.py + a word check of both EBOOTs): vf22 and the
 * camera writer's two call sites are byte-identical shapes in 1.14 (vf22 body: same G+0x2188 lfs, only the global's
 * lis/lwz halves differ; the `bctrl` at lr-4 and the Camera+0x68 store word 0xD03E0068 match). */
/* lr_fov (RSX pass 103): the camera writer's first vf22 call, whose result divides tan(half hfov) (0x0038BB7C bctrl,
 * 1.14 0x00397CAC; the writer keeps src in r29 and the full angle in f31 across it). game/mode: the game type
 * [[Game + mode] + 0x104], 0 = campaign: the Lua binding IsCampaign (1.14 0x0086B6D8, 1.00 0x00848DF8) returns
 * `type == 0` (the binder table 0x142B640 lists each OPD before its name; IsOnlineGame is the session test 0x0086B6A8),
 * and 1.00's UpdateEntities keys its multiplayer debug draw on types 1-3 (the RE notes s.7.1). Run
 * r103-k2f110 read 0 in the campaign. */
struct Kz3AspectVer { uint32_t vf22, lit, store, lr_cam, lr_frustum, lr_fov, game, mode; };
static constexpr Kz3AspectVer kAspect100 = {0x007E5908u, 0x007E5900u, 0x0038BC14u, 0x0038BC10u, 0x00326840u, 0x0038BB80u,
                                            0x01587880u, 0x768u};
static constexpr Kz3AspectVer kAspect114 = {0x00805EC8u, 0x00805EC0u, 0x00397D44u, 0x00397D40u, 0x00332A80u, 0x00397CB0u,
                                            0x015FCF80u, 0x778u};
static const Kz3AspectVer* s_asp = &kAspect100;
static int s_aspect_on;

/* Field of view (RSX pass 103, ENHANCEMENT, opt-in: Options > Display > Field of view, display.ini fov; default = the
 * game's own). The camera writer @0x0038BB08 (1.14 @0x00397C38) computes FieldOfView = 2 atan(tan(half) / A) with
 * half = 0.5 * src[0x40] (authored horizontal FOV at 16:9, degrees) * pi/180 / src[0x44] (zoom) and A = vf22().
 * A setting F (horizontal degrees at 16:9) scales the half angle by F / 80 (the first-person view's src[0x40] on foot
 * and on the K4 mounted gun, runs r103-k2c1 / r103-k4p1; cutscene cameras seen with 90; KZ3_FOV_BASE overrides) and
 * returns A' = A tan(half) / tan(half'), so zoom (ADS, src[0x44]) stays relative and the aspect handling above is kept.
 * Campaign only (game type 0, co-op included): off in every multiplayer mode, online or Botzone, so online play
 * never gets a wider view than the game's own. KZ3_FOV overrides the saved value for one run. */
static double kz3_fov_base(void)
{
    static const double b = [] { const char* e = getenv("KZ3_FOV_BASE"); const double v = e ? atof(e) : 0.0;
                                 return v >= 30.0 && v <= 150.0 ? v : 80.0; }();   /* runs r103-k2c1, r103-k4p1 */
    return b;
}
static int kz3_game_type(void)   /* 0 campaign (incl. co-op), 1-3 multiplayer types, -1 unknown */
{
    const uint32_t g = kz3_be32(s_asp->game + s_asp->mode);
    if (g < 0x10000u || g >= 0xF0000000u) return -1;
    return (int)kz3_be32(g + 0x104);
}

/* The camera writer's FOV divisor call: FOV and the RSX pass 103 camera log (KZ3_CAM_LOG=1, or KZ3_MOUSE_LOG=1: one line
 * per camera per frame with the pose rows, for the yaw per presented frame). s_fov_k = the factor A is multiplied by
 * (after the aspect scale, applied by the caller). */
/* Late-latch view rotation (RSX pass 105, patch 2112, ENHANCEMENT, display only; design the RE notes s.43.4/43.11).
 * Runs in the camera writer @0x00397C38 (1.14) at its first vf22 call (lr 0x00397CB0), BEFORE the pose copy
 * src+0x00..0x3F -> Camera+0x10..0x4F (0x00397CE0); r29 = src (the blender's stack local, rebuilt from the camera entity
 * E+0x60 on every call, never read back), r31 = the view. It rotates src rows 0-2 by the counts the next look update will
 * take (pad_mouse_peek, the look hook's scale and clamps: pad_look_latch_delta/rotate), so the drawn Camera = the
 * logic pose + the counts not applied yet, and the logic step still takes every count (no jump back). Nothing else is
 * written: E, the controller, the angles and the accumulator are untouched. Gates (any fails = no rotation):
 *   a  E = ([view+0x2C] - 0xC)->+0xE8 has a whitelisted vptr, 1.14 only: 0x00F46BF8 = the camera the blender
 *      snapshots while on foot (run r105-k2a0, every gameplay frame; ctor 0x005B49C0, stored at 0x005B49E0, right after
 *      FirstPersonCameraEntity's ctor 0x005B44D4; class name not in the RTTI names, ASSUMPTION: the player's first-person
 *      camera class; its vtable shares no slot with 0x00F465C8) and 0x00F465C8 FirstPersonCameraEntity (re-data s.43.1);
 *   b  a look update took counts within the last 4 flips (s_lat, same idle rule as patch 2099);
 *   c  no GUM menu has the focus (pad_mouse_menu_on);
 *   d  no camera blend: [view+0x38] null or view+0xD0 >= 1 (the blender mixes two snapshots while view+0xD0 is below
 *      the constant in f30 at 0x00399108; ASSUMPTION: that constant is 1.0, the log prints view+0xD0);
 *   e  one view per flip (split screen off);
 *   f  mounted gun: not the 0x5F7028 look variant (angles from the Camera) and no basis change (resource +0x234).
 * Pass 110 (patch 2116, the render-rate fill, KZ3_MOUSE_LATCH=2 = default): the camera Rep shows the body at the frame's
 * game time, i.e. logic - (1 - alpha) x the last take (alpha = Rep+0x124; checked on 7 runs of RSX pass 107 to 1.3 ms sd),
 * so the pending counts alone (pass 105, =1) left an alpha-dependent gap and juddered. The fill adds (1 - alpha) x
 * take + the pending counts: the drawn view = every count that arrived by this call, each shown once. Extra gates:
 * on foot only (9; the gun Rep not measured) and alpha readable (10); the consumed part only when Rep+0x110 = the
 * take's slot end. KZ3_MOUSE_LATCH=0 off. s_latch_dyaw/dpitch = what this frame added (logs). */
static int kz3_latch_on(void)
{
    /* RSX pass 110: 0 off, 1 = patch 2112 alone (pending counts only), 2 = the render-rate fill (default; patch 2116) */
    static const int on = [] { const char* e = getenv("KZ3_MOUSE_LATCH");
                               return !e || !e[0] ? 2 : e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0; }();
    return on;
}
static void kz3_wbef(uint32_t ea, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static float s_latch_dyaw, s_latch_dpitch, s_fill_a = -1.0f, s_fill_c;
static int s_latch_dx, s_latch_dy, s_latch_gate;
static uint32_t s_latch_evt;
static float s_latch_d0;
static unsigned s_writer_flip;   /* flip of the last writer call (the vf28 order counter) */
static const char* const kLatchGate[] = {"on", "a: camera entity not FirstPersonCameraEntity", "b: no look take in 4 flips",
                                         "c: menu focus", "d: camera blend", "e: more than one view", "f: mounted gun variant",
                                         "1.00 EBOOT", "no counts", "fill: not on foot (mounted gun not measured)",
                                         "fill: camera Rep alpha not readable (vptr not 0x00F46BF8 or alpha outside [0, 1])"};
/* RSX pass 105 order trace (diagnostics): one letter per event for 200 events from about flip 600 after the first writer
 * call: W camera writer, V HumanoidRep::vf28, T on-foot/mounted look take, F the flip count changed since the last
 * event. Tells whether the weapon rep reads the Camera written in the same frame (W..V) or the previous one. */
static char s_trace[260];
static int s_tn = -1;
static unsigned s_trace_flip, s_trace_arm;
extern "C" void kz3_latch_trace(char c)
{
    const unsigned f = cellGcm_flip_request_count();
    if (s_tn < 0) { if (c != 'W') return; if (!s_trace_arm) s_trace_arm = f + 600; if (f < s_trace_arm) return; s_tn = 0; s_trace_flip = f; }
    if (s_tn >= 200) return;
    if (f != s_trace_flip) { s_trace[s_tn++] = 'F'; s_trace_flip = f; }
    s_trace[s_tn++] = c;
    if (s_tn >= 200) { s_trace[s_tn] = 0; fprintf(stderr, "[latch] order trace from flip %u: %s\n", s_trace_arm, s_trace); }
}
static void kz3_cam_latch(ppu_context* ctx, uint32_t src)
{
    s_latch_dyaw = s_latch_dpitch = 0.0f;
    s_latch_dx = s_latch_dy = 0;
    const uint32_t view = (uint32_t)ctx->gpr[31];
    const unsigned flip = cellGcm_flip_request_count();
    s_writer_flip = flip;
    kz3_latch_trace('W');
    static unsigned s_vflip, s_multi_flip = ~0u;
    static uint32_t s_vfirst;
    if (flip != s_vflip) { s_vflip = flip; s_vfirst = view; }
    else if (view != s_vfirst) s_multi_flip = flip;
    uint32_t E = 0;
    const uint32_t cur = ml_ok(view) ? ml_r32(view + 0x2C) : 0;
    if (cur > 0xC && ml_ok(cur - 0xC)) E = ml_r32(cur - 0xC + 0xE8);
    s_latch_evt = ml_ok(E) ? ml_r32(E) : 0;
    s_latch_d0 = ml_ok(view) ? ml_rf(view + 0xD0) : 0.0f;
    int g = 0;
    if (s_asp != &kAspect114) g = 7;
    else if (s_latch_evt != 0x00F46BF8u && s_latch_evt != 0x00F465C8u) g = 1;
    else if (!s_lat.mode || (unsigned)(flip - s_lat.take_flip) > 4u) g = 2;
    else if (pad_mouse_menu_on()) g = 3;
    else if (ml_r32(view + 0x38) != 0 && !(s_latch_d0 >= 1.0f)) g = 4;
    else if (s_multi_flip != ~0u && (unsigned)(flip - s_multi_flip) < 60u) g = 5;
    else if (s_lat.mode == 2 && (s_lat.gun_cam || s_lat.basis)) g = 6;
    /* RSX pass 110 fill (mode 2): on foot only, and the Rep's alpha must be live (Rep+0x124, vptr 0x00F46BF8 only) */
    const int fill = kz3_latch_on() == 2;
    float alpha = -1.0f;
    int same_slot = 0;
    s_fill_a = -1.0f; s_fill_c = 0.0f;
    if (!g && fill) {
        if (s_lat.mode != 1) g = 9;
        else {
            alpha = s_latch_evt == 0x00F46BF8u ? ml_rf(E + 0x124) : -1.0f;
            if (!(alpha >= 0.0f && alpha <= 1.0f)) g = 10;
            /* the last take ran in the slot the Rep shows (Rep+0x110 = that take's W+0xE8); otherwise the Rep is past
             * it (a step without a take): nothing consumed is pending */
            else same_slot = s_lat.slot_end && ml_r64(E + 0x110) == s_lat.slot_end;
        }
    }
    if (!g) {
        pad_mouse_peek(&s_latch_dx, &s_latch_dy);
        if (!s_latch_dx && !s_latch_dy && !(fill && same_slot && (s_lat.tdy != 0.0f || s_lat.tdp != 0.0f))) g = 8;
    }
    s_latch_gate = g;
    if (fill && !g) {
        static unsigned long long s_n, s_other;
        s_n++;
        if (!same_slot) s_other++;
        if (s_n == 2000 || s_n == 20000) fprintf(stderr, "[latch] fill: %llu frames, %llu with the Rep past the take's slot\n", s_n, s_other);
    }
    {
        static int s_said[11];
        if (!s_said[g]++)
            fprintf(stderr, "[latch] gate %s first at flip=%u (E=0x%08X vt=0x%08X view=0x%08X D0=%.3f mode=%d)\n", kLatchGate[g],
                    flip, E, s_latch_evt, view, s_latch_d0, s_lat.mode);
    }
    if (g || !kz3_latch_on()) return;   /* off: the gates and the peek still go to the [cam] log */
    float dyaw, dpitch;
    pad_look_latch_delta(s_lat.k, s_lat.invert, s_latch_dx, s_latch_dy, s_lat.y0, s_lat.ymin, s_lat.ymax, s_lat.p0,
                         s_lat.pmin, s_lat.pmax, &dyaw, &dpitch);
    if (fill) {   /* + the consumed-but-not-shown part (1 - alpha) x take (patch 2116) */
        const float py = dyaw;
        pad_look_fill_delta(same_slot ? alpha : 1.0f, s_lat.tdy, s_lat.tdp, dyaw, dpitch, &dyaw, &dpitch);
        s_fill_a = alpha;
        s_fill_c = dyaw - py;
    }
    if (!isfinite(dyaw) || !isfinite(dpitch) || fabsf(dyaw) > 1.0f || fabsf(dpitch) > 1.0f) return;
    float r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = kz3_bef(src + 0x10 * i + 4 * j);
    pad_look_latch_rotate(r, dyaw, dpitch);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) kz3_wbef(src + 0x10 * i + 4 * j, r[i][j]);
    s_latch_dyaw = dyaw;
    s_latch_dpitch = dpitch;
}

/* RSX pass 105 diagnostics (KZ3_CAM_LOG=1 / KZ3_MOUSE_LOG=1, 1.14): lr at GameViewGame::vf17 0x00804B20 and vf27 0x00806398
 * entry (each distinct lr once, then counts every 10 s), and HumanoidRep::vf28 0x00661B48 calls counted before/after the
 * camera writer of the same flip (the RE notes s.43.8: after = the weapon reads this frame's Camera). Wrapped with
 * ppu_register_function: only the virtual calls reach them, which is how all three are called. */
static void (*s_vf17_orig)(ppu_context*);
static void (*s_vf27_orig)(ppu_context*);
static void (*s_vf28_orig)(ppu_context*);
static uint64_t s_vf28_before, s_vf28_after, s_vf17_n, s_vf27_n;
static void kz3_diag_lr(const char* what, uint32_t lr, ppu_context* ctx)
{
    static uint32_t seen[16];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 16) seen[n++] = lr;
    LARGE_INTEGER qf, qc;
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
    const uint32_t v = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[latch] %s lr=0x%08X r3=0x%08X vt=0x%08X flip=%u qpc=%.2f\n", what, lr, v, ml_ok(v) ? ml_r32(v) : 0,
            cellGcm_flip_request_count(), (double)qc.QuadPart * 1e3 / (double)qf.QuadPart);
}
static void kz3_diag_summary(void)
{
    static double s_last;
    const double now = (double)GetTickCount64() / 1000.0;
    if (now - s_last < 10.0) return;
    s_last = now;
    fprintf(stderr, "[latch] calls: vf17=%llu vf27=%llu vf28 before-writer=%llu after-writer=%llu flip=%u\n",
            (unsigned long long)s_vf17_n, (unsigned long long)s_vf27_n, (unsigned long long)s_vf28_before,
            (unsigned long long)s_vf28_after, cellGcm_flip_request_count());
}
static void kz3_vf17_diag(ppu_context* ctx) { s_vf17_n++; kz3_diag_lr("GameViewGame::vf17", (uint32_t)ctx->lr, ctx); s_vf17_orig(ctx); }
static void kz3_vf27_diag(ppu_context* ctx) { s_vf27_n++; kz3_diag_lr("GameViewGame::vf27", (uint32_t)ctx->lr, ctx); s_vf27_orig(ctx); kz3_diag_summary(); }
static void kz3_vf28_diag(ppu_context* ctx)
{
    if (s_writer_flip == cellGcm_flip_request_count()) s_vf28_after++; else s_vf28_before++;
    kz3_latch_trace('V');
    kz3_diag_lr("HumanoidRep::vf28", (uint32_t)ctx->lr, ctx);
    s_vf28_orig(ctx);
}

/* Render-time camera interpolation (RSX pass 107, patch 2114 + this hook, ENHANCEMENT candidate, OFF: measured
 * worse; design the RE notes s.50.5 (1), facts the RE notes "Pass 107"). In the camera writer before the
 * pose copy 0x397CE0, src rows 0-3 become lerp(prev, cur, alpha) (cam_interp_pose). Logging run r107-k2log: the
 * camera Rep (E, vptr 0xF46BF8) has flags 0xB400001B (bit 8: Rep::Update 0x34F5A8 copies Rep+0x60 -> Rep+0xA0 every
 * frame with a step; Rep+0xA0 = the previous frame's Rep+0x60 in 1144/1144 frames), alpha Rep+0x124 is live and equals
 * 1 - (W+0xE8 - W+0x78) / Rep+0x118 (Rep+0x110 = W+0xE8, Rep+0x118 = the slot length 1/30 or 2/30 s). But the Rep pose
 * is NOT the slot state: it is not a copy of the sim entity (+0x128 bit 8 skips 0x34F0D8) and already follows the
 * frame's game time W+0x78, so the lerp shows a mix of two frames and adds error (turn speed cv 0.228 -> 0.373 on
 * r107-k2log's frames, 0.149 -> 0.246 on r107-k2b1). Modes for re-measuring: KZ3_CAM_INTERP=1 the Rep pair
 * (Rep+0xA0, Rep+0x60) with alpha Rep+0x124, 2 the sim entity pair (sim+0x60, sim+0xA0, sim = Rep+0x12C) with that
 * alpha, 3 the sim pair with the world-clock alpha; 0 = off (default). Gates: 1.14, E vptr 0xF46BF8, no camera blend,
 * one view per flip, src == the Rep pose, the game's interpolation flag (Rep+0x128 bit 2), no cut (cam_interp_cut:
 * invalid pose, fallback 5 m / 1.5 rad per slot). The [rep] line (KZ3_CAM_LOG / KZ3_MOUSE_LOG) logs these fields. */
static int s_ci_mode = -1;
static int kz3_ci_mode(void)
{
    if (s_ci_mode < 0) {
        const char* e = getenv("KZ3_CAM_INTERP");
        s_ci_mode = e && e[0] >= '0' && e[0] <= '3' ? e[0] - '0' : 0;
    }
    return s_ci_mode;
}
static int s_ci_gate;
static float s_ci_alpha, s_ci_dyaw;
static const char* const kCiGate[] = {"on", "off", "1.00 EBOOT", "camera not FirstPersonCameraEntityRep 0xF46BF8", "camera blend",
                                      "more than one view", "src differs from the Rep pose", "game interpolation flag clear",
                                      "cut: invalid pose", "cut: moved over 5 m", "cut: turned over 1.5 rad", "sim entity invalid"};
static void kz3_rd_pose(uint32_t a, float m[4][4])
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = ml_rf(a + 0x10 * i + 4 * j);
}
static void kz3_cam_interp(ppu_context* ctx, uint32_t src, int log)
{
    s_ci_gate = 0; s_ci_alpha = -1.0f; s_ci_dyaw = 0.0f;
    const int mode = kz3_ci_mode();
    const uint32_t view = (uint32_t)ctx->gpr[31];
    const unsigned flip = cellGcm_flip_request_count();
    static unsigned s_vflip, s_multi_flip = ~0u;
    static uint32_t s_vfirst;
    if (flip != s_vflip) { s_vflip = flip; s_vfirst = view; }
    else if (view != s_vfirst) s_multi_flip = flip;
    uint32_t E = 0;
    const uint32_t curv = ml_ok(view) ? ml_r32(view + 0x2C) : 0;
    if (curv > 0xC && ml_ok(curv - 0xC)) E = ml_r32(curv - 0xC + 0xE8);
    const uint32_t vt = ml_ok(E) ? ml_r32(E) : 0;
    const uint32_t sim = vt == 0x00F46BF8u ? ml_r32(E + 0x12C) : 0;
    float S[4][4], R6[4][4], RA[4][4], S6[4][4], SA[4][4];
    if (vt == 0x00F46BF8u) {
        kz3_rd_pose(src, S); kz3_rd_pose(E + 0x60, R6); kz3_rd_pose(E + 0xA0, RA);
        if (ml_ok(sim)) { kz3_rd_pose(sim + 0x60, S6); kz3_rd_pose(sim + 0xA0, SA); }
    }
    /* The game's alpha (Rep+0x124) and the same formula on the world clock (W+0xE8 = next slot due, W+0x78 = now). */
    const uint32_t W = ml_r32(kMouse114.world);
    float aclk = -1.0f;
    if (ml_ok(W)) {
        const double left = (double)(int64_t)(ml_r64(W + 0xE8) - ml_r64(W + 0x78));
        aclk = (float)(1.0 - left / 2660000.0);
    }
    const float agame = vt == 0x00F46BF8u ? ml_rf(E + 0x124) : -1.0f;
    const uint32_t flags = vt == 0x00F46BF8u ? ml_r32(E + 0x128) : 0;
    int g = 0;
    if (!mode) g = 1;
    else if (s_asp != &kAspect114) g = 2;
    else if (vt != 0x00F46BF8u) g = 3;
    else if (ml_r32(view + 0x38) != 0 && !(ml_rf(view + 0xD0) >= 1.0f)) g = 4;
    else if (s_multi_flip != ~0u && (unsigned)(flip - s_multi_flip) < 60u) g = 5;
    float diff = 0.0f;
    if (vt == 0x00F46BF8u)
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 3; j++) diff = fmaxf(diff, fabsf(S[i][j] - R6[i][j]));
    if (!g && !(diff < 1e-4f)) g = 6;
    /* mode 1: the Rep's own pair (Rep+0xA0 prev, Rep+0x60 cur) and alpha; mode 2: the sim entity's pair
     * (sim+0x60 prev, sim+0xA0 cur) with the Rep's alpha; mode 3: the sim pair with the world-clock alpha. */
    const float (*P)[4] = mode == 1 ? RA : S6;
    const float (*Cu)[4] = mode == 1 ? R6 : SA;
    float alpha = mode == 3 ? aclk : agame;
    if (!g && mode >= 2 && !ml_ok(sim)) g = 11;
    if (!g && mode != 3 && !(flags & 2)) g = 7;
    if (!g) {
        const int cut = cam_interp_cut(P, Cu, 5.0f, 1.5f);
        if (cut) g = 7 + cut;
    }
    if (!(alpha >= 0.0f)) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    float O[4][4];
    if (!g) {
        cam_interp_pose(P, Cu, alpha, O);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 3; j++) kz3_wbef(src + 0x10 * i + 4 * j, O[i][j]);
        s_ci_alpha = alpha;
        s_ci_dyaw = cam_pose_yaw(O) - cam_pose_yaw(S);
    }
    s_ci_gate = g;
    {
        static int s_said[12];
        if (!s_said[g]++)
            fprintf(stderr, "[interp] gate %s first at flip=%u (E=0x%08X vt=0x%08X sim=0x%08X flags=0x%X mode=%d)\n", kCiGate[g], flip,
                    E, vt, sim, flags, mode);
    }
    if (log && vt == 0x00F46BF8u) {
        LARGE_INTEGER qf, qc;
        QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        float d6[3] = {R6[3][0] - RA[3][0], R6[3][1] - RA[3][1], R6[3][2] - RA[3][2]};
        float ds[3] = {SA[3][0] - S6[3][0], SA[3][1] - S6[3][1], SA[3][2] - S6[3][2]};
        printf("[rep] flip=%u qpc=%.2f g=%d step=%u a=%.4f aclk=%.4f fl=0x%X t110=%lld i118=%.5f f11c=%.5f yR60=%.5f yRA0=%.5f "
               "yS60=%.5f ySA0=%.5f ysrc=%.5f dyi=%.5f dR=%.4f dS=%.4f sdiff=%.6f w78=%llu wE8=%llu c10c=%.4f simvt=0x%08X\n",
               flip, (double)qc.QuadPart * 1e3 / (double)qf.QuadPart, g, ml_r32(E + 0x120), agame, aclk, flags,
               (long long)(ml_ok(W) ? (int64_t)(ml_r64(E + 0x110) - ml_r64(W + 0x78)) : 0), ml_rf(E + 0x118), ml_rf(E + 0x11C),
               cam_pose_yaw(R6), cam_pose_yaw(RA), ml_ok(sim) ? cam_pose_yaw(S6) : 0.f, ml_ok(sim) ? cam_pose_yaw(SA) : 0.f,
               cam_pose_yaw(S), s_ci_dyaw, sqrtf(cam_dot3(d6, d6)), ml_ok(sim) ? sqrtf(cam_dot3(ds, ds)) : 0.f, diff,
               (unsigned long long)(ml_ok(W) ? ml_r64(W + 0x78) : 0), (unsigned long long)(ml_ok(W) ? ml_r64(W + 0xE8) : 0),
               ml_ok(s_foot_C) ? ml_rf(s_foot_C + 0x10C) : 0.f, ml_ok(sim) ? ml_r32(sim) : 0);
    }
}

static int s_fov_applied;
static double s_fov_k = 1.0;
static void kz3_cam_fov(ppu_context* ctx)
{
    const uint32_t src = (uint32_t)ctx->gpr[29];
    const bool src_ok = src >= 0x10000u && src < 0xF0000000u;
    const float h = src_ok ? kz3_bef(src + 0x40) : 0.f, z = src_ok ? kz3_bef(src + 0x44) : 0.f;
    /* f31 = src[0x40] * c0 / src[0x44] (the full angle, c0 = pi/180 at r28+0); the writer halves it with c4 = r28+4
     * (0.5) before the tan (run r103-k4p1: f31 1.39626 for hfov 80). */
    const uint32_t tbl = (uint32_t)ctx->gpr[28];
    float c4 = tbl >= 0x10000u && tbl < 0xF0000000u ? kz3_bef(tbl + 4) : 0.5f;
    if (!(c4 > 0.1f && c4 < 1.0f)) c4 = 0.5f;
    const double half = ctx->fpr[31] * (double)c4;
    s_fov_applied = 0;
    s_fov_k = 1.0;
    const int fov = (int)s_fov_deg;
    static int s_type_said = -2;
    const int type = fov ? kz3_game_type() : 0;
    if (fov && type != s_type_said) {
        s_type_said = type;
        printf("[fov] %d deg (base %.1f), game type %d: %s\n", fov, kz3_fov_base(), type,
               type == 0 ? "applied" : "OFF outside the campaign (multiplayer, Botzone)");
        fflush(stdout);
    }
    if (fov && type == 0 && half > 1e-4 && half < 1.5) {
        double h2 = half * (double)fov / kz3_fov_base();
        if (h2 > 1.45) h2 = 1.45;   /* 166 degrees */
        s_fov_k = tan(half) / tan(h2);
        s_fov_applied = 1;
    }
    static int s_log = -1;
    if (s_log < 0) { const char* a = getenv("KZ3_CAM_LOG"); const char* b = getenv("KZ3_MOUSE_LOG");
                     s_log = (a && a[0] == '1') || (b && b[0] == '1'); }
    if (src_ok) kz3_cam_interp(ctx, src, s_log == 1);   /* RSX pass 107: interpolated pose first, then the latch on top */
    if (src_ok && (s_log == 1 || kz3_latch_on())) kz3_cam_latch(ctx, src);   /* RSX pass 105: before the pose copy */
    static float s_hz[32][2];
    static int s_nhz;
    int k = 0;
    while (k < s_nhz && !(fabsf(s_hz[k][0] - h) < 0.01f && fabsf(s_hz[k][1] - z) < 0.001f)) k++;
    if (k == s_nhz && s_nhz < 32) {
        s_hz[s_nhz][0] = h; s_hz[s_nhz][1] = z; s_nhz++;
        printf("[fov] camera src=0x%08X hfov=%.3f zoom=%.4f half=%.5f c4=%.3f (0.5*h*pi/180/z=%.5f) flip=%u\n", src, h, z, half, c4,
               z != 0.f ? 0.5 * h * 0.017453292519943295 / z : 0.0, cellGcm_flip_request_count());
        fflush(stdout);
    }
    if (s_log == 1 && src_ok) {
        LARGE_INTEGER qf, qc;
        QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        printf("[cam] flip=%u qpc=%.2f src=0x%08X h=%.3f z=%.4f k=%.4f ci=%d ca=%.4f lat=%d pk=%d,%d dyaw=%.5f dpitch=%.5f evt=0x%08X d0=%.3f "
               "fa=%.4f fc=%.5f r0=%.5f,%.5f,%.5f r1=%.5f,%.5f,%.5f r2=%.5f,%.5f,%.5f\n",
               cellGcm_flip_request_count(), (double)qc.QuadPart * 1e3 / (double)qf.QuadPart, src, h, z, s_fov_k,
               s_ci_gate, s_ci_alpha, s_latch_gate, s_latch_dx, s_latch_dy, s_latch_dyaw, s_latch_dpitch, s_latch_evt, s_latch_d0,
               s_fill_a, s_fill_c, kz3_bef(src), kz3_bef(src + 4), kz3_bef(src + 8), kz3_bef(src + 0x10), kz3_bef(src + 0x14),
               kz3_bef(src + 0x18), kz3_bef(src + 0x20), kz3_bef(src + 0x24), kz3_bef(src + 0x28));
    }
}

static void kz3_vf22_aspect(ppu_context* ctx)   /* GameViewGame::vf22 @0x007E5908 */
{
    const uint32_t lr = (uint32_t)ctx->lr;
    s_vf22_orig(ctx);
    const double r = rsx_display_view_ratio();
    /* width role: the Camera+0x68 store @0x0038BC14, and 0x003265D0's aspect out (lr 0x00326840, seen in r91-k4-219),
     * which 0x0061D4E0 uses as both divisor and frustum width: r there keeps its shape (horizontal FOV kept). */
    const int width = lr == s_asp->lr_cam || lr == s_asp->lr_frustum;
    if (lr == s_asp->lr_fov) kz3_cam_fov(ctx);
    if (s_aspect_on) {
        if (width) rsx_display_note_wide();
        if (r != 1.0) {
            const double base = ctx->fpr[1], k = rsx_dm_aspect_scale(r, width);
            ctx->fpr[1] = (double)(float)(base * k);
            static uint32_t seen[16];
            static int nseen;
            int i = 0;
            while (i < nseen && seen[i] != lr) i++;
            if (i == nseen && nseen < 16) {
                seen[nseen++] = lr;
                printf("[aspect] vf22 lr=0x%08X base=%.6f r=%.6f k=%.6f -> %.6f (%s)\n", lr, base, r, k, ctx->fpr[1],
                       width ? "frustum width" : "fov divisor");
                fflush(stdout);
            }
        }
    }
    if (lr == s_asp->lr_fov && s_fov_applied) ctx->fpr[1] = (double)(float)(ctx->fpr[1] * s_fov_k);   /* RSX pass 103 */
}

/* HUD world projection in the 16:9 box (RSX pass 108, patch 2115; ENHANCEMENT, display, with 2113; design
 * the RE notes Pass 169, notes the RE notes "Pass 108"). Every world-anchored HUD element (HUDNavigation
 * objective markers and arrows, HUDPlayerNames, HUDSpotAndMark, HUDAimLock/HUDTargetEntity, HUDHealthBars, the
 * crosshair spread) takes its frustum from one getter, 0x0080BD78 (1.00 0x007EA4B8), which calls Camera::vf17 0x00329D80
 * (1.00 0x0031E120) through the vtable (bctrl at 0x0080BDE8, lr 0x0080BDEC; 1.00 bctrl 0x007EA528, lr 0x007EA52C: same
 * instruction layout in both EBOOTs, rizin pd, pass 108). Patch 2096 makes that frustum Hor+/Vert+ for the window,
 * and 2113 squeezes the HUD's 2D draws into the centred 16:9 box, so a mark projected with the window's frustum lands
 * off its target by the squeeze factor. While the squeeze is active (the same predicate as the draw side,
 * rsx_live_draw_hud169_factors), the hook scales the frustum vf17 wrote for that one caller by the squeeze factors
 * (rsx_h169_frustum): the box's own 16:9 frustum. Every other vf17 caller (3D culling, the scene) is untouched.
 * KZ3_HUD_PROJ=0 turns it off for one run (A/B); KZ3_HUD_PROJ_LOG=1 prints the frustum once a second. */
#include "rsx_hud169.h"
extern "C" int rsx_live_draw_hud169_factors(float* kx, float* ky);   /* rsx_live_draw.c (patch 2113) */
struct Kz3HudProjVer { uint32_t vf17, lr; };
static constexpr Kz3HudProjVer kHudProj100 = {0x0031E120u, 0x007EA52Cu}, kHudProj114 = {0x00329D80u, 0x0080BDECu};
static void (*s_cvf17_orig)(ppu_context*);
static uint32_t s_hudp_lr;
static int s_hudp_on = 1, s_hudp_log;
static void kz3_cam_vf17_hud(ppu_context* ctx)   /* Camera::vf17 0x00329D80 (1.00 0x0031E120) */
{
    const uint32_t lr = (uint32_t)ctx->lr, out = (uint32_t)ctx->gpr[3];
    s_cvf17_orig(ctx);
    if (lr != s_hudp_lr || out < 0x10000u || out >= 0xF0000000u) return;
    float kx = 1.0f, ky = 1.0f;
    const int sq = rsx_live_draw_hud169_factors(&kx, &ky);
    if (s_hudp_log) {
        static ULONGLONG last;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            const float l = kz3_bef(out + 0xC), r = kz3_bef(out + 0x10), t = kz3_bef(out + 0x14), b = kz3_bef(out + 0x18);
            printf("[hudproj] flip=%u lr=0x%08X out=0x%08X f0..8=%.4f %.4f %.4f | l=%.5f r=%.5f t=%.5f b=%.5f | %.5f %.5f "
                   "r/t=%.5f squeeze=%d kx=%.5f ky=%.5f %s\n", cellGcm_flip_request_count(), lr, out, kz3_bef(out),
                   kz3_bef(out + 4), kz3_bef(out + 8), l, r, t, b, kz3_bef(out + 0x1C), kz3_bef(out + 0x20),
                   t != 0.0f ? r / t : 0.0f, sq, kx, ky, sq && s_hudp_on ? "-> box" : "kept");
            fflush(stdout);
        }
    }
    if (sq && s_hudp_on) rsx_h169_frustum(vm_base + out, kx, ky);
}
static void kz3_hud_proj_init(void)
{
    const char* e = getenv("KZ3_HUD_PROJ");
    if (e && e[0] == '0') s_hudp_on = 0;
    const char* lg = getenv("KZ3_HUD_PROJ_LOG");
    s_hudp_log = lg && lg[0] == '1';
    const Kz3HudProjVer* v = s_asp == &kAspect114 ? &kHudProj114 : &kHudProj100;
    /* the bctrl before the return address (BO=20 branch-always; the compiler's BI field varies: 0x4E9E0421 in 1.14,
     * pass 108's first run refused it), and vf17's prologue word (stdu r1,-N(r1)) */
    if ((kz3_be32(v->lr - 4) & 0xFFE0FFFFu) != 0x4E800421u || (kz3_be32(v->vf17) >> 16) != 0xF821u || !(s_cvf17_orig = kz3_lifted_fn(v->vf17))) {
        printf("[hudproj] HUD projection hook NOT installed (bctrl/prologue check failed at 0x%08X/0x%08X)\n", v->lr - 4, v->vf17);
        return;
    }
    s_hudp_lr = v->lr;
    ppu_register_function(v->vf17, kz3_cam_vf17_hud);
    printf("[hudproj] HUD world projection hook on Camera::vf17 0x%08X at lr 0x%08X (%s): %s\n", v->vf17, v->lr,
           v == &kHudProj114 ? "1.14" : "1.00", s_hudp_on ? "box frustum while the 2D layer is squeezed" : "off (KZ3_HUD_PROJ=0)");
}

static void kz3_aspect_init(void)
{
    s_aspect_on = rsx_display_get_aspect() == RSX_DM_ASPECT_SCREEN;
    if (!s_aspect_on) printf("[aspect] off (KZ3_ASPECT=16:9): PS3 camera (the hook stays for the field of view)\n");
    const Kz3AspectVer* v = nullptr;
    for (const Kz3AspectVer* c : { &kAspect100, &kAspect114 })
        if (kz3_be32(c->lit) == 0x3FE38E39u && kz3_be32(c->store) == 0xD03E0068u && kz3_lifted_fn(c->vf22)) { v = c; break; }
    if (!v || !(s_vf22_orig = kz3_lifted_fn(v->vf22))) {
        printf("[aspect] camera hook NOT installed (neither the 1.00 nor the 1.14 layout): 16:9 camera, image scaled by the Image scaling row\n");
        return;
    }
    s_asp = v;
    ppu_register_function(v->vf22, kz3_vf22_aspect);
    kz3_hud_proj_init();   /* RSX pass 108, patch 2115 */
    s_fov_ok = 1;
    if (kz3_latch_on()) printf("[latch] %s%s\n", kz3_latch_on() == 2 ? "render-rate fill ON (default; KZ3_MOUSE_LATCH=0 off, =1 pending counts only)"
                                                                   : "late-latch view rotation ON, pending counts only (KZ3_MOUSE_LATCH=1)",
                               v == &kAspect114 ? "" : ": 1.00 EBOOT, gate stays off (offsets not read)");
    /* RSX pass 105 diagnostics, 1.14 only (KZ3_CAM_LOG=1 or KZ3_MOUSE_LOG=1) */
    const char* cl = getenv("KZ3_CAM_LOG"); const char* ml = getenv("KZ3_MOUSE_LOG");
    if (v == &kAspect114 && ((cl && cl[0] == '1') || (ml && ml[0] == '1'))) {
        struct { uint32_t a; void (**orig)(ppu_context*); void (*fn)(ppu_context*); } d[] = {
            {0x00804B20u, &s_vf17_orig, kz3_vf17_diag}, {0x00806398u, &s_vf27_orig, kz3_vf27_diag},
            {0x00661B48u, &s_vf28_orig, kz3_vf28_diag}};
        for (auto& e : d)
            if ((*e.orig = kz3_lifted_fn(e.a))) ppu_register_function(e.a, e.fn);
            else printf("[latch] diag hook 0x%08X not found (not lifted)\n", e.a);
    }
    printf("[aspect] camera hook on GameViewGame::vf22 0x%08X (%s): %s\n", v->vf22, v == &kAspect100 ? "1.00" : "1.14",
           s_aspect_on ? "view at the client's aspect" : "field of view only");
}
#else
static void kz3_aspect_init(void) {}
extern "C" void kz3_latch_trace(char) {}
#endif

/* ---------------------------------------------------------------------------
 * Menu mouse (RSX pass 93, patch 2098; ENHANCEMENT, the RE notes "Pass 93")
 *
 * Hover focuses the GUM item under the pointer through the game's own setter, so the green bracket and the focus
 * sound follow as with the pad; left click = CROSS on that item, right click = CIRCLE, wheel = UP/DOWN (LEFT/RIGHT
 * on a select or slider). Layouts: the RE notes "Menu cursor, pass 143" (RE pass 143), the tag offsets
 * (+0x10 parent, +0x14 owner Menu, +4/+8 children, +0x10C "enabled" expression, +0x140 enable object) re-read by
 * RSX pass 93 in the pad navigation handler 0x0011B974 / 1.14 0x0011E1A0 (identical code in both). The tag's widget is
 * not a field: RE pass 146 (the RE notes "Pass 146"): widget = Q_lookup(Q, tag+0xEC), Q = *(*(Menu+0x70)+8);
 * hit rect = inner rect grown by the border, visible byte, scissor-class ancestors clip.
 *   - The GUM context C = *(Menu + 0x60) is captured from r3 of the tag methods the game calls through vtables
 *     (ppu_register_function wraps them: only indirect calls reach the wrapper, the lifted bodies run unchanged);
 *     a capture counts only when *(tag+0x14) is an object with the Menu vtable. The capturing thread of the event
 *     handler (slot 10, 0x0010D2D8 / 1.14 0x0010F614) is the menu's thread.
 *   - The rest runs in cellPadGetData's port-0 poll (cellPad.c g_pad_menu_bits_hook), and its guest calls (SetFocus,
 *     the widgets' IsEnabled) only on that menu thread, on the poll's context in a fresh stack frame below r1; the
 *     whole context is restored afterwards. ASSUMPTION: that poll is outside the menu's own tree walk (it is the
 *     PadDriver's per-frame read, before GameInput and the pages tick).
 * KZ3_MENU_MOUSE=0 off; KZ3_MENU_MOUSE_LOG=1 one line per hover look; KZ3_MENU_MOUSE_SCRIPT (pad_mouse.h) for tests.
 * -----------------------------------------------------------------------*/
#include "../input/pad_menu_mouse.h"
extern "C" unsigned (*g_pad_menu_bits_hook)(unsigned pad_buttons, int pad_active);
extern "C" void (*g_pad_menu_sent_hook)(int delivered);
extern "C" int pad_kb_capture_busy(void);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

namespace {
struct MmVer {
    const char* ver;
    uint32_t set_focus, src, menu_vt, base_vt, nav_vt, nav_fn, rect, tick;
    uint32_t wl[7];   /* widget vtables: Button, ListItem, Select, Slider, Checkbox, Input, Tab (RE pass 143/146) */
    /* RE pass 146: tag -> widget = Q_lookup(Q, tag+0xEC), Q = *(*(Menu+0x70)+8) (F vtable f_vt); widget fields */
    uint32_t q_lookup, f_vt, cursor_vt, border, vis, parent, wq, cur_focus;
    uint32_t clip_vt[3];   /* scissor classes: ScrollingPanel (VERIFIED), List, Panel (ASSUMPTION, RE pass 146) */
};
constexpr MmVer kMm[2] = {
    {"1.00", 0x0010CB34u, 0x013D4818u, 0x00EAB4A0u, 0x00EAC250u, 0x00EAC2A0u, 0x0011B974u, 0xE0u, 0x001062C0u,
     {0x00EACA80u, 0x00EACCD8u, 0x00EACD50u, 0x00EACDC8u, 0x00EACB10u, 0x00EACB88u, 0x00EACF78u},
     0x0011230Cu, 0x00EABCC8u, 0x00EAD258u, 0x90u, 0x130u, 0x100u, 0x114u, 0x144u, {0x00EAD868u, 0x00EAD7F0u, 0x00EAD508u}},
    {"1.14", 0x0010ED9Cu, 0x01449E98u, 0x00F25280u, 0x00F26038u, 0x00F26088u, 0x0011E1A0u, 0xF0u, 0x00108784u,
     {0x00F26868u, 0x00F26AC0u, 0x00F26B38u, 0x00F26BB0u, 0x00F268F8u, 0x00F26970u, 0x00F26D60u},
     0x00114704u, 0x00F25AB0u, 0x00F27050u, 0x94u, 0x140u, 0x110u, 0x124u, 0x15Cu, {0x00F27660u, 0x00F275E8u, 0x00F27300u}},
};
/* Wrapped vtable slots: the Gum code ones of the base tag vtable and of the list/page tag vtable that holds the pad
 * navigation handler (the shared 0xB0xxxx stubs are left alone). Slot 10 = the event handler. */
constexpr int kMmBaseSlots[] = {4, 6, 7, 8, 9, 10, 15, 16};
constexpr int kMmNavSlots[] = {3, 4, 7, 10, 17};
constexpr int kMmWrapMax = 14;
int s_mm_tick_idx = -1;   /* the wrapper of the per-frame Gum hook (tick) */
const MmVer* s_mm = nullptr;
uint32_t s_mm_toc = 0;
volatile uint32_t s_mm_C = 0;
volatile uint64_t s_mm_tid = ~0ull;
int s_mm_log = 0;
void (*s_mm_orig[kMmWrapMax])(ppu_context*);
uint32_t s_mm_addr[kMmWrapMax];
int s_mm_is_event[kMmWrapMax];
int s_mm_n = 0;

unsigned s_mm_hits[kMmWrapMax], s_mm_root_hits[kMmWrapMax];   /* diag: calls per wrapped method, and with r3 = root */
volatile uint32_t s_mm_root = 0;
void mm_capture(ppu_context* ctx, int i)
{
    const uint32_t t = (uint32_t)ctx->gpr[3];
    s_mm_hits[i]++;
    if (t == s_mm_root) s_mm_root_hits[i]++;
    if (!vm_base || !ml_ok(t)) return;
    const uint32_t m = ml_r32(t + 0x14);
    if (!ml_ok(m) || ml_r32(m) != s_mm->menu_vt) return;
    const uint32_t c = ml_r32(m + 0x60);
    if (!ml_ok(c)) return;
    if (c != s_mm_C) {
        s_mm_C = c;
        KZ3_LOGI("menu-mouse", "GUM context C=0x%08X (Menu 0x%08X) from tag 0x%08X in 0x%08X, thread %llu", c, m, t,
                 s_mm_addr[i], (unsigned long long)ctx->thread_id);
    }
    if ((s_mm_is_event[i] || s_mm_tid == ~0ull) && s_mm_tid != ctx->thread_id) {
        static int said = 0;
        if (said++ < 6)
            KZ3_LOGI("menu-mouse", "menu thread %llu (tag method 0x%08X%s)", (unsigned long long)ctx->thread_id,
                     s_mm_addr[i], s_mm_is_event[i] ? ", event handler" : "");
        s_mm_tid = ctx->thread_id;
    }
}
void mm_menu_step(ppu_context* ctx);
void mm_frame_tick(void);
template <int N> void mm_wrap(ppu_context* ctx)
{
    mm_capture(ctx, N);
    s_mm_orig[N](ctx);
    if (N == s_mm_tick_idx && ctx->thread_id == s_mm_tid) {   /* the menu thread's frame tick */
        mm_frame_tick();                                    /* a press lasts two menu frames (pad_menu_mouse.h) */
        mm_menu_step(ctx);                                  /* hover/click */
    }
}
void (*const kMmWraps[kMmWrapMax])(ppu_context*) = {mm_wrap<0>, mm_wrap<1>, mm_wrap<2>,  mm_wrap<3>,  mm_wrap<4>,
                                                    mm_wrap<5>, mm_wrap<6>, mm_wrap<7>,  mm_wrap<8>,  mm_wrap<9>,
                                                    mm_wrap<10>, mm_wrap<11>, mm_wrap<12>, mm_wrap<13>};

/* A guest call on ctx (the caller saved and restores the context; r1 is already below the live frame). */
uint32_t mm_call(ppu_context* ctx, uint32_t code, uint32_t toc, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6)
{
    ctx->gpr[2] = toc;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6;
    ctx->ctr = code;
    ctx->lr = 0;
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f(ctx); }
    return (uint32_t)ctx->gpr[3];
}
/* obj->vf5(obj, tag) != 0 (the check the pad navigation makes on the widget and on the tag's +0x140 object). */
int mm_vf5(ppu_context* ctx, uint32_t obj, uint32_t tag)
{
    const uint32_t vt = ml_r32(obj), opd = ml_ok(vt) ? ml_r32(vt + 0x14) : 0;
    if (!ml_ok(opd)) return 0;
    return mm_call(ctx, ml_r32(opd), ml_r32(opd + 4), obj, tag, 0, 0) != 0;
}
/* Tag enabled the way 0x0011B974 tests it before it moves the focus there. */
int mm_enabled(ppu_context* ctx, uint32_t tag)
{
    const uint32_t w = ml_r32(tag + 0x10C), o = ml_r32(tag + 0x140);
    if (!ml_ok(w) || !mm_vf5(ctx, w, tag)) return 0;
    if (!ml_ok(o)) return 1;
    const uint32_t o2 = ml_r32(o + 8);
    if (!ml_ok(o2) || ml_r32(o2 + 8) == 0) return 1;
    return mm_vf5(ctx, o, tag);
}
int mm_kind(uint32_t w)   /* index in the whitelist, -1 = not a focus target */
{
    const uint32_t vt = ml_ok(w) ? ml_r32(w) : 0;
    for (int k = 0; k < 7; k++) if (vt == s_mm->wl[k]) return k;
    return -1;
}
struct MmPick { uint32_t tag, widget; int n_cands, n_nodes; KZ3MenuRect r; };
constexpr int kMmNodes = 768;
/* Q, the page widget context of the Menu that owns tag (RE pass 146): Menu = tag+0x14, F = Menu+0x70 (vtable f_vt),
 * Q = F+8. 0 when the chain does not check out. */
uint32_t mm_q(uint32_t tag)
{
    const uint32_t m = ml_r32(tag + 0x14);
    if (!ml_ok(m)) return 0;
    const uint32_t f = ml_r32(m + 0x70);
    if (!ml_ok(f) || ml_r32(f) != s_mm->f_vt) return 0;
    const uint32_t q = ml_r32(f + 8);
    return ml_ok(q) && ml_r32(q + 0x20) == m ? q : 0;
}
/* The tag's widget through the game's own map lookup (0x0011230C / 0x00114704, borrowed pointer). */
uint32_t mm_widget(ppu_context* ctx, uint32_t q, uint32_t tag)
{
    const uint32_t w = mm_call(ctx, s_mm->q_lookup, s_mm_toc, q, ml_r32(tag + 0xEC), 0, 0);
    return ml_ok(w) && ml_r32(w + 0xC) == ml_r32(tag + 0xEC) ? w : 0;
}
/* Hit rect = inner rect grown by the border (the CursorWidget's bracket, RE pass 146). */
KZ3MenuRect mm_hit_rect(uint32_t w)
{
    const uint32_t ro = s_mm->rect;
    const int bd = (int)ml_r32(w + s_mm->border);
    const int g = bd > 0 && bd < 64 ? bd : 0;
    KZ3MenuRect r = {(int)ml_r32(w + ro) - g, (int)ml_r32(w + ro + 4) - g, (int)ml_r32(w + ro + 8) + g, (int)ml_r32(w + ro + 12) + g};
    return r;
}
/* Clips: the inner rects of the widget's scissor-class ancestors (widget parent chain), innermost first. */
int mm_clips(uint32_t w, KZ3MenuRect* clip)
{
    int n = 0;
    uint32_t p = ml_r32(w + s_mm->parent);
    for (int k = 0; k < 24 && ml_ok(p) && n < KZ3_MENU_CLIPS; k++, p = ml_r32(p + s_mm->parent)) {
        const uint32_t vt = ml_r32(p);
        if (vt != s_mm->clip_vt[0] && vt != s_mm->clip_vt[1] && vt != s_mm->clip_vt[2]) continue;
        const uint32_t ro = s_mm->rect;
        clip[n].l = (int)ml_r32(p + ro); clip[n].t = (int)ml_r32(p + ro + 4);
        clip[n].r = (int)ml_r32(p + ro + 8); clip[n].b = (int)ml_r32(p + ro + 12);
        if (clip[n].r > clip[n].l && clip[n].b > clip[n].t) n++;
    }
    return n;
}
/* The focusable, enabled, visible item under (x, y) among root's tag subtree (tag 0 = none). */
MmPick mm_pick(ppu_context* ctx, uint32_t root, float x, float y)
{
    static KZ3MenuCand cand[kMmNodes];
    static uint32_t ctag[kMmNodes], cwid[kMmNodes];
    struct Fr { uint32_t tag; int depth; };
    static Fr stack[kMmNodes];
    MmPick out = {0, 0, 0, 0, {0, 0, 0, 0}};
    const uint32_t q = mm_q(root);
    if (!q) return out;
    int sp = 0, nc = 0;
    stack[sp].tag = root; stack[sp].depth = 0; sp++;
    while (sp > 0 && out.n_nodes < kMmNodes) {
        const Fr f = stack[--sp];
        out.n_nodes++;
        const uint32_t w = mm_widget(ctx, q, f.tag);
        if (w && mm_kind(w) >= 0 && vm_base[w + s_mm->vis] != 0 && ml_r32(w + 0x34) != 0 && ml_r32(w + 0x3C) != 0 && nc < kMmNodes) {
            KZ3MenuCand& c = cand[nc];
            const KZ3MenuRect r = mm_hit_rect(w);
            c.id = nc; c.l = r.l; c.t = r.t; c.r = r.r; c.b = r.b; c.depth = f.depth;
            c.n_clips = mm_clips(w, c.clip);
            c.enabled = 1; c.focusable = 1;
            ctag[nc] = f.tag; cwid[nc] = w; nc++;
        }
        const uint32_t n = ml_r32(f.tag + 4), arr = ml_r32(f.tag + 8);
        if (!ml_ok(arr) || n > 256 || f.depth >= 24) continue;
        for (uint32_t i = n; i-- > 0 && sp < kMmNodes;) {
            const uint32_t ch = ml_r32(arr + 4 * i);
            if (!ml_ok(ch)) continue;
            stack[sp].tag = ch; stack[sp].depth = f.depth + 1; sp++;
        }
    }
    out.n_cands = nc;
    for (int tries = 0; tries < 6; tries++) {
        const int id = kz3_menu_hit_test(cand, nc, x, y, 0);
        if (id < 0) break;
        if (mm_enabled(ctx, ctag[id])) {
            out.tag = ctag[id];
            out.widget = cwid[id];
            out.r = {cand[id].l, cand[id].t, cand[id].r, cand[id].b};
            break;
        }
        cand[id].enabled = 0;
    }
    return out;
}

/* RE pass 146's one-run checklist, once (the first look with a focused item): one line per item. */
void mm_checklist(ppu_context* ctx, uint32_t c, uint32_t f0)
{
    const uint32_t m = ml_r32(f0 + 0x14), f = ml_ok(m) ? ml_r32(m + 0x70) : 0, q = ml_ok(f) ? ml_r32(f + 8) : 0;
    KZ3_LOGI("menu-mouse", "check 1: Menu 0x%08X, Menu+0x60 0x%08X == C 0x%08X: %s", m, ml_ok(m) ? ml_r32(m + 0x60) : 0, c,
             ml_ok(m) && ml_r32(m + 0x60) == c ? "ok" : "NO");
    KZ3_LOGI("menu-mouse", "check 2: F 0x%08X vt 0x%08X (want 0x%08X), F+4 0x%08X: %s", f, ml_ok(f) ? ml_r32(f) : 0, s_mm->f_vt,
             ml_ok(f) ? ml_r32(f + 4) : 0, ml_ok(f) && ml_r32(f) == s_mm->f_vt && ml_r32(f + 4) == m ? "ok" : "NO");
    if (!ml_ok(q)) { KZ3_LOGI("menu-mouse", "check 3: Q invalid"); return; }
    const uint32_t cur = ml_r32(q + 0x40);
    KZ3_LOGI("menu-mouse", "check 3: Q 0x%08X, Q+0x20 0x%08X, root rect %d,%d,%d,%d, cursor 0x%08X vt 0x%08X (want 0x%08X): %s", q,
             ml_r32(q + 0x20), (int)ml_r32(q + 0x10), (int)ml_r32(q + 0x14), (int)ml_r32(q + 0x18), (int)ml_r32(q + 0x1C), cur,
             ml_ok(cur) ? ml_r32(cur) : 0, s_mm->cursor_vt, ml_r32(q + 0x20) == m && ml_ok(cur) && ml_r32(cur) == s_mm->cursor_vt ? "ok" : "NO");
    const uint32_t id = ml_r32(f0 + 0xEC);
    const uint32_t w = mm_call(ctx, s_mm->q_lookup, s_mm_toc, q, id, 0, 0);
    KZ3_LOGI("menu-mouse", "check 4: id %u -> widget 0x%08X, w+0xC %u, w->Q 0x%08X, vt 0x%08X (kind %d), visible %u", id, w,
             ml_ok(w) ? ml_r32(w + 0xC) : 0, ml_ok(w) ? ml_r32(w + s_mm->wq) : 0, ml_ok(w) ? ml_r32(w) : 0, ml_ok(w) ? mm_kind(w) : -9,
             ml_ok(w) ? vm_base[w + s_mm->vis] : 0);
    if (!ml_ok(w)) return;
    const uint32_t ro = s_mm->rect;
    KZ3_LOGI("menu-mouse", "check 5: outer %d,%d,%d,%d inner %d,%d,%d,%d border %d layer %d parent 0x%08X", (int)ml_r32(w + ro - 0x20),
             (int)ml_r32(w + ro - 0x1C), (int)ml_r32(w + ro - 0x18), (int)ml_r32(w + ro - 0x14), (int)ml_r32(w + ro), (int)ml_r32(w + ro + 4),
             (int)ml_r32(w + ro + 8), (int)ml_r32(w + ro + 12), (int)ml_r32(w + s_mm->border), (int)ml_r32(w + 0x30), ml_r32(w + s_mm->parent));
    KZ3_LOGI("menu-mouse", "check 6: cursor focused widget 0x%08X (this frame; == w: %s)", ml_ok(cur) ? ml_r32(cur + s_mm->cur_focus) : 0,
             ml_ok(cur) && ml_r32(cur + s_mm->cur_focus) == w ? "yes" : "no");
}

struct MmState { uint32_t c, focused, root; int count; };
MmState mm_state(uint32_t c)
{
    MmState s = {c, 0, 0, 0};
    const int excl = vm_base[c + 0x20] != 0;
    const uint32_t vec = c + (excl ? 0x14u : 0x08u);
    const uint32_t n = ml_r32(vec), arr = ml_r32(vec + 4);
    if (n == 0 || n > 64 || !ml_ok(arr)) return s;
    s.count = (int)n;
    s.focused = ml_r32(arr);
    if (!ml_ok(s.focused)) { s.count = 0; return s; }
    if (excl && ml_ok(ml_r32(c + 0x190))) s.root = ml_r32(c + 0x190);   /* modal dialog: its subtree only */
    else {
        uint32_t t = s.focused;
        for (int k = 0; k < 32; k++) { const uint32_t p = ml_r32(t + 0x10); if (!ml_ok(p)) break; t = p; }
        s.root = t;
    }
    return s;
}

std::atomic_flag s_mm_busy = ATOMIC_FLAG_INIT;   /* never blocks a poll: another thread's poll skips instead */
PadMenuInject s_mm_inj;
PadMenuHover s_mm_hov;

/* Focus tag through the game's setter; 1 when the chain head is tag afterwards. */
int mm_focus(ppu_context* ctx, uint32_t c, uint32_t tag, const KZ3MenuRect& r, float x, float y, const char* why)
{
    const uint32_t ok = mm_call(ctx, s_mm->set_focus, s_mm_toc, c, tag, 0, s_mm->src);
    const MmState s = mm_state(c);
    KZ3_LOGI("menu-mouse", "%s: SetFocus(C=0x%08X, tag 0x%08X tag vt 0x%08X rect %d,%d-%d,%d) at %.0f,%.0f -> %u, focused 0x%08X",
             why, c, tag, ml_r32(tag), r.l, r.t, r.r, r.b, x, y, ok, s.focused);
    return s.focused == tag;
}

/* Lock for s_mm_inj (pad poll thread vs menu thread). The poll never waits (it skips the packet's extra bits for
 * one poll); the menu step spins, which is safe because the poll holds it for a few instructions, no guest calls. */
void mm_lock(void) { while (s_mm_busy.test_and_set(std::memory_order_acquire)) {} }
void mm_unlock(void) { s_mm_busy.clear(std::memory_order_release); }
volatile int s_mm_active = 0;
void mm_frame_tick(void) { mm_lock(); pmi_tick(&s_mm_inj); mm_unlock(); }
volatile double s_mm_pad_t = -1e9;   /* last poll with the pad's menu buttons or sticks in use (s) */

/* The hover and click step, on the MENU thread (run 2026-10-21 r93-mm2: the pad is polled on guest thread 15, the GUM
 * tags run on thread 1): called after a wrapped tag method returned (mm_wrap), at most every 10 ms, never nested. Its
 * SetFocus call is the same kind of call the game makes itself from a tag's event handler (0x0011B974 calls SetFocus
 * from inside the event dispatch), here between two tag methods on the same thread. */
void mm_menu_step(ppu_context* ctx)
{
    static double last = -1.0;
    static int inside = 0;
    if (inside || !s_mm_active) return;
    const double now = (double)GetTickCount64() / 1000.0;
    if (now - last < 0.010) return;
    last = now;
    const uint32_t c = s_mm_C;
    if (!c || !ml_ok(c)) return;
    MmState st = mm_state(c);
    if (st.count <= 0) return;
    inside = 1;
    pad_mouse_menu_script_step(now);
    PadMenuMouseEv ev[16];
    const int n = pad_mouse_menu_take(ev, 16);
    const float gx = g_mouse_guest.x, gy = g_mouse_guest.y;
    const int pad_active = now - s_mm_pad_t < 0.25;
    const int hover = pmh_should_hover(&s_mm_hov, g_mouse_guest.seq, g_mouse_guest.valid, pad_active);
    if (hover || n) {
        ppu_context save = *ctx;
        const uint32_t sp = ((uint32_t)save.gpr[1] - 0x400u) & ~0xFu;   /* a fresh frame below the live one */
        ctx->gpr[1] = sp;
        for (int k = 0; k < 8; k++) vm_base[sp + k] = (uint8_t)(save.gpr[1] >> (56 - 8 * k));   /* back chain */
        {   /* RE pass 146's checklist once, and item 6 (the bracket's widget) again one look later */
            static int checked = 0;
            if (checked < 2 && hover) {
                if (checked == 0) mm_checklist(ctx, c, st.focused);
                else {
                    const uint32_t q = mm_q(st.focused), cur = q ? ml_r32(q + 0x40) : 0;
                    const uint32_t w = q ? mm_widget(ctx, q, st.focused) : 0;
                    KZ3_LOGI("menu-mouse", "check 6 (later): cursor focused widget 0x%08X, focused tag's widget 0x%08X: %s",
                             ml_ok(cur) ? ml_r32(cur + s_mm->cur_focus) : 0, w,
                             ml_ok(cur) && w && ml_r32(cur + s_mm->cur_focus) == w ? "same" : "DIFFERENT");
                }
                checked++;
            }
        }
        if (hover) {
            const MmPick p = mm_pick(ctx, st.root, gx, gy);
            if (s_mm_log)
                KZ3_LOGI("menu-mouse", "look %.0f,%.0f: %d node(s), %d candidate(s), hit 0x%08X widget 0x%08X (focused 0x%08X)",
                         gx, gy, p.n_nodes, p.n_cands, p.tag, p.widget, st.focused);
            if (p.tag && p.tag != st.focused && mm_focus(ctx, c, p.tag, p.r, gx, gy, "hover")) st.focused = p.tag;
        }
        for (int i = 0; i < n; i++) {
            int hit = -1, deferred = 0;
            if (ev[i].kind == PMM_EV_LEFT && ev[i].valid) {
                const MmPick p = mm_pick(ctx, st.root, ev[i].x, ev[i].y);
                if (p.tag) {
                    hit = 0;
                    if (p.tag != st.focused) {
                        if (mm_focus(ctx, c, p.tag, p.r, ev[i].x, ev[i].y, "click")) st.focused = p.tag;
                        deferred = 1;
                    }
                }
            }
            const uint32_t fq = mm_q(st.focused), fw = fq ? mm_widget(ctx, fq, st.focused) : 0;
            const int k = fw ? mm_kind(fw) : -1;   /* the focused item's widget: Select (2) / Slider (3) take LEFT/RIGHT */
            int notches = ev[i].kind == PMM_EV_WHEEL ? ev[i].wheel : 1;
            if (notches > 4) notches = 4;
            if (notches < -4) notches = -4;
            mm_lock();
            if (deferred) pmi_defer(&s_mm_inj);
            for (int j = 0; j < (notches < 0 ? -notches : notches); j++) {
                const unsigned b = pmm_event_bits(ev[i].kind, notches < 0 ? -1 : 1, hit, k == 2 || k == 3);
                if (b && pmi_push(&s_mm_inj, b))
                    KZ3_LOGI("menu-mouse", "%s -> pad 0x%04X", ev[i].kind == PMM_EV_LEFT ? "left click" :
                             ev[i].kind == PMM_EV_RIGHT ? "right click" : "wheel", b);
            }
            mm_unlock();
        }
        *ctx = save;
    }
    inside = 0;
}

/* The pad poll (cellPad.c g_pad_menu_bits_hook, any thread): the menu state, the cursor rule, and the presses for
 * this packet. No guest calls here. */
unsigned kz3_menu_mouse_poll(unsigned pad_buttons, int pad_active)
{
    (void)pad_buttons;
    const uint32_t c = s_mm_C;
    MmState st = {0, 0, 0, 0};
    if (c && ml_ok(c)) st = mm_state(c);
    const int active = st.count > 0 && !pad_kb_capture_busy();
    const double now = (double)GetTickCount64() / 1000.0;
    if (pad_active) s_mm_pad_t = now;
    if (active != s_mm_active) {
        if (active) pmh_sync(&s_mm_hov, g_mouse_guest.seq);
        s_mm_active = active;
        pad_mouse_menu_set(active);
        KZ3_LOGI("menu-mouse", "menu %s (chain %d, focused 0x%08X)", active ? "up: pointer shown and free" : "gone", st.count,
                 st.focused);
    }
    if (s_mm_busy.test_and_set(std::memory_order_acquire)) return 0;
    if (!active) pmi_clear(&s_mm_inj);
    const unsigned bits = pmi_bits(&s_mm_inj);
    s_mm_busy.clear(std::memory_order_release);
    return bits;
}
void kz3_menu_mouse_sent(int delivered)
{
    if (s_mm_busy.test_and_set(std::memory_order_acquire)) return;
    const int was = s_mm_inj.sent;
    pmi_delivered(&s_mm_inj, delivered);
    const int ph = s_mm_inj.phase, now_sent = s_mm_inj.sent;
    const unsigned cur = s_mm_inj.cur;
    s_mm_busy.clear(std::memory_order_release);
    if (!was && now_sent)
        KZ3_LOGI("menu-mouse", "%s 0x%04X in a delivered pad packet", ph == 1 ? "press" : "release", cur);
}
} // namespace

static void kz3_menu_mouse_install(void)
{
    const char* e = getenv("KZ3_MENU_MOUSE");
    if (e && *e == '0') { printf("[menu-mouse] off (KZ3_MENU_MOUSE=0)\n"); return; }
    e = getenv("KZ3_MENU_MOUSE_LOG");
    s_mm_log = e && atoi(e) > 0;
    for (const MmVer& v : kMm) {
        const uint32_t opd = kz3_be32(v.nav_vt + 4 * 10);
        if (opd < 0x10000u || opd >= 0xF0000000u || kz3_be32(opd) != v.nav_fn) continue;
        s_mm = &v;
        s_mm_toc = kz3_be32(opd + 4);
        s_mm_n = 0;
        for (int pass = 0; pass < 2; pass++) {
            const uint32_t vt = pass ? v.nav_vt : v.base_vt;
            const int* sl = pass ? kMmNavSlots : kMmBaseSlots;
            const int ns = pass ? (int)(sizeof kMmNavSlots / sizeof *kMmNavSlots) : (int)(sizeof kMmBaseSlots / sizeof *kMmBaseSlots);
            for (int k = 0; k < ns && s_mm_n < kMmWrapMax; k++) {
                const uint32_t o = kz3_be32(vt + 4 * (uint32_t)sl[k]), code = o >= 0x10000u ? kz3_be32(o) : 0;
                bool dup = false;
                for (int j = 0; j < s_mm_n; j++) dup |= s_mm_addr[j] == code;
                void (*orig)(ppu_context*) = code ? kz3_lifted_fn(code) : nullptr;
                if (dup || !orig) continue;
                s_mm_addr[s_mm_n] = code;
                s_mm_orig[s_mm_n] = orig;
                s_mm_is_event[s_mm_n] = sl[k] == 10;
                ppu_register_function(code, kMmWraps[s_mm_n]);
                s_mm_n++;
            }
        }
        /* The frame tick: slot 4 of the Gum class vtable 0x00EAB5D0 (1.14 0x00F253B8), an empty method the menu thread
         * calls once per frame (run r93-ic1, PS3_ICALL_GATE: 1677 calls in 1677 frames on thread 1, from boot on). */
        if (kz3_be32(v.tick) == 0x4E800020u && s_mm_n < kMmWrapMax) {   /* blr */
            void (*orig)(ppu_context*) = kz3_lifted_fn(v.tick);
            if (orig) {
                s_mm_addr[s_mm_n] = v.tick;
                s_mm_orig[s_mm_n] = orig;
                s_mm_is_event[s_mm_n] = 0;
                s_mm_tick_idx = s_mm_n;
                ppu_register_function(v.tick, kMmWraps[s_mm_n]);
                s_mm_n++;
            }
        }
        if (s_mm_tick_idx < 0) printf("[menu-mouse] WARNING: frame tick 0x%08X not found: no hover or clicks\n", v.tick);
        g_pad_menu_bits_hook = kz3_menu_mouse_poll;
        g_pad_menu_sent_hook = kz3_menu_mouse_sent;
        printf("[menu-mouse] on (%s): SetFocus 0x%08X, %d tag method(s) wrapped for the GUM context, TOC 0x%08X\n", v.ver,
               v.set_focus, s_mm_n, s_mm_toc);
        return;
    }
    printf("[menu-mouse] NOT installed (no known EBOOT layout)\n");
}

static void kz3_kb_page_init(void)
{
    const char* e = getenv("KZ3_KB_PAGE");
    if (e && !strcmp(e, "0")) { printf("[kb-page] off (KZ3_KB_PAGE=0)\n"); return; }
    if (!kz3_kb_bridge_install()) return;
    const char* t = getenv("KZ3_KB_PAGE_TEST");
    s_kb_page_test = t && !strcmp(t, "1");
    s_kb_dir = std::string(kz3_cache_root()) + "/psarc-cache/kz3-ui";
    for (char& c : s_kb_dir) if (c == '\\') c = '/';
#ifdef _WIN32
    _mkdir((std::string(kz3_cache_root()) + "/psarc-cache").c_str());
    _mkdir(s_kb_dir.c_str());
#else
    mkdir((std::string(kz3_cache_root()) + "/psarc-cache").c_str(), 0755);
    mkdir(s_kb_dir.c_str(), 0755);
#endif
    InitializeCriticalSection(&s_kb_vfs_lock);
    s_kb_prev_miss = g_ps3_vfs_miss_hook;
    g_ps3_vfs_miss_hook = kz3_kb_miss_hook;
}

static void start_diagnostics(void)
{
    const char* wd = getenv("KZ3_WATCHDOG");
    if (wd && *wd) CreateThread(NULL, 0, kz3_watchdog, (LPVOID)wd, 0, NULL);
#ifdef _WIN32
    ps3_sampler_start();     /* no-op unless PS3_SAMPLE is set */
#endif
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * logging: the file log in <exe dir>/logs, the fatal-error notice, the crash notice
 * -----------------------------------------------------------------------*/
static int kz3_notice_wanted(void)
{
    /* Test runs (KZ3_LOG_STDOUT=1, hidden display) and KZ3_NO_ERROR_BOX=1 must never block on a dialog;
     * KZ3_ERROR_NOTICE=1 forces the notice on anyway (screenshots of it in hidden runs). */
    const char* e = getenv("KZ3_NO_ERROR_BOX");
    if (e && *e && *e != '0') return 0;
    e = getenv("KZ3_ERROR_NOTICE");
    if (e && *e == '1') return 1;
    e = getenv("KZ3_LOG_STDOUT");
    return !(e && *e == '1');
}

#ifdef _WIN32
/* Game-styled error notice (RSX pass 33): an own top-most borderless window painted with GDI in
 * the game's menu look (black panel, red rails and highlight bar, pale-amber condensed caps, a dim prompt row; see
 * a progress screenshot). It depends on nothing of the renderer, which is
 * exactly what is suspect at crash time; MessageBoxA remains the fallback when the window cannot be created.
 * Dismissed by Enter / Esc / Space / a click / gamepad A, B or Start (XInput loaded at run time), or after
 * KZ3_ERROR_NOTICE_TIMEOUT=<s> (unattended runs). Runs its message loop on the calling (fatal or crashing) thread,
 * like MessageBox would. Pass 38 (ui-review rows 6-9): Rajdhani loaded privately, white message on the red bar in two
 * lines, one prompt, DPI-scaled and centred on the game's monitor. The full version draws with the game's own glyph
 * atlas (design in the RE notes pass 33). */
struct kz3_notice_t {
    const char* what; const char* log; const char* crash;
    ULONGLONG deadline; const char* closed_by;
};
typedef DWORD (WINAPI *xi_getstate_fn)(DWORD, void*);
static xi_getstate_fn kz3_xi_getstate;

/* Fonts (pass 38, ui-style.md "Fonts"): Rajdhani Bold/Medium from <exe dir>\fonts, loaded privately (FR_PRIVATE:
 * this process only, never installed); without the files the Windows UI font, unsqueezed. */
static const char* kz3_notice_face(int bold)
{
    static int loaded = -1;
    static char face_ok[2];
    if (loaded < 0) {
        char dir[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
        while (n > 0 && dir[n - 1] != '\\' && dir[n - 1] != '/') n--;
        dir[n] = 0;
        const char* files[2] = { "fonts\\Rajdhani-Medium.ttf", "fonts\\Rajdhani-Bold.ttf" };
        for (int i = 0; i < 2; i++) {
            char path[MAX_PATH + 40];
            snprintf(path, sizeof path, "%s%s", dir, files[i]);
            face_ok[i] = AddFontResourceExA(path, FR_PRIVATE, 0) > 0;
        }
        loaded = 1;
        KZ3_LOGI("notice", "Rajdhani %s (fonts\\Rajdhani-Medium.ttf %s, -Bold.ttf %s)",
                 face_ok[0] || face_ok[1] ? "loaded privately" : "missing: Windows UI font", face_ok[0] ? "ok" : "no",
                 face_ok[1] ? "ok" : "no");
    }
    return face_ok[bold ? 1 : 0] ? "Rajdhani" : "Segoe UI";     /* kz3_notice_font checks what GDI really chose */
}

/* 1 when a font family of exactly this name is installed (a substituted name is not: Wine maps "Segoe UI" to a
 * serif face and GetTextFace still reports "Segoe UI", r38-n4). */
static int CALLBACK kz3_notice_face_cb(const LOGFONTA*, const TEXTMETRICA*, DWORD, LPARAM found)
{
    *(int*)found = 1;
    return 0;
}
static int kz3_notice_face_exists(const char* face)
{
    LOGFONTA lf; memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", face);
    int found = 0;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExA(dc, &lf, (FONTENUMPROCA)kz3_notice_face_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static HFONT kz3_notice_font(int height, int bold)
{
    /* Without Rajdhani: Segoe UI (Windows), else Tahoma (Wine's UI font), else Arial, whichever is installed. */
    const char* face = kz3_notice_face(bold);
    if (strcmp(face, "Rajdhani")) {
        static const char* fb = NULL;
        if (!fb) {
            const char* order[3] = { "Segoe UI", "Tahoma", "Arial" };
            fb = "Arial";
            for (int i = 0; i < 3; i++) if (kz3_notice_face_exists(order[i])) { fb = order[i]; break; }
            KZ3_LOGI("notice", "fallback face %s", fb);
        }
        face = fb;
    }
    return CreateFontA(-height, 0, 0, 0, bold ? FW_BOLD : FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}
/* Keycap prompts (RSX pass 39, patch 2039 hooks D/E): the live-draw texture cache swaps the pad-button cells
 * of the game's two prompt atlases for keycaps while the keyboard is in use. It asks here for the bound keys
 * (cellPad.c) and for the label text, drawn with the UI face (Rajdhani Bold, the notice's fallbacks without it) in
 * grey-scale anti-aliasing into a coverage mask. */
extern "C" int         pad_kb_last_device_keyboard(void);                 /* cellPad.c (patch 2038) */
extern "C" int         pad_kb_key_for_input(const char* name, int slot);  /* cellPad.c (patch 2039) */
extern "C" const char* pad_kb_key_label(int vk);                          /* cellPad.c (patch 2038) */
struct rsx_keycap_source {
    int (*keyboard_active)(void);
    int (*key_for_input)(const char*, int);
    const char* (*key_name)(int);
    int (*text_mask)(const char*, int, unsigned char*, int, int);
    int (*text_mask_thin)(const char*, int, unsigned char*, int, int);   /* patch 2094: the thin, letter-spaced label face */
    int (*pad_variant)(void);                                           /* patch 2094: 1 = DualSense */
    int (*layout_l1_unused)(void);                                      /* patch 2117: 1 = Modern (no L1 line) */
};
extern "C" int pad_prompt_device(void);                                  /* cellPad.c (patch 4011) */
extern "C" void rsx_keycaps_set_source(const rsx_keycap_source* s);       /* rsx_live_draw.c (patch 2039) */

/* bold = Rajdhani Bold (keycaps); thin = Rajdhani Medium with letter spacing of 0.10 em (choice 1: the button
 * labels in a thin face like the game's own SELECT / START labels). ASSUMPTION: the game's label face is not a
 * font the port can load (the labels are atlas pixels), so Rajdhani Medium is the nearest the port ships. */
static int kz3_label_text(const char* text, int max_px, unsigned char* mask, int w, int h, int bold)
{
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(dc); return 0; }
    HGDIOBJ obm = SelectObject(dc, bm);
    const int n = (int)strlen(text);
    int em = (int)(max_px / 0.70f + 0.5f), drawn = 0;   /* Rajdhani's capitals are about 0.7 em */
    for (; em >= 6 && !drawn; em--) {
        HFONT base = kz3_notice_font(em, bold);
        LOGFONTA lf; GetObjectA(base, sizeof lf, &lf); DeleteObject(base);
        lf.lfQuality = ANTIALIASED_QUALITY;                /* grey-scale coverage, no ClearType fringes */
        HFONT f = CreateFontIndirectA(&lf);
        HGDIOBJ of = SelectObject(dc, f);
        const int track = (!bold && n > 1) ? (int)(em * 0.10f + 0.5f) : 0;
        SetTextCharacterExtra(dc, track);
        SIZE sz; GetTextExtentPoint32A(dc, text, n, &sz);
        if (track) sz.cx -= track;                          /* no spacing after the last letter */
        if ((sz.cx <= w && sz.cy <= h + em / 4) || em == 6) {
            memset(bits, 0, (size_t)w * h * 4);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            TextOutA(dc, (w - sz.cx) / 2, (h - sz.cy) / 2, text, n);
            GdiFlush();
            const uint8_t* px = (const uint8_t*)bits;
            for (int i = 0; i < w * h; i++) mask[i] = px[i * 4 + 1];
            drawn = 1;
        }
        SelectObject(dc, of); DeleteObject(f);
    }
    SelectObject(dc, obm); DeleteObject(bm); DeleteDC(dc);
    return drawn;
}

static int kz3_keycap_text(const char* t, int m, unsigned char* k, int w, int h) { return kz3_label_text(t, m, k, w, h, 1); }
static int kz3_label_thin(const char* t, int m, unsigned char* k, int w, int h) { return kz3_label_text(t, m, k, w, h, 0); }
/* 1 when the prompts are for a DualSense (PAD_DEV_PS5 = 4 in cellPad.c patch 4011); KZ3_PROMPTS_PS5=1 forces it (tests). */
static int kz3_pad_variant(void)
{
    static int force = -1;
    if (force < 0) { const char* e = getenv("KZ3_PROMPTS_PS5"); force = (e && e[0] == '1') ? 1 : 0; }
    return force || pad_prompt_device() == 4;
}

/* Patch 2106: the menu legends' Back key is the "back" input's (Esc) only while the port tracks the game's menus
 * (2098's pad hook installed); without it Esc stays Start in menus, so the legend keeps Circle's own key. */
extern "C" unsigned (*g_pad_menu_bits_hook)(unsigned pad_buttons, int pad_active);
static int kz3_kc_key_for_input(const char* name, int slot)
{
    if (!strcmp(name, "back") && !g_pad_menu_bits_hook) return 0;
    return pad_kb_key_for_input(name, slot);
}

/* Patch 2117 (RSX pass 110): 1 while the pad layout is Modern (the Shadow Fall layout, net rounds 35/39: no on-foot
 * action on L1), so the controls page's picture leaves out L1's leader line. The layout is keybinds.ini [pad] (the
 * Options row "Controller layout", read through pad_kb_ui_cmd "padmap") or KZ3_PAD_PRESET (cellPad.c's override).
 * Called once per frame by the live draw: re-read at most every 250 ms. */
static int kz3_layout_l1_unused(void)
{
    static int s_env = -1, s_v;
    static ULONGLONG s_t;
    if (s_env < 0) { const char* e = getenv("KZ3_PAD_PRESET"); s_env = !(e && *e) ? 2 : _stricmp(e, "modern") == 0; }
    if (s_env != 2) return s_env;
    const ULONGLONG now = GetTickCount64();
    if (s_t && now - s_t < 250) return s_v;
    s_t = now;
    char o[32] = "";
    pad_kb_ui_cmd("padmap", o, sizeof o);
    const int v = _stricmp(o, "modern") == 0;
    if (v != s_v) { printf("[keycap] controls picture: %s\n", v ? "Modern layout, no L1 line" : "the game's lines"); fflush(stdout); }
    s_v = v;
    return v;
}

static void kz3_keycaps_register(void)
{
    static const rsx_keycap_source src = { pad_kb_last_device_keyboard, kz3_kc_key_for_input, pad_kb_key_label,
                                           kz3_keycap_text, kz3_label_thin, kz3_pad_variant, kz3_layout_l1_unused };
    rsx_keycaps_set_source(&src);
}

static int s_notice_dpi = 96;
#define NS(v) MulDiv((v), s_notice_dpi, 96)     /* layout pixels at 96 dpi -> this monitor */

static void kz3_notice_paint(HWND h, const kz3_notice_t* n)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT r; GetClientRect(h, &r);
    /* ui-style.md colour tokens: game red, amber (on black), dim red (prompts), white (text on red). */
    const COLORREF red = RGB(0xD6, 0x1C, 0x14), amber = RGB(0xF6, 0xDE, 0x96), dim = RGB(0xB0, 0x2A, 0x1E),
                   white = RGB(0xF0, 0xF0, 0xF0);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)), rb = CreateSolidBrush(red), wb = CreateSolidBrush(white);
    FillRect(dc, &r, black);
    RECT q;
    q = { NS(18), NS(22), NS(21), r.bottom - NS(22) }; FillRect(dc, &q, rb);       /* the menu's red rails */
    q = { r.right - NS(21), NS(22), r.right - NS(18), r.bottom - NS(22) }; FillRect(dc, &q, rb);
    q = { NS(44), NS(32), NS(47), NS(70) }; FillRect(dc, &q, wb);                  /* bracketed title: [ icon TITLE ] */
    q = { NS(56), NS(38), NS(84), NS(66) }; FillRect(dc, &q, rb);
    q = { NS(62), NS(44), NS(78), NS(60) }; FillRect(dc, &q, black);
    q = { NS(66), NS(48), NS(74), NS(56) }; FillRect(dc, &q, rb);
    SetBkMode(dc, TRANSPARENT);
    HFONT title = kz3_notice_font(NS(38), 1), body = kz3_notice_font(NS(22), 1), small = kz3_notice_font(NS(19), 0),
          prompt = kz3_notice_font(NS(19), 1);
    HGDIOBJ old = SelectObject(dc, title);
    SetTextColor(dc, white);
    const char* t = "KILLZONE 3 STOPPED";
    TextOutA(dc, NS(96), NS(31), t, (int)strlen(t));
    SIZE ts; GetTextExtentPoint32A(dc, t, (int)strlen(t), &ts);
    q = { NS(96) + ts.cx + NS(10), NS(32), NS(96) + ts.cx + NS(13), NS(70) }; FillRect(dc, &q, wb);
    q = { NS(44), NS(80), r.right - NS(44), NS(82) }; FillRect(dc, &q, rb);
    SelectObject(dc, small); SetTextColor(dc, amber);
    t = "THE GAME MET AN ERROR AND HAD TO STOP";
    TextOutA(dc, NS(46), NS(90), t, (int)strlen(t));
    q = { NS(44), NS(118), r.right - NS(44), NS(178) }; FillRect(dc, &q, rb);     /* the highlighted item bar */
    SelectObject(dc, body); SetTextColor(dc, white);                                /* white on red, two lines */
    char what[600];
    snprintf(what, sizeof what, "%s", n->what ? n->what : "");
    what[0] = (char)toupper((unsigned char)what[0]);
    RECT tr = { NS(56), NS(122), r.right - NS(56), NS(174) };
    RECT mr = tr;
    DrawTextA(dc, what, -1, &mr, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    if (mr.bottom - mr.top < tr.bottom - tr.top) tr.top += (tr.bottom - tr.top - (mr.bottom - mr.top)) / 2;
    DrawTextA(dc, what, -1, &tr, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, small);
    int y = NS(192);
    const char* rows[2][2] = { { "LOG", n->log }, { "CRASH REPORT", n->crash } };
    SIZE lw; GetTextExtentPoint32A(dc, rows[1][0], (int)strlen(rows[1][0]), &lw);   /* the wider label sets the column */
    int vx = NS(46) + lw.cx + NS(18);
    if (vx < NS(176)) vx = NS(176);
    for (int i = 0; i < 2; i++, y += NS(28)) {
        SetTextColor(dc, red); TextOutA(dc, NS(46), y, rows[i][0], (int)strlen(rows[i][0]));
        SetTextColor(dc, amber);
        tr = { vx, y, r.right - NS(46), y + NS(26) };
        DrawTextA(dc, rows[i][1] && *rows[i][1] ? rows[i][1] : "(none)", -1, &tr, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, prompt); SetTextColor(dc, dim);                               /* one prompt, every input that closes */
    q = { NS(46), r.bottom - NS(54), NS(60), r.bottom - NS(40) }; FillRect(dc, &q, rb);
    t = "ENTER / ESC / (A)  CLOSE"; TextOutA(dc, NS(68), r.bottom - NS(58), t, (int)strlen(t));
    SelectObject(dc, old);
    DeleteObject(title); DeleteObject(body); DeleteObject(small); DeleteObject(prompt);
    DeleteObject(black); DeleteObject(rb); DeleteObject(wb);
    EndPaint(h, &ps);
}

/* The game's window (the first visible top-level window of this process that is not the notice), or NULL. */
static BOOL CALLBACK kz3_notice_find_game(HWND h, LPARAM out)
{
    DWORD pid = 0; char cls[32] = { 0 };
    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, cls, sizeof cls);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && strcmp(cls, "KZ3Notice")) { *(HWND*)out = h; return FALSE; }
    return TRUE;
}

static LRESULT CALLBACK kz3_notice_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    kz3_notice_t* n = (kz3_notice_t*)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (m) {
    case WM_PAINT: if (n) kz3_notice_paint(h, n); return 0;
    case WM_KEYDOWN:
        if (w == VK_RETURN || w == VK_ESCAPE || w == VK_SPACE) { if (n) n->closed_by = "key"; DestroyWindow(h); }
        return 0;
    case WM_LBUTTONDOWN: if (n) n->closed_by = "click"; DestroyWindow(h); return 0;
    case WM_TIMER:
        if (n && n->deadline && GetTickCount64() >= n->deadline) { n->closed_by = "timeout"; DestroyWindow(h); return 0; }
        if (kz3_xi_getstate) {
            struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
            for (DWORD i = 0; i < 4; i++)
                if (kz3_xi_getstate(i, &st) == 0 && (st.buttons & (0x1000 | 0x2000 | 0x0010))) {   /* A, B, Start */
                    if (n) n->closed_by = "gamepad";
                    DestroyWindow(h); return 0;
                }
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void kz3_notice_relpath(const char* path, char* out, size_t cap)
{
    const char* dir = kz3_log_dir();                       /* "<exe dir>/logs" */
    size_t dl = strlen(dir);
    if (*path && dl && !strncmp(path, dir, dl) && (path[dl] == '/' || path[dl] == '\\'))
        snprintf(out, cap, "logs%s", path + dl);
    else
        snprintf(out, cap, "%s", path);
    for (char* c = out; *c; c++) if (*c == '/') *c = '\\';
}

static int kz3_notice_show(const char* what, const char* crash_file)
{
    static LONG busy;
    if (InterlockedExchange(&busy, 1)) { KZ3_LOGW("notice", "a notice is already up; not showing '%s'", what); return 1; }
    static int registered;
    if (!registered) {
        WNDCLASSEXA wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = kz3_notice_proc; wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "KZ3Notice";
        registered = RegisterClassExA(&wc) != 0;
        /* System32 only (net 31): no planted XInput DLL from the exe folder or working directory. */
        HMODULE xi = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!xi) xi = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (xi) kz3_xi_getstate = (xi_getstate_fn)(void*)GetProcAddress(xi, "XInputGetState");
    }
    if (!registered) { InterlockedExchange(&busy, 0); return 0; }
    /* Paths relative to the exe folder ("logs\Killzone3-PC-....log"): nicer for players, and the full path (home folder,
     * user name) stays in the log file only, so a screenshot of the notice can be published. */
    char rel_log[600], rel_crash[600];
    kz3_notice_relpath(kz3_log_file_path(), rel_log, sizeof rel_log);
    kz3_notice_relpath(crash_file && *crash_file ? crash_file : "", rel_crash, sizeof rel_crash);
    kz3_notice_t n = { what, rel_log, rel_crash, 0, "?" };
    const char* to = getenv("KZ3_ERROR_NOTICE_TIMEOUT");
    if (to && atoi(to) > 0) n.deadline = GetTickCount64() + (ULONGLONG)atoi(to) * 1000u;
    /* Pass 38 (ui-style.md "Layout"): scaled by the game window's DPI and centred on its monitor's work area
     * (the primary monitor when the game has no window yet). GetDpiForWindow is Windows 10 1607+: looked up. */
    HWND game = NULL;
    EnumWindows(kz3_notice_find_game, (LPARAM)&game);
    typedef UINT (WINAPI *dpi_fn)(HWND);
    dpi_fn get_dpi = (dpi_fn)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT dpi = game && get_dpi ? get_dpi(game) : 0;
    s_notice_dpi = dpi >= 96 && dpi <= 480 ? (int)dpi : 96;
    MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    HMONITOR mon = game ? MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoA(mon, &mi)) mi.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    const int W = NS(760), H = NS(320);
    int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top;
    int x = mi.rcWork.left + (mw > W ? (mw - W) / 2 : 0), y = mi.rcWork.top + (mh > H ? (mh - H) / 2 : 0);
    KZ3_LOGI("notice", "%d dpi, %dx%d at %d,%d (game window %s)", s_notice_dpi, W, H, x, y, game ? "found" : "none");
    HWND h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "KZ3Notice", "Project Helghast", WS_POPUP,
                             x, y, W, H, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!h) { KZ3_LOGW("notice", "CreateWindow failed (%lu), falling back to MessageBox", (unsigned long)GetLastError()); InterlockedExchange(&busy, 0); return 0; }
    SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)&n);
    ShowWindow(h, SW_SHOWNORMAL); SetForegroundWindow(h); UpdateWindow(h);
    SetTimer(h, 1, 100, NULL);
    KZ3_LOGI("notice", "game-styled notice up: '%s' (timeout %s s)", what, to && atoi(to) > 0 ? to : "none");
    ULONGLONG t0 = GetTickCount64();
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    KZ3_LOGI("notice", "closed by %s after %llu ms", n.closed_by, (unsigned long long)(GetTickCount64() - t0));
    InterlockedExchange(&busy, 0);
    return 1;
}
#endif

static void kz3_show_error(const char* what, const char* crash_file)
{
#ifdef _WIN32
    if (!kz3_notice_wanted()) return;
    if (kz3_notice_show(what, crash_file)) return;
    /* Fallback: the plain system message box (window class or window creation failed). */
    char msg[1600];
    snprintf(msg, sizeof msg, "Project Helghast stopped: %s\n\nLog: %s\nCrash report: %s", what, kz3_log_file_path(),
             crash_file && *crash_file ? crash_file : "(none)");
    MessageBoxA(NULL, msg, "Project Helghast", MB_OK | MB_ICONERROR | MB_TOPMOST);
#else
    (void)what; (void)crash_file;
#endif
}

static int kz3_fatal_box(const char* message, const char* crash_file, void*)
{
    kz3_show_error(message, crash_file);
    return 0;   /* stop the process (exit code KZ3_LOG_FATAL_EXIT) */
}

/* Called by ppu_crash_report (patch 2030) after the crash file and the minidump are written. */
extern "C" void kz3_crash_notify(const char* crash_file)
{
    kz3_show_error("unexpected error (unhandled exception)", crash_file);
    kz3_log_shutdown();
}

/* Test triggers for the fatal paths (RSX pass 31): KZ3_TEST_FATAL=<s> calls kz3_log_fatal after s seconds,
 * KZ3_TEST_CRASH=<s> makes a host access violation (the unhandled-exception path). */
static DWORD WINAPI kz3_test_fault(LPVOID p)
{
    const int crash = (int)(intptr_t)p;
    const char* e = getenv(crash ? "KZ3_TEST_CRASH" : "KZ3_TEST_FATAL");
    Sleep((DWORD)(atoi(e) * 1000));
    kz3_log_set_thread_label("test");
    if (!crash) { kz3_log_fatal("test", "KZ3_TEST_FATAL=%s: forced fatal error", e); return 0; }
    KZ3_LOGE("test", "KZ3_TEST_CRASH=%s: forcing a host access violation", e);
    *(volatile int*)(uintptr_t)8 = 1;
    return 0;
}

static char s_exe_dir[1024] = ".";   /* the exe's folder (ANSI code page), set by kz3_logging_start */

/* Writes the three mouse options into <exe dir>/kz3.ini [input] (pad_mouse_ini_upsert keeps every other line). */
static void kz3_mouse_opts_save(void)
{
    static char in[16384], out[16384 + 512];
    char path[1100], tmp[1110];
    snprintf(path, sizeof path, "%s/kz3.ini", s_exe_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (kz3_ini_read_file(path, in, sizeof in) < 0) in[0] = 0;
    float u, z; int inv;
    pad_mouse_opts_get(&u, &z, &inv);
    char vals[3][32];
    pad_mouse_opt_fmt(vals[0], sizeof vals[0], u);
    pad_mouse_opt_fmt(vals[1], sizeof vals[1], z);
    snprintf(vals[2], sizeof vals[2], "%d", inv);
    for (int i = 0; i < 3; i++) {
        if (pad_mouse_ini_upsert(in, out, sizeof out, pad_mouse_ini_keys[i].key, vals[i]) < 0) {
            KZ3_LOGW("mouse", "kz3.ini too large to update: %s not saved", pad_mouse_ini_keys[i].key);
            return;
        }
        memcpy(in, out, strlen(out) + 1);
    }
    bool ok = kz3_write_file(tmp, in);
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    KZ3_LOGI("mouse", "options saved to %s: sensitivity=%s ads=%s invert=%d%s", path, vals[0], vals[1], inv, ok ? "" : " (WRITE FAILED)");
}

static void kz3_logging_start(void)
{
    char* dir = s_exe_dir;
#ifdef _WIN32
    /* ENHANCEMENT: kz3.exe is a GUI-subsystem program, so no console window opens beside the game. KZ3_CONSOLE=1 opens
     * one (log lines echoed to it unless KZ3_LOG_STDOUT says otherwise). A redirected stdout (the test runners) is
     * inherited either way; the log file in logs/ is always written. */
    {
        const char* c = getenv("KZ3_CONSOLE");
        if (c && c[0] == '1' && !GetConsoleWindow() && AllocConsole()) {
            FILE* f;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            if (!getenv("KZ3_LOG_STDOUT")) _putenv("KZ3_LOG_STDOUT=1");
        }
    }
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n && n < 1024) {
        while (n && w[n - 1] != L'\\' && w[n - 1] != L'/') n--;   /* strip the exe name */
        if (n > 1) {
            w[n - 1] = 0;
            /* kz3_log opens files with the ANSI API, so the ANSI code page. */
            if (!WideCharToMultiByte(CP_ACP, 0, w, -1, dir, (int)sizeof s_exe_dir, NULL, NULL)) snprintf(dir, sizeof s_exe_dir, ".");
        }
    }
#endif
    kz3_log_init(dir, KZ3_LOG_F_STDERR_ERRORS);
    kz3_log_set_thread_label("main");
    kz3_log_set_fatal_hook(kz3_fatal_box, NULL);
    if (getenv("KZ3_TEST_FATAL")) CreateThread(NULL, 0, kz3_test_fault, (LPVOID)0, 0, NULL);
    if (getenv("KZ3_TEST_CRASH")) CreateThread(NULL, 0, kz3_test_fault, (LPVOID)1, 0, NULL);
    if (getenv("KZ3_TEST_CLOSE")) CreateThread(NULL, 0, kz3_test_close, NULL, 0, NULL);
}

static void kz3_setenv(const char* k, const char* v)
{
#ifdef _WIN32
    char kv[1200]; snprintf(kv, sizeof kv, "%s=%s", k, v); _putenv(kv);
#else
    setenv(k, v, 1);
#endif
}

/* Set by kz3_boot_inputs when the EBOOT built into the exe is the one to load (kz3_load_eboot). */
static int s_eboot_embedded;

/* No launch options (release pass 26): the EBOOT is argv[1] if given, else the one built into the exe (release step
 * 9s: the first-run build links the player's own EBOOT.elf, kz3_embed.h), else `eboot` in kz3.ini next to the exe, else
 * <exe dir>/PS3_GAME/USRDIR/EBOOT.elf (a dump folder with kz3.exe dropped in, e.g. a Steam shortcut to it). kz3.ini's
 * other keys fill the environment variables that are not set yet (kz3_ini.h; written by tools/release/firstrun.py)
 * and its data_dir becomes the working folder. No EBOOT found: the game-styled notice, not a usage line.
 * -> the EBOOT path (static storage; with the built-in EBOOT only the VFS root is derived from it). */
static const char* kz3_boot_inputs(int argc, char** argv)
{
    static char eboot[1100], text[16384];
    char ini[1100], v[1024], abs[1100];
    snprintf(ini, sizeof ini, "%s/kz3.ini", s_exe_dir);
    const int have_ini = kz3_ini_read_file(ini, text, sizeof text) >= 0;
    if (!have_ini) text[0] = 0;
    KZ3_LOGI("boot", "kz3.ini %s", have_ini ? "read" : "not found (optional)");
    auto ini_to_env = [&](const char* key, const char* env, int path) {
        if (!env || !kz3_ini_get(text, key, v, sizeof v)) return;
        const char* e = getenv(env);
        if (e && *e) { KZ3_LOGI("boot", "kz3.ini %s: %s is set, the environment wins", key, env); return; }
        if (path && !kz3_ini_resolve(s_exe_dir, v, abs, sizeof abs)) return;
        kz3_setenv(env, path ? abs : v);
        KZ3_LOGI("boot", "kz3.ini %s -> %s=%s", key, env, path ? abs : v);
    };
    for (size_t i = 0; i < KZ3_INI_NKEYS; i++) ini_to_env(kz3_ini_keys[i].key, kz3_ini_keys[i].env, kz3_ini_keys[i].path);
    /* RSX pass 94 (patch 2099): [input] mouse_sensitivity / mouse_ads_sensitivity / mouse_invert_y, table next to the
     * switches in pad_mouse.h (host test tools/smoke/test_mouse_look.sh); same rules as kz3_ini_keys. */
    for (size_t i = 0; i < PAD_MOUSE_NINI; i++) ini_to_env(pad_mouse_ini_keys[i].key, pad_mouse_ini_keys[i].env, 0);
    const char* from;
    const unsigned char* blob;
    size_t blob_size;
    if (argc >= 2) {
        from = "the command line";
#ifdef _WIN32
        if (!_fullpath(eboot, argv[1], sizeof eboot))
#endif
            snprintf(eboot, sizeof eboot, "%s", argv[1]);
    } else if (kz3_embedded_eboot(&blob, &blob_size)) {
        /* kz3.ini's eboot line (installs from before step 9s) only places the VFS root now, when vfs_root is unset. */
        from = "the exe (built in)";
        s_eboot_embedded = 1;
        if (!(kz3_ini_get(text, "eboot", v, sizeof v) && kz3_ini_resolve(s_exe_dir, v, eboot, sizeof eboot)))
            snprintf(eboot, sizeof eboot, "%s/PS3_GAME/USRDIR/EBOOT.elf", s_exe_dir);
    } else if (kz3_ini_get(text, "eboot", v, sizeof v) && kz3_ini_resolve(s_exe_dir, v, eboot, sizeof eboot)) {
        from = "kz3.ini";
    } else {
        from = "beside the exe";
        snprintf(eboot, sizeof eboot, "%s/PS3_GAME/USRDIR/EBOOT.elf", s_exe_dir);
    }
    KZ3_LOGI("boot", "EBOOT from %s: %s", from, s_eboot_embedded ? "(in kz3.exe)" : eboot);
    if (s_eboot_embedded) {
        /* nothing to open */
    } else if (FILE* f = fopen(eboot, "rb")) {
        fclose(f);
    } else if (argc >= 2) {   /* notice texts stay one line: the notice's message box shows one Rajdhani line */
        kz3_log_fatal("boot", "the EBOOT.elf on the command line was not found (see the log)");
    } else if (have_ini && kz3_ini_get(text, "eboot", v, sizeof v)) {
        KZ3_LOGE("boot", "fix: correct the eboot line in %s, or run the launcher's \"Verify and rebuild\"", ini);
        kz3_log_fatal("boot", "the EBOOT.elf named in kz3.ini was not found (see the log)");
    } else {
        KZ3_LOGE("boot", "fix: start the port from its launcher, or write eboot = <path of your decrypted EBOOT.elf> "
                 "in %s, or put kz3.exe next to the PS3_GAME folder of your dump", ini);
        kz3_log_fatal("boot", "no game files found: set eboot in kz3.ini or use the launcher");
    }
    if (kz3_ini_get(text, "data_dir", v, sizeof v) && kz3_ini_resolve(s_exe_dir, v, abs, sizeof abs)) {
#ifdef _WIN32
        CreateDirectoryA(abs, NULL);
        const int rc = _chdir(abs);
#else
        const int rc = chdir(abs);
#endif
        if (rc == 0) KZ3_LOGI("boot", "kz3.ini data_dir: working folder %s", abs);
        else KZ3_LOGW("boot", "kz3.ini data_dir: cannot enter %s; the working folder stays", abs);
    }
    return eboot;
}

/* Load the EBOOT into guest memory -> its entry OPD, or 0. The built-in one (release step 9s) loads only when its
 * bytes still hash to what the build recorded; an exe with a built-in EBOOT still takes an external one on its
 * command line (tests) and says so when that file differs from the one its code was lifted from. */
static uint32_t kz3_load_eboot(const char* path)
{
    const unsigned char* blob;
    size_t size;
    const char* want = kz3_embedded_eboot(&blob, &size);
    char hex[65];
    if (s_eboot_embedded) {
        kz3_sha256_hex(blob, size, hex);
        if (strcmp(hex, want) != 0) {
            KZ3_LOGE("boot", "the EBOOT built into kz3.exe hashes to %s, the build recorded %s", hex, want);
            KZ3_LOGE("boot", "fix: run the launcher's \"Verify and rebuild\"");
            kz3_log_fatal("boot", "kz3.exe is damaged: run Verify and rebuild (see the log)");
            return 0;
        }
        KZ3_LOGI("boot", "EBOOT built into kz3.exe: %zu bytes, SHA-256 %s (%s), hash check passed", size, hex,
                 kz3_eboot_version_of(hex));
        return ppu_load_elf_image(blob, size);
    }
    if (!want) return ppu_load_elf(path);
    /* An external ELF on an exe with a built-in one: read it, compare, load from memory. */
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = n > 0 ? (unsigned char*)malloc((size_t)n) : NULL;
    const int ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(buf); return 0; }
    kz3_sha256_hex(buf, (size_t)n, hex);
    if (strcmp(hex, want) != 0)
        KZ3_LOGW("boot", "%s (SHA-256 %s, %s) is not the EBOOT this exe was built from (%s, %s): expect a crash",
                 path, hex, kz3_eboot_version_of(hex), want, kz3_eboot_version_of(want));
    const uint32_t entry = ppu_load_elf_image(buf, (uint64_t)n);
    free(buf);
    return entry;
}

int main(int argc, char** argv)
{
    kz3_logging_start();   /* first: every line below goes to the log file */
    atexit(kz3_log_shutdown);
    const char* eboot = kz3_boot_inputs(argc, argv);   /* argv[1] / kz3.ini / beside the exe (release pass 26) */
    kz3_discord_start();                               /* off unless discord_presence=1; background thread */
    atexit(kz3_discord_stop);

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: whatever still bypasses kz3_log */
#endif

    printf("=== ps3recomp game runner ===\n");

    /* KZ3 runtime defaults (an explicit environment setting still wins):
     *  PS3_CAMERA=0     no PlayStation Eye in this port (Move/camera declined); the HLE reports one otherwise.
     *  RSX_LIVE_DRAW=1  ps3recomp's live-draw engine renders: per-surface depth, MRT, KZ3's post effects and
     *                   text draw where the rsx_d3d12_backend path shows none. RSX_LIVE_DRAW=0 selects the old path.
     *  SPURS_TASKATTR_DESC=1  patch 1005: KZ3 passes its Bink SPU tasks' control block as the taskset argument
     *                   (SPU pass 5b); without it 1005 aborts at boot. */
    static const char* const kz3_env[][2] = { {"PS3_CAMERA", "0"}, {"RSX_LIVE_DRAW", "1"}, {"SPURS_TASKATTR_DESC", "1"} };
    for (const auto& e : kz3_env) {
        if (getenv(e[0])) continue;
#ifdef _WIN32
        char kv[64]; snprintf(kv, sizeof kv, "%s=%s", e[0], e[1]); _putenv(kv);
#else
        setenv(e[0], e[1], 0);
#endif
    }

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = kz3_load_eboot(eboot);   /* built in (release step 9s) or the file named above */
    if (!entry) {
        kz3_log_fatal("boot", "could not load the EBOOT.elf (it must be the decrypted EBOOT)");
        return 1;
    }

    /* KZ3 maps its own main memory for RSX at 0x20000000+ (cellGcmMapMainMemory, 2 x 1 MiB at load start), where the
     * runtime puts its GCM window by default; the two overwrote each other (corrupt streaming strings, null vtable
     * calls; SPU pass 9). Move the window out of the way before any guest code runs. KZ3_INJECT_BASE=<hex> overrides. */
    {
        const char* e = getenv("KZ3_INJECT_BASE");
        ppu_hle_inject_base = e ? (uint32_t)strtoul(e, nullptr, 16) : 0x7F000000u;
    }
    derive_vfs_root(eboot);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    kz3_split_roots();

    /* KZ3: answer asset opens that miss on disk from data_ps3.psarc (psarc_vfs.cpp). */
    {
        char arc[1100], cache[1100];
        snprintf(arc, sizeof arc, "%s/PS3_GAME/USRDIR/data_ps3.psarc", ppu_vfs_root);
        snprintf(cache, sizeof cache, "%s/psarc-cache", kz3_cache_root());
        kz3_psarc_vfs_init(arc, cache);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    kz3_kb_page_init();      /* KZ3: the Keyboard page's Lua bridge and GUM files (hooks A/B) */
    kz3_menu_mouse_install(); /* KZ3: mouse hover and click in the GUM menus (patch 2098, RSX pass 93) */
    kz3_aspect_init();       /* KZ3: camera at the window's aspect (patch 2096, KZ3_ASPECT) */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    kz3_hle_register();      /* KZ3: imports the runtime leaves unresolved (hle_kz3.cpp) */
    /* Savedata User*Auto calls take a ninth argument (userdata) from the guest stack; the
     * generic table drops it (RE pass 9: KZ3's profile callbacks got userdata 0). */
    cellSaveData_register_ctx_handlers();
    /* Title id from PARAM.SFO (BCUS98234) instead of the runtime default BLES00000: the
     * save directory and SysCache id come from it. */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    start_diagnostics();

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    kz3_log_shutdown();
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
