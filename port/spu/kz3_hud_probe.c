/* KZ3 HUD probe (SPU pass 35; RE pass 69/71, the RE notes sections 6.2-6.4): KZ3_HUD_PROBE=1.
 *
 * The HUD is native (HUD*Resource). Pass 42: mission_0 draws the elements its state allows (objectives marker,
 * reticle), level 2 the crosshair and ammo too (the RE notes Pass 42). Override rows count the calls of the HUD
 * manager's render slot (0x7EE150; 1.14 0x80FA30), the HUD render (0x7ED438; 1.14 0x80ECF8) and the font system
 * init that sets the text-context flag (0x1BCDF8; 1.14 0x1C8398), and once per second the render hook logs the
 * gates of 6.2 that a hook can read: G1 the font flag byte (0x1345C80; 1.14 0x13BA380: 0 = 0x1B9884 returns NULL
 * and the element pass is skipped), G2 the game object's state `*(*(Game+4)+0x44)` (1 = the element loop runs),
 * the manager global (0x136351C; 1.14 0x13D7C18) and the profile's DrawHUD byte +0xDAB (profile = *0x1587FEC;
 * 1.14 *0x15FD6FC). Every 10 s: the three call counts since the last line. The caller passes the globals of its
 * EBOOT version (kz3_ppu_overrides.cpp picks them by the PPU_OVERRIDE_ define), so both versions share this file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;

static int g_on = -1;
static unsigned g_calls[3], g_calls_rep[3];
static unsigned long long g_last_line, g_last_sum;

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static int guest_ok(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0x50000000u;
}
static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}

void kz3_hud_probe_ev_summary(void);
void kz3_hud_probe_view_summary(void);
static void kz3_hud_session_summary(void);
void kz3_hud_elem_summary(void);
static void summary(void)
{
    fprintf(stderr, "[hud-probe] calls/10s: manager-render=%u hud-render=%u font-init=%u (totals %u/%u/%u)\n",
            g_calls[0] - g_calls_rep[0], g_calls[1] - g_calls_rep[1], g_calls[2] - g_calls_rep[2], g_calls[0],
            g_calls[1], g_calls[2]);
    memcpy(g_calls_rep, g_calls, sizeof g_calls);
    kz3_hud_probe_ev_summary();
    kz3_hud_probe_view_summary();
    kz3_hud_session_summary();
    kz3_hud_elem_summary();
}
#ifdef _WIN32
/* The summary comes from its own thread (started at the first probed call, the font init at boot) so that a render
 * slot that is never called still shows as 0 every 10 s. */
static DWORD WINAPI summary_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(10000);
        summary();
    }
}
#endif

/* which: 0 = manager render slot, 1 = HUD render (globals: font flag byte, Game+4 pointer word, manager global,
 * profile pointer word), 2 = font init. r3 = the callee's this. */
void kz3_hud_probe_call(int which, uint32_t r3, const uint32_t* globals)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_HUD_PROBE");
        g_on = (e && atoi(e) > 0) ? 1 : 0;
#ifdef _WIN32
        if (g_on) CreateThread(NULL, 64 * 1024, summary_thread, NULL, 0, NULL);
#endif
    }
    if (!g_on || !vm_base || which < 0 || which > 2) return;   /* which -1: init only (pass 37 events) */
    unsigned long long t = now_ms();
    g_calls[which]++;
    if (which == 1 && globals && t - g_last_line >= 1000) {
        g_last_line = t;
        uint32_t obj = rd32(globals[1]), prof = rd32(globals[3]);
        uint32_t state = guest_ok(obj) ? rd32(obj + 0x44) : 0xFFFFFFFFu;
        unsigned drawhud = guest_ok(prof) ? vm_base[prof + 0xDAB] : 0xFFu;
        fprintf(stderr,
                "[hud-probe] render this=0x%08X font_flag=%u (G1) game_obj=0x%08X state=%u (G2, 1 = element loop) "
                "manager=0x%08X profile=0x%08X DrawHUD=%u (0 = on)\n",
                r3, vm_base[globals[0]], obj, state, rd32(globals[2]), prof, drawhud);
    }
