/* kz3_gameplay: the gameplay-test glue of SPU pass 33 (the gameplay test plan B4, B7, B8).
 *
 * Pure helpers in kz3_gameplay.c (no ppu_context, host-tested by tools/spu/test_kz3_gameplay.sh) plus the override
 * macros for port/spu/kz3_ppu_overrides.cpp (need ppu_context, vm_base and func_<ADDR>__lifted, patch 1018).
 *
 * Switches (all off by default):
 *   KZ3_SP_UNLOCK=1       B7: after the PlayerProfile ctor, profile+0x11F18 ("SPAllUnlocked", outside the saved
 *                         blob, per boot) = 1. ENHANCEMENT (test/debug switch): every mission shows in Select Mission.
 *   KZ3_GAMEPLAY_TRACE=1  B4: once per second "[trace] t=.. yaw=.. pitch=.. pos=(x,y,z) move=(x,y)" from the
 *                         SetViewAngles / UpdateMoveInput post-hooks. A line is written by whichever hook runs first
 *                         after the second elapsed, so no line means neither ran (menus, loading).
 *   B8 (1.14 only, no switch): the Motion Sensor byte profile+0xB6 is forced to 0 at the defaults store, the
 *                         pad-type rewrite and the profile load while kz3_pad_has_gyro() is 0 (the RE notes
 *                         section 14.5; the menu setter 0xBB0CB4 and the copy 0x96CAF4 are left alone).
 *
 * Test aids (SPU pass 43, the RE notes "Test aids: invulnerability, debug menu, ammo"; test switches, off by default,
 * player-only, through the game's own paths):
 *   KZ3_GOD_MODE=1        IsGodMode answers 1 for the local player's entity (the v1.00 profile god mode "On": damage
 *                         still lands, Humanoid::vf46 returns before the death step); "[god]" once per second.
 *   KZ3_INFINITE_AMMO=1   the Humanoid owner test of InventoryWeapon::HasInfiniteAmmo answers 1 for the local player
 *                         (the game's "Infinite Ammo": reserve and grenades never drop, the magazine still empties);
 *                   =2    also keeps the local player's magazine (and reserve) across the per-shot decrement
 *                         (DiscreteAmmoEjector shot 0x73D4B8: never reloads); "[ammo]" once per second.
 *   KZ3_DEBUG_MENU=1      the GUM data node game:cheat_enabled reads "true" (the String built at 0x89A15C gets the
 *                         pooled "true" literal), so the pause menu shows "Quit and reload" and "Debug Options".
 *                         Only that node: the level list builder's own "false" literals are untouched.
 *   Rows (v1.00 / 1.14 twin): KZ3_HOOK_GOD_MODE(005A7158 / 005BB618), KZ3_HOOK_INF_AMMO_OWNER(0061A480 / 0062F180),
 *   KZ3_HOOK_INF_MAG(0073D4B8 / 00752F38), KZ3_HOOK_DEBUG_MENU(00016700 lr 0x0089A164 "true" 0x00E6163C /
 *   00016618 lr 0x008BEF00 "true" 0x00EDD3A4). Player list: *(u32*)0x013A6D7C (1.14 0x0141C030), count +0x10, data
 *   +0x14; Player +0x70 local, +0x71 bot, +0x68 entity. Health = Destructibility *(ent+0x104) -> float +0x24 (re-data
 *   107; both versions), log only; entity+0x580 (1.14 +0x570), still passed as HP_OFF, is the Humanoid death timer.
 *   1.14: IsGodMode is the stub `li r3,0`, so the same post-hook re-creates god mode there; owner test offsets of
 *   the lifted body differ (+0x43D -> +0x42D) but the hook only reads the player list.
 *
 * Addresses (v1.00 / 1.14, the RE notes "B4/B7 address table" and the RE notes 14.5):
 *   KZ3_HOOK_SP_UNLOCK(004C2F08)     / 004CF158   PlayerProfile ctor, this = r3 (r31), zeroes +0x11F18 at 0x4C2F88
 *   KZ3_HOOK_TRACE_VIEW(005A3350)    / 005B7760   SetViewAngles(this, f1 yaw, f2 pitch) -> this+0x9C/+0xA0; entity
 *                                                 = [this+0x20]; position (RE pass 68): f32 at entity+0x2A0/
 *                                                 +0x2A4/+0x2A8 when *entity is the Soldier / Humanoid vtable
 *                                                 (0xEDD160 / 0xED8188; 1.14 0xF571E8 / 0xF521D8), else the result
 *                                                 of vf136 (vtable slot 0x220) + 0x30/+0x34/+0x38
 *   KZ3_HOOK_TRACE_MOVE(006861B0)    / 0069B330   UpdateMoveInput: stores f1 -> this+0xFC, f2 -> this+0xF8 (0x687444)
 *   KZ3_HOOK_SIXAXIS_DEFAULT(00964880)  1.14 only  defaults, profile = r3 (r31), stb r30,0xB6(r31) at 0x9648F8
 *   KZ3_HOOK_SIXAXIS_PADTYPE(009652DC)  1.14 only  pad-type hook, profile = r3 (r7), stb r4,0xB6(r7) at 0x9652F8
 *   KZ3_HOOK_SIXAXIS_LOAD(009703A0)     1.14 only  profile load, profile = r4 (r30), stb r3,0xB6(r30) at 0x9704A8
 */
