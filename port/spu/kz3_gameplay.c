/* kz3_gameplay.c: B4 gameplay trace, B7 SP unlock, B8 Motion Sensor default (SPU pass 33). See kz3_gameplay.h.
 * Plain C, no runtime dependency: tools/spu/test_kz3_gameplay.sh builds it on the host. */
#include "kz3_gameplay.h"
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

int kz3_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && atoi(v) > 0;
}

/* ---- B7 ---- */
static int g_sp_unlock = -1, g_sp_logged = 0;

int kz3_sp_unlock_enabled(void)
{
    if (g_sp_unlock < 0) g_sp_unlock = kz3_env_on("KZ3_SP_UNLOCK");
    return g_sp_unlock;
}

int kz3_sp_unlock_apply(uint8_t* profile, uint32_t ea)
{
    if (!profile || !kz3_sp_unlock_enabled()) return 0;
    profile[KZ3_SP_ALL_UNLOCKED_OFF] = 1;
    if (!g_sp_logged++)
        fprintf(stderr,
                "[sp-unlock] ENHANCEMENT KZ3_SP_UNLOCK: profile 0x%08X +0x%X (SPAllUnlocked) = 1 for this boot\n", ea,
                KZ3_SP_ALL_UNLOCKED_OFF);
    return 1;
}

/* ---- B8 ---- */
static int g_six_logged = 0;

#if defined(_WIN32) || defined(KZ3_SIXAXIS_PAD_LAYER)
/* network pass 48: the pad layer's answer (cellPad.c, patches 4010/4011/4028): the device in use has working motion
 * sensors (SDL accelerometer/gyro enabled, or DSU samples arriving); the keyboard, XInput pads and pads without
 * sensors (an Xbox-style pad) answer 0. Host tests define KZ3_SIXAXIS_PAD_LAYER and supply the function. */
extern int pad_prompt_motion(void);
int kz3_pad_has_gyro(void)
{
    return pad_prompt_motion() != 0;
}
#else
int kz3_pad_has_gyro(void)
{
    return 0; /* no pad layer in a host test build */
}
#endif

uint8_t kz3_sixaxis_filter(uint8_t b)
{
    if (kz3_pad_has_gyro() || b == 0) return b;
    if (!g_six_logged++)
        fprintf(stderr, "[sixaxis] Motion Sensor Function forced Off: the pad reports no gyro (profile +0x%X was %u)\n",
                KZ3_SIXAXIS_OFF, b);
    return 0;
}

uint8_t kz3_sixaxis_fixup(uint8_t* profile)
{
    if (!profile) return 0;
    profile[KZ3_SIXAXIS_OFF] = kz3_sixaxis_filter(profile[KZ3_SIXAXIS_OFF]);
    return profile[KZ3_SIXAXIS_OFF];
}

/* ---- B4 ---- */
int g_kz3_trace = -1;
static double g_tr_t0 = -1.0, g_tr_last = -1.0;
static float g_tr_yaw, g_tr_pitch, g_tr_pos[3], g_tr_move[2];
static int g_tr_have_view, g_tr_have_pos, g_tr_have_move;

int kz3_trace_init(void)
{
    g_kz3_trace = kz3_env_on("KZ3_GAMEPLAY_TRACE");
    if (g_kz3_trace)
        fprintf(stderr, "[trace] KZ3_GAMEPLAY_TRACE on: one line per %.0f s from SetViewAngles / UpdateMoveInput\n",
                KZ3_TRACE_PERIOD);
    return g_kz3_trace;
}

static void kz3_aids_reset(void);

void kz3_gameplay_reset(void)
{
    kz3_aids_reset();
    g_sp_unlock = -1;
    g_sp_logged = 0;
    g_six_logged = 0;
    g_kz3_trace = -1;
    g_tr_t0 = g_tr_last = -1.0;
    g_tr_have_view = g_tr_have_pos = g_tr_have_move = 0;
}