#ifndef _WIN32
    if (t - g_last_sum >= 10000) {
        g_last_sum = t;
        summary();
    }
#endif
}

/* Pass 37 (the RE notes 7.8, suspects 1-5): KZ3_HUD_PROBE=1 also logs the HUD view-creation chain.
 * ev: 3 Player::vf13 0x325AD4 entry (r4 = msg, player+0x78, HUD = the manager global), 4 player module vf14 0x7BBB70,
 * 5 HUD module vf14 0x7AF444, 6 0x36C01C exit (r3, HUD+4 count, HUD+8), 7 HUD vf16 0x7F2970, 8 0x7F1A10 (HUD+0xF0
 * dirty flag), 9 0x7F0470 entry (HUD+0x216C, its setup count +0xC), 10 0x7F0058 (r4). A global sequence number orders
 * the events (suspect 1: is HUD still 0 when Player::vf13 runs, i.e. does 4 come before 5). First 12 of each are
 * logged, then counts in the 10 s summary line. `hud` = the manager global's address for this EBOOT version. */
static unsigned g_ev[11], g_ev_rep[11], g_seq;
static const char* const g_ev_name[11] = {"", "", "", "player-vf13", "player-mod-vf14", "hud-mod-vf14", "36C01C-exit",
                                          "hud-vf16", "7F1A10", "7F0470", "7F0058"};
void kz3_hud_probe_ev(int ev, uint32_t r3, uint32_t r4, uint32_t hud_global)
{
    if (g_on < 0) kz3_hud_probe_call(-1, 0, NULL);   /* reads KZ3_HUD_PROBE, starts the summary thread */
    if (!g_on || !vm_base || ev < 3 || ev > 10) return;
    unsigned n = ++g_ev[ev], seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    if (n > 12 && !(ev == 8 && (n % 2000) == 0)) return;
    uint32_t hud = rd32(hud_global);
    char extra[160] = "";
    if (ev == 3)
        snprintf(extra, sizeof extra, " msg=0x%08X player+0x78=0x%08X", r4, guest_ok(r3) ? rd32(r3 + 0x78) : 0);
    else if (ev == 6 && guest_ok(hud))
        snprintf(extra, sizeof extra, " ret=0x%08X HUD+4=%u HUD+8=0x%08X", r3, rd32(hud + 4), rd32(hud + 8));
    else if (ev == 8 && guest_ok(hud))
        snprintf(extra, sizeof extra, " HUD+0xF0=%u", vm_base[hud + 0xF0]);
    else if (ev == 9 && guest_ok(hud)) {
        uint32_t vs = rd32(hud + 0x216C);
        snprintf(extra, sizeof extra, " HUD+0x216C=0x%08X setups=%u HUD+0xD4=0x%08X HUD+0xD8=0x%08X", vs,
                 guest_ok(vs) ? rd32(vs + 0xC) : 0, rd32(hud + 0xD4), rd32(hud + 0xD8));
    } else if (ev == 10)
        snprintf(extra, sizeof extra, " r4=0x%08X", r4);
    fprintf(stderr, "[hud-chain] seq=%u %s #%u r3=0x%08X HUD=0x%08X%s\n", seq, g_ev_name[ev], n, r3, hud, extra);
}
void kz3_hud_probe_ev_summary(void)
{
    if (!g_on) return;
    fprintf(stderr, "[hud-chain] calls/10s: player-vf13=%u player-mod-vf14=%u hud-mod-vf14=%u 36C01C=%u hud-vf16=%u "
                    "7F1A10=%u 7F0470=%u 7F0058=%u\n", g_ev[3] - g_ev_rep[3], g_ev[4] - g_ev_rep[4], g_ev[5] - g_ev_rep[5],
            g_ev[6] - g_ev_rep[6], g_ev[7] - g_ev_rep[7], g_ev[8] - g_ev_rep[8], g_ev[9] - g_ev_rep[9], g_ev[10] - g_ev_rep[10]);
    memcpy(g_ev_rep, g_ev, sizeof g_ev);
}