#ifndef KZ3_GAMEPLAY_H
#define KZ3_GAMEPLAY_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Env flag: set and atoi(value) > 0. Unset, "", "0" and "no" are off. */
int kz3_env_on(const char* name);
/* Forget the cached switches (host tests only; the exe reads each switch once). */
void kz3_gameplay_reset(void);

/* B7. profile = host pointer at the PlayerProfile object; ea only for the log line. Writes +0x11F18 = 1 and logs
 * "[sp-unlock]" once per run when KZ3_SP_UNLOCK is on; returns 1 when the byte was written, 0 when off or null. */
#define KZ3_SP_ALL_UNLOCKED_OFF 0x11F18u
int kz3_sp_unlock_enabled(void);
int kz3_sp_unlock_apply(uint8_t* profile, uint32_t ea);

/* B8. No PC pad reports a Sixaxis gyro through cellPad yet: 0 (ASSUMPTION until SDL sensor passthrough exists,
 * enhancements.md "Gyro passthrough"). kz3_sixaxis_filter keeps b on a gyro pad, else 0; logs "[sixaxis]" once
 * per run the first time it changes a non-zero byte. */
#define KZ3_SIXAXIS_OFF 0xB6u
int kz3_pad_has_gyro(void);
uint8_t kz3_sixaxis_filter(uint8_t b);
/* profile = host pointer at the PlayerProfile; *(profile+0xB6) = kz3_sixaxis_filter(old); returns the new byte. */
uint8_t kz3_sixaxis_fixup(uint8_t* profile);

/* B4. State (last yaw/pitch/position/move) and a 1 s limiter. kz3_trace_on() is the only cost when off: one cached
 * int compare. now = seconds, any monotonic origin; kz3_trace_now() is the host clock. kz3_trace_emit writes one
 * line to out and returns 1 when at least KZ3_TRACE_PERIOD passed since the previous line (the first call always
 * emits); 0 (nothing written) otherwise or when the switch is off. */
#define KZ3_TRACE_PERIOD 1.0
extern int g_kz3_trace; /* -1 unread, 0 off, 1 on */
int kz3_trace_init(void);
static inline int kz3_trace_on(void)
{
    return g_kz3_trace > 0 || (g_kz3_trace < 0 && kz3_trace_init());
}
void kz3_trace_view(float yaw, float pitch, const float* pos /* 3 floats or NULL */);
void kz3_trace_move(float mx, float my);
int kz3_trace_emit(double now, FILE* out);
double kz3_trace_now(void);
/* The guest readers used by the macros: vm = guest base, this_ = the controller object (view) / the move input.
 * vcall(entity, opd, user) calls the entity's vf136 (returns its r3, 0 = not callable); NULL = never call it. */