void kz3_trace_view(float yaw, float pitch, const float* pos)
{
    g_tr_yaw = yaw;
    g_tr_pitch = pitch;
    g_tr_have_view = 1;
    if (pos) {
        memcpy(g_tr_pos, pos, sizeof g_tr_pos);
        g_tr_have_pos = 1;
    }
}

void kz3_trace_move(float mx, float my)
{
    g_tr_move[0] = mx;
    g_tr_move[1] = my;
    g_tr_have_move = 1;
}

int kz3_trace_emit(double now, FILE* out)
{
    if (!kz3_trace_on()) return 0;
    if (g_tr_t0 < 0) g_tr_t0 = now;
    if (g_tr_last >= 0 && now - g_tr_last < KZ3_TRACE_PERIOD) return 0;
    g_tr_last = now;
    fprintf(out, "[trace] t=%.1f yaw=%.3f pitch=%.3f pos=(%.2f,%.2f,%.2f) move=(%.2f,%.2f)%s\n", now - g_tr_t0,
            g_tr_have_view ? g_tr_yaw : 0.f, g_tr_have_view ? g_tr_pitch : 0.f, g_tr_have_pos ? g_tr_pos[0] : 0.f,
            g_tr_have_pos ? g_tr_pos[1] : 0.f, g_tr_have_pos ? g_tr_pos[2] : 0.f, g_tr_have_move ? g_tr_move[0] : 0.f,
            g_tr_have_move ? g_tr_move[1] : 0.f,
            (g_tr_have_view && g_tr_have_pos && g_tr_have_move) ? "" : " (partial)");
    return 1;
}