/* Pass 39 (the RE notes 7.9, the view-entry gate that replaced G2): KZ3_HUD_PROBE=1 also logs
 * ev 11 the HUD view draw 0x7EDF6C entry (r3 = drawer, r4 = vector {+0 count, +4 view*[]}), 12 its exit, 13 the view
 * viewport setter 0x38CA7C entry (r3 view, r4 &viewport, r5 index), 14 0x7F0058 exit (HUD+4 view count), 15 0x7E5570
 * per-entry draw (r3 view, the call that only happens when view+0xC8 > 0). For the first view of the vector (11/12)
 * and HUD+8's first view (13/14): view+0xC8 entry count, +0xCC entry array, entry[0]+0x10 viewport index, and
 * HUD+0x217C (viewport count of the active setup), +0x2180, +0x2170. `voff` = 0xC8 on 1.00, 0xD8 on 1.14
 * (the RE notes s.35). First 12 of each, then every 2000th, counts in the summary line. */
static unsigned g_vev[20], g_vev_rep[20];
static const char* const g_vev_name[20] = {"", "", "", "", "", "", "", "", "", "", "", "7EDF6C-entry", "7EDF6C-exit",
                                            "38CA7C", "7F0058-exit", "7E5570",
                                            "1BDF84-entry", "1BDCB0", "HUD-vf3-exit", ""};
static __thread uint32_t t_vec;
/* Pass 40 (the RE notes 7.10): HUD+0xD4 (= what vf+0x3C at 0x7E578C returns), +0xD8, *(HUD+0xD4)+0x40, and the
 * UI renderer U = r32(0x14DDCD4) (1.14 0x1553354) with U+4, +0xC, +0x10 and byte +0x9D, the fields 0x1BDF84 gates on. */