#define KZ3_VT_SOLDIER_100 0x00EDD160u
#define KZ3_VT_HUMANOID_100 0x00ED8188u
#define KZ3_VT_SOLDIER_114 0x00F571E8u
#define KZ3_VT_HUMANOID_114 0x00F521D8u
#define KZ3_VF136_SLOT 0x220u
#define KZ3_SOLDIER_POS_OFF 0x2A0u
typedef uint32_t (*kz3_vcall_fn)(uint32_t entity, uint32_t opd, void* user);
void kz3_trace_view_guest(const uint8_t* vm, uint32_t this_, kz3_vcall_fn vcall, void* user);
/* The exe's vcall (kz3_ppu_overrides.cpp): user = the hook's ppu_context, ps3_indirect_call on the OPD. */
uint32_t kz3_trace_vcall_ctx(uint32_t entity, uint32_t opd, void* user);
void kz3_trace_move_guest(const uint8_t* vm, uint32_t this_);
/* rsx patch 2069 (port/kz3/main.cpp): native mouse look, a pre-hook on SetViewAngles that adds the mouse counts to
 * the yaw delta (f1) and pitch (f2) at the soldier's on-foot call only (it checks the return address itself);
 * KZ3_MOUSE_LOOK unset = one cached compare. ctx = the ppu_context. */
void kz3_mouse_look_pre(void* ctx);

#ifndef KZ3_GUEST_PTR_OK
#define KZ3_GUEST_PTR_OK(p) ((uint32_t)(p) >= 0x10000u && (uint32_t)(p) < 0xF0000000u)
#endif

/* Test aids (see the top comment). Each switch is read once. kz3_ammo_mode: 0 off, 1 game's infinite ammo, 2 also
 * the magazine. kz3_is_local_player_entity: 1 when ent is the entity (+0x68) of a Player in the list behind the
 * u32 at mgr_ptr_ea whose +0x70 (local) is set and +0x71 (bot) clear. The notes count and write one line per
 * second to stderr (now = seconds). */
int kz3_god_on(void);
int kz3_ammo_mode(void);
int kz3_debug_menu_on(void);
int kz3_is_local_player_entity(const uint8_t* vm, uint32_t mgr_ptr_ea, uint32_t ent);
/* god_note: every IsGodMode call while on (pl = this is the local player, ch = the hook turned 0 into 1); a line per
 * second when the player was asked. god_watch: the player's health once per second ("[god-hp]"). */
void kz3_god_note(const uint8_t* vm, uint32_t ent, uint32_t hp_off, int pl, int ch, double now);
void kz3_god_watch(const uint8_t* vm, uint32_t ent, uint32_t hp_off, double now);
void kz3_ammo_owner_note(double now);
#define KZ3_EJ_WEAPON_OFF 0x1Cu  /* DiscreteAmmoEjector -> InventoryWeapon (ASSUMPTION: the RE notes s.3) */
#define KZ3_EJ_STATE_OFF 0x24u   /* ammo state: +0x10 magazine, +0xC reserve */
#define KZ3_WEAPON_OWNER_OFF 0xF0u
/* Pre: the ammo state EA when ej is a local player's ejector (and the clip/reserve before), else 0. */
uint32_t kz3_mag_pre(const uint8_t* vm, uint32_t mgr_ptr_ea, uint32_t ej, uint32_t* clip, uint32_t* reserve);
/* Post: counts the shot, restores clip/reserve when keep, logs once per second. */
void kz3_mag_post(uint8_t* vm, uint32_t st, uint32_t clip0, uint32_t res0, int keep, double now);
void kz3_debug_menu_note(void);

/* Override glue (ppu_context, vm_base, func_##ADDR##__lifted from the lift). All post-hooks: the lifted body runs
 * first, then the host reads/writes the object; the switches cost one cached compare when off. */
#define KZ3_HOOK_SP_UNLOCK(ADDR)                                                                                       \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t this_ = (uint32_t)ctx->gpr[3];                                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (vm_base && KZ3_GUEST_PTR_OK(this_)) kz3_sp_unlock_apply(vm_base + this_, this_);                           \
    }
#define KZ3_HOOK_TRACE_VIEW(ADDR)                                                                                      \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        kz3_mouse_look_pre(ctx);                                                                                       \
        uint32_t this_ = (uint32_t)ctx->gpr[3];                                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_trace_on() && vm_base && KZ3_GUEST_PTR_OK(this_))                                                     \
            kz3_trace_view_guest(vm_base, this_, kz3_trace_vcall_ctx, ctx);                                            \
    }
#define KZ3_HOOK_TRACE_MOVE(ADDR)                                                                                      \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t this_ = (uint32_t)ctx->gpr[3];                                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_trace_on() && vm_base && KZ3_GUEST_PTR_OK(this_)) kz3_trace_move_guest(vm_base, this_);                \
    }