double kz3_trace_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static uint32_t rd32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static float rdf32be(const uint8_t* p)
{
    uint32_t u = rd32be(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void kz3_trace_view_guest(const uint8_t* vm, uint32_t this_, kz3_vcall_fn vcall, void* user)
{
    float yaw = rdf32be(vm + this_ + 0x9C), pitch = rdf32be(vm + this_ + 0xA0);
    uint32_t ent = rd32be(vm + this_ + 0x20);
    float pos[3];
    const float* pp = NULL;
    if (KZ3_GUEST_PTR_OK(ent)) {
        /* RE pass 68: SetViewAngles takes P from M+0x30 with M = entity->vf136(); on the Soldier/Humanoid
         * vtables vf136 returns entity+0x270 (so P = entity+0x2A0) and was just called by the body itself. */
        uint32_t vt = rd32be(vm + ent), m = 0;
        if (vt == KZ3_VT_SOLDIER_100 || vt == KZ3_VT_HUMANOID_100 || vt == KZ3_VT_SOLDIER_114 || vt == KZ3_VT_HUMANOID_114)
            m = ent + KZ3_SOLDIER_POS_OFF - 0x30;
        else if (vcall && KZ3_GUEST_PTR_OK(vt)) {
            uint32_t opd = rd32be(vm + vt + KZ3_VF136_SLOT);
            if (KZ3_GUEST_PTR_OK(opd)) m = vcall(ent, opd, user);
        }
        if (KZ3_GUEST_PTR_OK(m)) {
            pos[0] = rdf32be(vm + m + 0x30);
            pos[1] = rdf32be(vm + m + 0x34);
            pos[2] = rdf32be(vm + m + 0x38);
            pp = pos;
        }
    }
    kz3_trace_view(yaw, pitch, pp);
    kz3_trace_emit(kz3_trace_now(), stderr);
}

void kz3_trace_move_guest(const uint8_t* vm, uint32_t this_)
{
    kz3_trace_move(rdf32be(vm + this_ + 0xF8), rdf32be(vm + this_ + 0xFC));
    kz3_trace_emit(kz3_trace_now(), stderr);
}

/* ---- Test aids (SPU pass 43): KZ3_GOD_MODE, KZ3_INFINITE_AMMO, KZ3_DEBUG_MENU ---- */
static int g_god = -1, g_ammo = -1, g_dbg = -1, g_dbg_logged = 0;
static unsigned g_god_calls, g_god_pl, g_god_n, g_god_n_last, g_own_n, g_own_n_last, g_mag_n, g_mag_n_last;
static double g_god_t = -1.0, g_ammo_t = -1.0, g_hp_t = -1.0;
static float g_hp_last;
static float g_god_min_hp;
static uint32_t g_mag_c0, g_mag_c1, g_mag_r0, g_mag_r1;

static void kz3_aids_reset(void)
{
    g_god = g_ammo = g_dbg = -1;
    g_dbg_logged = 0;
    g_god_calls = g_god_pl = 0;
    g_hp_t = -1.0;
    g_god_n = g_god_n_last = g_own_n = g_own_n_last = g_mag_n = g_mag_n_last = 0;
    g_god_t = g_ammo_t = -1.0;
    g_god_min_hp = 1e30f;
}

int kz3_god_on(void)
{
    if (g_god < 0) {
        g_god = kz3_env_on("KZ3_GOD_MODE");
        g_god_min_hp = 1e30f;
        if (g_god) fprintf(stderr, "[god] KZ3_GOD_MODE=1 (test aid): the local player cannot die\n");
    }
    return g_god;
}

int kz3_ammo_mode(void)
{
    if (g_ammo < 0) {
        const char* v = getenv("KZ3_INFINITE_AMMO");
        int m = v ? atoi(v) : 0;
        g_ammo = m < 0 ? 0 : m > 2 ? 2 : m;
        if (g_ammo)
            fprintf(stderr, "[ammo] KZ3_INFINITE_AMMO=%d (test aid): local player %s\n", g_ammo,
                    g_ammo == 1 ? "reserve and grenades" : "reserve, grenades and magazine");
    }
    return g_ammo;
}

int kz3_debug_menu_on(void)
{
    if (g_dbg < 0) g_dbg = kz3_env_on("KZ3_DEBUG_MENU");
    return g_dbg;
}

int kz3_is_local_player_entity(const uint8_t* vm, uint32_t mgr_ptr_ea, uint32_t ent)
{
    if (!vm || !KZ3_GUEST_PTR_OK(ent)) return 0;
    uint32_t m = rd32be(vm + mgr_ptr_ea);
    if (!KZ3_GUEST_PTR_OK(m)) return 0;
    int32_t n = (int32_t)rd32be(vm + m + 0x10);
    uint32_t data = rd32be(vm + m + 0x14);
    if (n <= 0 || n > 64 || !KZ3_GUEST_PTR_OK(data)) return 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t p = rd32be(vm + data + 4u * (uint32_t)i);
        if (KZ3_GUEST_PTR_OK(p) && vm[p + 0x70] && !vm[p + 0x71] && rd32be(vm + p + 0x68) == ent) return 1;
    }
    return 0;
}

static float kz3_rdf(const uint8_t* vm, uint32_t ea)
{
    uint32_t u = rd32be(vm + ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Entity health (RE pass 107, the RE notes "God mode in every level"): the Destructibility component
 * C = *(u32*)(ent+0x104), hp = *(float*)(C+0x24), max = *(float*)(*(u32*)(C+0x18)+0x10) (100); same offsets on
 * v1.00 and 1.14. The old reader took ent+hp_off (0x580 / 0x570), which is the Humanoid death timer, not health.
 * hp_off is kept in the signature for the hook macros and logged no more. */
static float kz3_ent_health(const uint8_t* vm, uint32_t ent)
{
    uint32_t c = rd32be(vm + ent + 0x104);
    if (!KZ3_GUEST_PTR_OK(c)) return -1.0f;
    return kz3_rdf(vm, c + 0x24);
}

void kz3_god_note(const uint8_t* vm, uint32_t ent, uint32_t hp_off, int pl, int ch, double now)
{
    (void)hp_off;
    g_god_calls++;
    if (!pl) return;
    g_god_pl++;
    if (ch) g_god_n++;
    float hp = kz3_ent_health(vm, ent);
    if (hp < g_god_min_hp) g_god_min_hp = hp;
    if (g_god_t < 0 || now - g_god_t >= KZ3_TRACE_PERIOD) {
        fprintf(stderr, "[god] t=%.1f saves=%u (+%u) player-asks=%u all-asks=%u entity=0x%08X health=%.2f min=%.2f\n", now,
                g_god_n, g_god_n - g_god_n_last, g_god_pl, g_god_calls, ent, hp, g_god_min_hp);
        g_god_n_last = g_god_n;
        g_god_t = now;
    }
}

void kz3_god_watch(const uint8_t* vm, uint32_t ent, uint32_t hp_off, double now)
{
    (void)hp_off;
    float hp = kz3_ent_health(vm, ent);
    if (hp < g_god_min_hp) g_god_min_hp = hp;
    if (g_hp_t >= 0 && now - g_hp_t < KZ3_TRACE_PERIOD && hp == g_hp_last) return;
    if (g_hp_t >= 0 && now - g_hp_t < 0.25) return; /* changes: at most 4 lines a second */
    fprintf(stderr, "[god-hp] t=%.1f entity=0x%08X health=%.2f min=%.2f saves=%u player-asks=%u all-asks=%u\n", now, ent, hp,
            g_god_min_hp, g_god_n, g_god_pl, g_god_calls);
    g_hp_last = hp;
    g_hp_t = now;
}

static void kz3_ammo_line(double now)
{
    if (g_ammo_t >= 0 && now - g_ammo_t < KZ3_TRACE_PERIOD) return;
    fprintf(stderr, "[ammo] t=%.1f owner-test=%u (+%u) shots=%u (+%u) clip %u->%u reserve %u->%u\n", now, g_own_n,
            g_own_n - g_own_n_last, g_mag_n, g_mag_n - g_mag_n_last, g_mag_c0, g_mag_c1, g_mag_r0, g_mag_r1);
    g_own_n_last = g_own_n;
    g_mag_n_last = g_mag_n;
    g_ammo_t = now;
}

void kz3_ammo_owner_note(double now)
{
    g_own_n++;
    kz3_ammo_line(now);
}

uint32_t kz3_mag_pre(const uint8_t* vm, uint32_t mgr_ptr_ea, uint32_t ej, uint32_t* clip, uint32_t* reserve)
{
    if (!vm || !KZ3_GUEST_PTR_OK(ej)) return 0;
    uint32_t st = rd32be(vm + ej + KZ3_EJ_STATE_OFF), w = rd32be(vm + ej + KZ3_EJ_WEAPON_OFF);
    if (!KZ3_GUEST_PTR_OK(st) || !KZ3_GUEST_PTR_OK(w)) return 0;
    if (!kz3_is_local_player_entity(vm, mgr_ptr_ea, rd32be(vm + w + KZ3_WEAPON_OWNER_OFF))) return 0;
    *clip = rd32be(vm + st + 0x10);
    *reserve = rd32be(vm + st + 0xC);
    return st;
}

static void wr32be(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void kz3_mag_post(uint8_t* vm, uint32_t st, uint32_t clip0, uint32_t res0, int keep, double now)
{
    g_mag_n++;
    g_mag_c0 = clip0;
    g_mag_r0 = res0;
    g_mag_c1 = rd32be(vm + st + 0x10);
    g_mag_r1 = rd32be(vm + st + 0xC);
    if (keep) {
        wr32be(vm + st + 0x10, clip0);
        wr32be(vm + st + 0xC, res0);
    }
    kz3_ammo_line(now);
}

void kz3_debug_menu_note(void)
{
    if (!g_dbg_logged++)
        fprintf(stderr, "[debug-menu] KZ3_DEBUG_MENU=1 (test aid): game:cheat_enabled = \"true\" (Debug Options shown)\n");
}