static void kz3_hud_u_fields(char* o, size_t n, uint32_t hud, uint32_t hud_global)
{
    const uint32_t ug = hud_global == 0x136351Cu ? 0x14DDCD4u : 0x1553354u, u = rd32(ug);
    uint32_t d4 = 0, d8 = 0, d40 = 0;
    if (guest_ok(hud)) { d4 = rd32(hud + 0xD4); d8 = rd32(hud + 0xD8); if (guest_ok(d4)) d40 = rd32(d4 + 0x40); }
    if (guest_ok(u))
        snprintf(o, n, " HUD+0xD4=0x%08X +0xD8=0x%08X *(+0xD4)+0x40=0x%08X U=0x%08X U+4=0x%08X U+0xC=0x%08X U+0x10=0x%08X U+0x9D=%u",
                 d4, d8, d40, u, rd32(u + 4), rd32(u + 0xC), rd32(u + 0x10), vm_base[u + 0x9D]);
    else
        snprintf(o, n, " HUD+0xD4=0x%08X +0xD8=0x%08X *(+0xD4)+0x40=0x%08X U=0x%08X(bad)", d4, d8, d40, u);
}
static void view_fields(char* o, size_t n, uint32_t view, uint32_t voff)
{
    if (!guest_ok(view)) { snprintf(o, n, " view=0x%08X(bad)", view); return; }
    const uint32_t cnt = rd32(view + voff), arr = rd32(view + voff + 4);
    snprintf(o, n, " view=0x%08X +0x%X=%u +0x%X=0x%08X entry0+0x10=%d", view, voff, cnt, voff + 4, arr,
             (cnt && guest_ok(arr)) ? (int)rd32(arr + 0x10) : -1);
}
void kz3_hud_probe_view(int ev, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t hud_global, uint32_t voff)
{
    if (g_on < 0) kz3_hud_probe_call(-1, 0, NULL);
    if (!g_on || !vm_base || ev < 11 || ev > 18) return;
    if (ev == 11) t_vec = r4;
    unsigned n = ++g_vev[ev], seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);
    if (n > 12 && (n % 2000) != 0) return;
    const uint32_t hud = rd32(hud_global);
    char v[640] = "", h[128] = "";
    if (ev == 11 || ev == 12) {
        const uint32_t vec = t_vec;
        const uint32_t c = guest_ok(vec) ? rd32(vec) : 0, a = guest_ok(vec) ? rd32(vec + 4) : 0;
        char f[128] = "";
        if (c && guest_ok(a)) view_fields(f, sizeof f, rd32(a), voff);
        snprintf(v, sizeof v, " vec=0x%08X n=%u%s", vec, c, f);
    } else if (ev == 13) {
        snprintf(v, sizeof v, " r4=0x%08X i=%u", r4, r5);
        size_t l = strlen(v);
        view_fields(v + l, sizeof v - l, r3, voff);
    } else if (ev == 14 && guest_ok(hud)) {
        const uint32_t vs = rd32(hud + 8);
        snprintf(v, sizeof v, " HUD+4=%u", rd32(hud + 4));
        size_t l = strlen(v);
        if (guest_ok(vs)) view_fields(v + l, sizeof v - l, rd32(vs), voff);
    } else if (ev == 15) {
        size_t l = strlen(v);
        view_fields(v + l, sizeof v - l, r3, voff);
        l = strlen(v);
        kz3_hud_u_fields(v + l, sizeof v - l, hud, hud_global);
    }
    if (ev == 16) {   /* 0x1BDF84 entry: r3 = U (the UI renderer) */
        if (guest_ok(r3)) {
            int l = snprintf(v, sizeof v, " U+4=0x%08X U+0xC=0x%08X U+0x10=0x%08X U+0x9D=%u", rd32(r3 + 4),
                             rd32(r3 + 0xC), rd32(r3 + 0x10), vm_base[r3 + 0x9D]);
            /* pass 41 (the RE notes 7.13 gate (b)): X = *(U+0x18), the array of U+0x14 refs; elements
             * 22-25 (X+0x58..+0x64) must be non-null refs; ref -> surface, class = surface+4 >> 30 */
            const uint32_t X = rd32(r3 + 0x18);
            l += snprintf(v + l, sizeof v - l, " U+0x14=%u X=0x%08X", rd32(r3 + 0x14), X);
            for (int e = 22; e <= 25 && guest_ok(X) && l < (int)sizeof v - 60; e++) {
                const uint32_t ref = rd32(X + 4 * e), surf = guest_ok(ref) ? rd32(ref) : 0;
                l += snprintf(v + l, sizeof v - l, " [%d]=0x%08X->0x%08X/c%u", e, ref, surf,
                              guest_ok(surf) ? rd32(surf + 4) >> 30 : 9u);
            }
        }
    } else if (ev == 18) {   /* HUD vf3 exit */
        kz3_hud_u_fields(v, sizeof v, hud, hud_global);
    }
    if (guest_ok(hud))
        snprintf(h, sizeof h, " HUD+0x217C=%u +0x2180=0x%08X +0x2170=0x%08X", rd32(hud + 0x217C), rd32(hud + 0x2180),
                 rd32(hud + 0x2170));
    fprintf(stderr, "[hud-view] seq=%u %s #%u r3=0x%08X HUD=0x%08X%s%s\n", seq, g_vev_name[ev], n, r3, hud, v, h);
}
void kz3_hud_probe_view_summary(void)
{
    if (!g_on) return;
    fprintf(stderr, "[hud-view] calls/10s: 7EDF6C=%u 38CA7C=%u 7F0058=%u 7E5570=%u 1BDF84=%u 1BDCB0=%u vf3=%u\n",
            g_vev[11] - g_vev_rep[11], g_vev[13] - g_vev_rep[13], g_vev[14] - g_vev_rep[14], g_vev[15] - g_vev_rep[15],
            g_vev[16] - g_vev_rep[16], g_vev[17] - g_vev_rep[17], g_vev[18] - g_vev_rep[18]);
    memcpy(g_vev_rep, g_vev, sizeof g_vev);
}