/* 1.14 B8 (profile register per address above). PADTYPE restores the byte the game had before the call when there
 * is no gyro (the function's own store would turn it back on, 14.2); DEFAULT and LOAD filter the stored byte. */
#define KZ3_HOOK_SIXAXIS_DEFAULT(ADDR)                                                                                 \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t p_ = (uint32_t)ctx->gpr[3];                                                                           \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (vm_base && KZ3_GUEST_PTR_OK(p_)) kz3_sixaxis_fixup(vm_base + p_);                                          \
    }
#define KZ3_HOOK_SIXAXIS_LOAD(ADDR)                                                                                    \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t p_ = (uint32_t)ctx->gpr[4];                                                                           \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (vm_base && KZ3_GUEST_PTR_OK(p_)) kz3_sixaxis_fixup(vm_base + p_);                                          \
    }
#define KZ3_HOOK_SIXAXIS_PADTYPE(ADDR)                                                                                 \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t p_ = (uint32_t)ctx->gpr[3];                                                                           \
        int ok_ = vm_base && KZ3_GUEST_PTR_OK(p_);                                                                     \
        uint8_t old_ = ok_ ? vm_base[p_ + KZ3_SIXAXIS_OFF] : 0;                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (ok_ && !kz3_pad_has_gyro()) vm_base[p_ + KZ3_SIXAXIS_OFF] = old_;                                          \
    }
/* Test aids: post-hooks except DEBUG_MENU (a pre-hook on the String-from-C-string ctor that swaps r4 only at the one
 * call whose return address is LR). Off: one cached compare (DEBUG_MENU: an lr compare first). */
#define KZ3_HOOK_GOD_MODE(ADDR, MGR, HP_OFF)                                                                          \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t this_ = (uint32_t)ctx->gpr[3];                                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (kz3_god_on() && vm_base)                                                                                   \
        {                                                                                                              \
            int pl_ = kz3_is_local_player_entity(vm_base, MGR, this_), ch_ = pl_ && (uint32_t)ctx->gpr[3] == 0;        \
            if (ch_) ctx->gpr[3] = 1;                                                                                  \
            kz3_god_note(vm_base, this_, HP_OFF, pl_, ch_, kz3_trace_now());                                           \
        }                                                                                                              \
    }
/* The owner test runs ~20 times a second for the armed local player: also the KZ3_GOD_MODE health watch. */
#define KZ3_HOOK_INF_AMMO_OWNER(ADDR, MGR, HP_OFF)                                                                     \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t this_ = (uint32_t)ctx->gpr[3];                                                                        \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if ((kz3_ammo_mode() || kz3_god_on()) && vm_base && kz3_is_local_player_entity(vm_base, MGR, this_))           \
        {                                                                                                              \
            if (kz3_god_on()) kz3_god_watch(vm_base, this_, HP_OFF, kz3_trace_now());                                  \
            if (kz3_ammo_mode() && (uint32_t)ctx->gpr[3] == 0)                                                         \
            {                                                                                                          \
                ctx->gpr[3] = 1;                                                                                       \
                kz3_ammo_owner_note(kz3_trace_now());                                                                  \
            }                                                                                                          \
        }                                                                                                              \
    }
#define KZ3_HOOK_INF_MAG(ADDR, MGR)                                                                                    \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t st_ = 0, c0_ = 0, r0_ = 0;                                                                            \
        if (kz3_ammo_mode() && vm_base) st_ = kz3_mag_pre(vm_base, MGR, (uint32_t)ctx->gpr[3], &c0_, &r0_);            \
        func_##ADDR##__lifted(ctx);                                                                                    \
        if (st_) kz3_mag_post(vm_base, st_, c0_, r0_, kz3_ammo_mode() >= 2, kz3_trace_now());                          \
    }
#define KZ3_HOOK_DEBUG_MENU(ADDR, LR, TRUE_EA)                                                                         \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        if ((uint32_t)ctx->lr == (LR) && kz3_debug_menu_on())                                                          \
        {                                                                                                              \
            ctx->gpr[4] = (TRUE_EA);                                                                                   \
            kz3_debug_menu_note();                                                                                     \
        }                                                                                                              \
        func_##ADDR##__lifted(ctx);                                                                                    \
    }

#ifdef __cplusplus
}
#endif
#endif