/* Pass 41 (the RE notes 7.13): the GameModule session object's state machine. 4 -> 5 (which runs HUD vf3)
 * needs the movie player state == 3 (a movie played to its end), the Game's pending mode pointer == 0 and the tests in
 * 0x7BAF7C. ev 0 = session Update 0x7BB404 entry (1.14 0x7D2110), ev 1 = its exit, ev 2 = 0x7BAF7C exit (the 4 -> 5
 * try; 1.14 0x7D1C7C), ev 3 = movie start 0x8C5080 entry (1.14 0x8F1128). g = {movie state EA, pending EA, flag EA}.
 * A line whenever (state, movie, +0x13, +0x14, pending != 0, flag) changes (first 300), plus a 10 s count line. */
static unsigned g_sess_n[4], g_sess_rep[4], g_sess_lines;
static uint32_t g_sess_last = 0xFFFFFFFFu, g_sess_obj;
static const uint32_t* g_sess_g;
void kz3_hud_session(int ev, uint32_t s, uint32_t r4, uint32_t r5, const uint32_t* g)
{
    if (g_on < 0) kz3_hud_probe_call(-1, 0, NULL);
    if (!g_on || !vm_base || ev < 0 || ev > 3) return;
    __atomic_add_fetch(&g_sess_n[ev], 1, __ATOMIC_RELAXED);
    g_sess_g = g;
    if (ev == 3) {
        if (g_sess_n[3] <= 50)
            fprintf(stderr, "[hud-sess] t=%llu movie-start #%u r3=0x%08X r4=0x%08X r5=0x%08X movie=%u\n", now_ms(),
                    g_sess_n[3], s, r4, r5, rd32(g[0]));
        return;
    }
    if (ev < 2) g_sess_obj = s;
    if (!guest_ok(s)) return;
    const uint32_t st = rd32(s + 8), mv = rd32(g[0]), pend = rd32(g[1]);
    const uint8_t b13 = vm_base[s + 0x13], b14 = vm_base[s + 0x14], fl = vm_base[g[2]];
    const uint32_t key = (st & 0xFF) | (mv & 0xFF) << 8 | (uint32_t)b13 << 16 | (uint32_t)(b14 & 1) << 24 |
                         (uint32_t)(pend != 0) << 25 | (uint32_t)(fl != 0) << 26;
    if (ev == 2 || key != g_sess_last) {
        if (ev != 2) g_sess_last = key;
        if (g_sess_lines++ < 300)
            fprintf(stderr, "[hud-sess] t=%llu %s s=0x%08X state=%u movie=%u +0x13=%u +0x14=%u pending=0x%08X flag=%u\n",
                    now_ms(), ev == 0 ? "update-in" : ev == 1 ? "update-out" : "try4to5-out", s, st, mv, b13, b14,
                    pend, fl);
    }
}
static void kz3_hud_session_summary(void)
{
    if (!g_on || !g_sess_g) return;
    const uint32_t s = g_sess_obj;
    fprintf(stderr, "[hud-sess] calls/10s: update=%u try4to5=%u movie-start=%u now state=%u movie=%u\n",
            g_sess_n[0] - g_sess_rep[0], g_sess_n[2] - g_sess_rep[2], g_sess_n[3] - g_sess_rep[3],
            guest_ok(s) ? rd32(s + 8) : 0xFFFFFFFFu, rd32(g_sess_g[0]));
    memcpy(g_sess_rep, g_sess_n, sizeof g_sess_n);
}

/* Pass 42: the HUD element path and the UI renderer's view pass (the RE notes Pass 42 s.1). Entry counters:
 * 0 HUD element instance base ctor 0x7FAEC4 (vtable 0xF00600; every HUD*Resource::vf16 = CreateInstance ends here),
 * 1 element base vf5 0x7FB9A0, 2 element base vf7 0x7FBB44, 3 crosshair instance vf16 0x8119B8, 4 crosshair vf6 0x810280,
 * 5 HUDPartText::vf2 0x7FD908, 6 HUDPartBackground::vf2 0x7FC9A4, 7 HUDPartProgressBar::vf2 0x7FCE48,
 * 8 HUDPartIcon::vf2 0x7FC328, 9 text-context getter 0x1B9884, 10 Renderer::BuildView 0x1FF878 (r3 = renderer,
 * r4 = U, r5 = {view count, views of 0x810 B} = the frameinfo 0x7EDF6C builds; the UI pass is the call with
 * r3 == *(U+0x10)), 11 the view submit 0x1FB4A4 (r3 = renderer), 12 0x1F3350 (first call of the 0x1BDF84 draw block,
 * i.e. gates (a)-(c) passed), 13 renderer vf+0x30 0x200204 (end of the 0x1BDF84 draw block); pass 42 second run:
 * 14-17 element base vf2/vf3/vf10/vf11 0x7FB164/0x7FB248/0x7FBCE4/0x7FBDB0, 18-21 crosshair vf4/vf10/vf11/vf14
 * 0x810E60/0x811118/0x8124B0/0x8112CC. Every event logs its first 6 distinct return addresses (lr = the caller). 10 and 11 count the UI
 * renderer and the others separately. Once per second the UI BuildView logs its early-return inputs: every view's
 * word 0 against renderer+0x3C (BuildView compares them), view+0x18 (0 = BuildView returns before anything is drawn)
 * and view+0x800 (gate (a) bit 2). */
#define KZ3_HE_N 28
#define KZ3_HE_LR 6
static uint32_t g_he_lr[KZ3_HE_N][KZ3_HE_LR];   /* first distinct return addresses per event (who calls it) */
static unsigned g_he[KZ3_HE_N], g_he_rep[KZ3_HE_N], g_he_ui[2], g_he_ui_rep[2], g_he_lines;
static unsigned long long g_he_last;
static uint32_t g_he_ptr;   /* 0x14DDCD4 (1.14 0x1553354): the UI renderer pointer U */
static const char* const g_he_name[KZ3_HE_N] = {"elem-ctor", "elem-vf5", "elem-vf7", "xhair-vf16", "xhair-vf6", "part-text", "part-bg",
    "part-bar", "part-icon", "textctx", "BuildView", "submit", "1F3350", "rend-vf30", "elem-vf2", "elem-vf3", "elem-vf10", "elem-vf11",
    "xhair-vf4", "xhair-vf10", "xhair-vf11", "xhair-vf14", "draw-81BB34", "draw-84094C", "sold-spawn-616250", "sold-state-65D658", "sold-616880", ""};
void kz3_hud_elem(int idx, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t u_ptr, uint32_t lr)
{
    (void)r4;
    if (g_on < 0) kz3_hud_probe_call(-1, 0, 0);
    if (!g_on || !vm_base || idx < 0 || idx >= KZ3_HE_N) return;
    g_he[idx]++;
    for (int i = 0; i < KZ3_HE_LR; i++) {   /* record the first distinct callers (lr) of each event */
        if (g_he_lr[idx][i] == lr) break;
        if (g_he_lr[idx][i] == 0) {
            g_he_lr[idx][i] = lr;
            fprintf(stderr, "[hud-elem] %s caller #%d lr=0x%08X (call #%u, this=0x%08X r4=0x%08X)\n", g_he_name[idx], i + 1, lr, g_he[idx], r3, r4);
            break;
        }
    }
    g_he_ptr = u_ptr;
    if (idx == 0 && g_he[0] <= 40) fprintf(stderr, "[hud-elem] instance #%u ctor this=0x%08X\n", g_he[0], r3);
    if (idx != 10 && idx != 11) return;
    const uint32_t u = rd32(u_ptr);
    const int ui = guest_ok(u) && r3 == rd32(u + 0x10);
    if (!ui) return;
    g_he_ui[idx - 10]++;
    if (idx != 10) return;
    unsigned long long t = now_ms();
    if (t - g_he_last < 1000 || g_he_lines >= 400) return;
    g_he_last = t;
    g_he_lines++;
    char o[640];
    int l = snprintf(o, sizeof o, "[hud-elem] BuildView(ui) #%u rend=0x%08X +0x38=0x%08X +0x3C=0x%08X +0xA40=%u U=0x%08X U+4=0x%08X fi=0x%08X n=%u",
                     g_he_ui[0], r3, rd32(r3 + 0x38), rd32(r3 + 0x3C), vm_base[r3 + 0xA40], u, rd32(u + 4), r5,
                     guest_ok(r5) ? rd32(r5) : 0xFFFFFFFFu);
    if (guest_ok(r5)) {
        const uint32_t n = rd32(r5), d = rd32(r5 + 4);
        for (uint32_t i = 0; i < n && i < 4 && guest_ok(d + i * 0x810); i++) {
            const uint32_t v = d + i * 0x810;
            l += snprintf(o + l, sizeof o - l, " v%u=0x%08X[+0=0x%08X +0x18=0x%08X +0x800=0x%08X]", i, v, rd32(v),
                          rd32(v + 0x18), rd32(v + 0x800));
            if (l >= (int)sizeof o - 80) break;
        }
    }
    fprintf(stderr, "%s\n", o);
}
static void kz3_hud_vf3_summary(void);
void kz3_hud_elem_summary(void)
{
    if (!g_on) return;
    kz3_hud_vf3_summary();
    fprintf(stderr,
            "[hud-elem] calls/10s: elem-ctor=%u elem-vf5=%u elem-vf7=%u xhair-vf16=%u xhair-vf6=%u part-text=%u part-bg=%u "
            "part-bar=%u part-icon=%u textctx=%u BuildView=%u(ui %u) submit=%u(ui %u) 1F3350=%u rend-vf30=%u (instances %u)"
            " elem-vf2=%u vf3=%u vf10=%u vf11=%u xhair-vf4=%u vf10=%u vf11=%u vf14=%u spawn=%u state=%u 616880=%u\n",
            g_he[0] - g_he_rep[0], g_he[1] - g_he_rep[1], g_he[2] - g_he_rep[2], g_he[3] - g_he_rep[3],
            g_he[4] - g_he_rep[4], g_he[5] - g_he_rep[5], g_he[6] - g_he_rep[6], g_he[7] - g_he_rep[7],
            g_he[8] - g_he_rep[8], g_he[9] - g_he_rep[9], g_he[10] - g_he_rep[10], g_he_ui[0] - g_he_ui_rep[0],
            g_he[11] - g_he_rep[11], g_he_ui[1] - g_he_ui_rep[1], g_he[12] - g_he_rep[12], g_he[13] - g_he_rep[13],
            g_he[0], g_he[14] - g_he_rep[14], g_he[15] - g_he_rep[15], g_he[16] - g_he_rep[16], g_he[17] - g_he_rep[17],
            g_he[18] - g_he_rep[18], g_he[19] - g_he_rep[19], g_he[20] - g_he_rep[20], g_he[21] - g_he_rep[21],
            g_he[24] - g_he_rep[24], g_he[25] - g_he_rep[25], g_he[26] - g_he_rep[26]);
    memcpy(g_he_rep, g_he, sizeof g_he);
    memcpy(g_he_ui_rep, g_he_ui, sizeof g_he_ui);
}

/* Pass 42, third run: the per-view element container update 0x7F49D0 (r3 = container: count +0x10, elements +0x14,
 * entity +4) computes the HUD mode it passes to every element's vf3 (r5): 1 = the entity is a Soldier (RTTI
 * 0x13886E4) without bit 0x1000 at +0x108, 2/3 = Soldier state +0x584 in {1, 2} (3 with the fall-over state), 0 =
 * no entity / not a Soldier / bit 0x1000 set; r6 = (profile DrawHUD byte +0xDAB == 1). Element vf3 0x7FB248 calls
 * vf10(this, mode) (visible-in-mode flag from the resource bytes +0x54..+0x57) and sets this+0x9C = 1 (visible this
 * frame) only when it is non-zero and vf9 (resource byte +0x59) is set. Logged: the container fields once per
 * second, a histogram of the mode seen at vf3's entry and how many vf3 calls end with +0x9C == 1, per 10 s. */
static unsigned g_mode[4], g_mode_rep[4], g_vis, g_vis_rep, g_cont, g_cont_rep, g_cont_lines;
static unsigned long long g_cont_last;
void kz3_hud_container(uint32_t c, uint32_t prof_ptr)
{
    if (g_on < 0) kz3_hud_probe_call(-1, 0, 0);
    if (!g_on || !vm_base) return;
    g_cont++;
    unsigned long long t = now_ms();
    if (t - g_cont_last < 1000 || g_cont_lines >= 400 || !guest_ok(c)) return;
    g_cont_last = t;
    g_cont_lines++;
    const uint32_t e = rd32(c + 4), prof = rd32(prof_ptr);
    fprintf(stderr, "[hud-elem] container #%u c=0x%08X n=%u entity=0x%08X vt=0x%08X +0x108=0x%08X(bit12 %u) +0x584=%u DrawHUD=%u\n",
            g_cont, c, rd32(c + 0x10), e, guest_ok(e) ? rd32(e) : 0, guest_ok(e) ? rd32(e + 0x108) : 0,
            guest_ok(e) ? (rd32(e + 0x108) >> 12) & 1 : 9, guest_ok(e) ? rd32(e + 0x584) : 0,
            guest_ok(prof) ? vm_base[prof + 0xDAB] : 0xFF);
}
static uint32_t g_vis_vt[16];
static unsigned g_vis_vt_n[16];
void kz3_hud_vf3(int exit, uint32_t self, uint32_t mode)
{
    if (!g_on || !vm_base) return;
    if (!exit) {
        g_mode[mode < 4 ? mode : 3]++;
        return;
    }
    if (!guest_ok(self) || !vm_base[self + 0x9C]) return;
    g_vis++;
    const uint32_t vt = rd32(self);   /* which element classes are visible: first 16 distinct vtables */
    for (int i = 0; i < 16; i++) {
        if (g_vis_vt[i] == vt) {
            g_vis_vt_n[i]++;
            break;
        }
        if (!g_vis_vt[i]) {
            g_vis_vt[i] = vt;
            g_vis_vt_n[i] = 1;
            fprintf(stderr, "[hud-elem] visible element class #%d vtable=0x%08X this=0x%08X res=0x%08X fade=0x%08X\n", i + 1, vt, self,
                    rd32(self + 8), rd32(self + 0x30));
            break;
        }
    }
}
/* crosshair vf10 0x811118 (visible-in-mode with game-state conditions) exit: r3 once per second */
static unsigned long long g_xv_last;
static unsigned g_xv_n[2];
void kz3_hud_xhair_vis(uint32_t self, uint32_t ret)
{
    if (!g_on || !vm_base) return;
    g_xv_n[ret != 0]++;
    unsigned long long t = now_ms();
    if (t - g_xv_last < 1000) return;
    g_xv_last = t;
    fprintf(stderr, "[hud-elem] xhair-vf10 ret=%u this=0x%08X (nonzero %u / zero %u so far) res+0x54..57=%02X %02X %02X %02X +0x58=%02X +0x59=%02X\n", ret, self,
            g_xv_n[1], g_xv_n[0], vm_base[rd32(self + 8) + 0x54], vm_base[rd32(self + 8) + 0x55], vm_base[rd32(self + 8) + 0x56],
            vm_base[rd32(self + 8) + 0x57], vm_base[rd32(self + 8) + 0x58], vm_base[rd32(self + 8) + 0x59]);
}
static void kz3_hud_vf3_summary(void)
{
    if (!g_on) return;
    char o[400];
    int l = 0;
    for (int i = 0; i < 16 && g_vis_vt[i] && l < (int)sizeof o - 24; i++) l += snprintf(o + l, sizeof o - l, " %08X:%u", g_vis_vt[i], g_vis_vt_n[i]);
    fprintf(stderr, "[hud-elem] vf3/10s: mode0=%u mode1=%u mode2=%u mode3=%u visible(+0x9C)=%u container-updates=%u classes(vtable:total)%s\n",
            g_mode[0] - g_mode_rep[0], g_mode[1] - g_mode_rep[1], g_mode[2] - g_mode_rep[2], g_mode[3] - g_mode_rep[3],
            g_vis - g_vis_rep, g_cont - g_cont_rep, o);
    memcpy(g_mode_rep, g_mode, sizeof g_mode);
    g_vis_rep = g_vis;
    g_cont_rep = g_cont;
}
