/* KZ3 path-mover probe (SPU pass 52; RE pass 114 s.2 probes 1-3, K2 Pyrrhus Evac: the Hammer never moves): KZ3_RAIL_PROBE=1.
 *
 * Hooks (1.00 only; layouts from RE pass 114 s.2):
 *   0x79B3F0 PathMover SetPath(this, name): this, vtable, the name, then on return +0xC8 (resolved path, 0 = not
 *     found), +0x140 (float speed), +0x24 (float time), +0x20 (state; 3 = at end).
 *   0x79B570 Teleport(this): count.
 *   0x79A740 FollowPathMover per-step Update(this, dt=f1): count; first 10 and every 60th: the same fields.
 *   0x384D10 InertiaPathMover::vf48(this): count; first 10 and every 60th: this+0x68, +0x69.
 *   0x7B3598 GameWorld::LogicStep: count.
 *   0x79B4D8 FollowPathMover GetTime -> f1 (Lua Player_hammer_time reads it every tick): count + last value.
 *   0x7B3CD0 the frame function that calls LogicStep (at 0x7B413C): skips everything when u32 [this+0x180] >= 3, and
 *     steps the logic only when u64 [this+0x78] >= u64 [this+0xE8] (0x7B3EA8..0x7B3ED8, read this round with rizin):
 *     first 5 calls, every 120th, and whenever +0x180 or the time scale +0x88 changes (up to 300 lines). The game
 *     clock: 0x31C960(this+0x70, ticks) adds ticks * float [this+0x88] to u64 [this+0x78] (rizin, this round).
 *   Pass 53: 0x7B1C00 Pause, 0x7B1D30 Resume, 0x849FB8 SetGameTimeScale, 0x7BA8C8 level-start states, 0x8C4E7C /
 *     0x8C5808 movie start/stop ([pause-probe] lines, kz3_rail_probe_pause), frame line once a second with +8.
 * A monitor thread prints all counters once a second while any changed (up to 400 lines). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

extern uint8_t* vm_base;

static int g_on = -1;
static volatile long g_n[6];
static volatile uint32_t g_last_this[6];
static volatile float g_last_time;
static volatile uint32_t g_hammer; /* pass 20 (net): the InertiaPathMover whose SetPath name was 'Hammer' */
static volatile long g_pm[7]; /* pass 65 PathMover rows, see kz3_rail_probe_pm */

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
static float rf(uint32_t ea)
{
    const uint32_t u = r32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void pm_fields_fwd(char* b, int n, uint32_t m);
#ifdef _WIN32
static DWORD WINAPI mon(LPVOID p)
#else
static void* mon(void* p)
#endif
{
    (void)p;
    long last[6] = {0}, last_pm[7] = {0}, lines = 0;
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        usleep(1000000);
#endif
        if (g_hammer) {
            /* network pass 20: the Hammer's PathMover time against its path, every 5th second (opt-in, KZ3_RAIL_PROBE) */
            static long tick;
            if (++tick % 5 == 0) {
                char fb[260];
                pm_fields_fwd(fb, sizeof fb, g_hammer);
                fprintf(stderr, "[rail-probe] hammer t=%llu this=0x%08X %s\n", now_ms(), g_hammer, fb);
                fflush(stderr);
            }
        }
        long pm[7];
        memcpy(pm, (const void*)g_pm, sizeof pm);
        if (!memcmp(last, (const void*)g_n, sizeof last) && !memcmp(last_pm, pm, sizeof pm)) continue;
        memcpy(last, (const void*)g_n, sizeof last);
        memcpy(last_pm, pm, sizeof pm);
        if (++lines > 400) continue;
        const uint32_t m = g_last_this[0] ? g_last_this[0] : g_last_this[2];
        fprintf(
            stderr,
            "[rail-probe] sec t=%llu setpath=%ld teleport=%ld fpm-update=%ld ipm-vf48=%ld logicstep=%ld gettime=%ld "
            "last_time=%.3f | mover 0x%08X C8=0x%08X speed=%.3f time=%.3f state=%u\n",
            now_ms(), g_n[0], g_n[1], g_n[2], g_n[3], g_n[4], g_n[5], g_last_time, m, r32(m + 0xC8), rf(m + 0x140),
            rf(m + 0x24), r32(m + 0x20));
        fprintf(
            stderr,
            "[rail-probe] sec-pm setpath=%ld teleport=%ld blend=%ld gettime=%ld vf48=%ld lookup=%ld createmover=%ld\n",
            pm[0], pm[1], pm[2], pm[3], pm[4], pm[5], pm[6]);
        fflush(stderr);
    }
    return 0;
}

int kz3_rail_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_RAIL_PROBE");
        g_on = e && *e && *e != '0';
        if (g_on) {
#ifdef _WIN32
            CreateThread(NULL, 0, mon, NULL, 0, NULL);
#else
            pthread_t t;
            pthread_create(&t, NULL, mon, NULL);
#endif
        }
    }
    return g_on;
}

static void str_at(uint32_t ea, char* out, int n)
{
    int i = 0;
    for (; ok(ea) && i < n - 1; i++) {
        const uint8_t c = vm_base[ea + i];
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[i] = 0;
}

static void fields(char* b, int n, uint32_t m)
{
    snprintf(b, n, "vt=0x%08X C8=0x%08X speed140=%.3f time24=%.3f state20=%u 68=%u 69=%u", r32(m), r32(m + 0xC8),
             rf(m + 0x140), rf(m + 0x24), r32(m + 0x20), ok(m) ? vm_base[m + 0x68] : 0xDEu,
             ok(m) ? vm_base[m + 0x69] : 0xDEu);
}

/* kind: 0 SetPath (after; name = r4 at entry), 1 Teleport, 2 FPM Update, 3 IPM vf48, 4 LogicStep, 5 GetTime. */
void kz3_rail_probe_hit(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr)
{
    if (!kz3_rail_probe_on() || kind < 0 || kind > 5) return;
    const long c = __atomic_add_fetch(&g_n[kind], 1, __ATOMIC_RELAXED);
    g_last_this[kind] = self;
    char fb[200];
    if (kind == 0) {
        char s1[40], s2[40];
        str_at(r4, s1, sizeof s1);
        str_at(r32(r4), s2, sizeof s2);
        fields(fb, sizeof fb, self);
        fprintf(stderr, "[rail-probe] SetPath #%ld t=%llu this=0x%08X lr=0x%08X name r4=0x%08X '%s' *r4='%s' -> %s\n",
                c, now_ms(), self, lr, r4, s1, s2, fb);
    } else if (kind == 1) {
        if (c <= 20)
            fprintf(stderr, "[rail-probe] Teleport #%ld t=%llu this=0x%08X lr=0x%08X\n", c, now_ms(), self, lr);
    } else if (kind == 2 || kind == 3) {
        if (c <= 10 || c % 60 == 0) {
            fields(fb, sizeof fb, self);
            fprintf(stderr, "[rail-probe] %s #%ld t=%llu this=0x%08X lr=0x%08X dt=%.4f %s\n",
                    kind == 2 ? "fpm-update" : "ipm-vf48", c, now_ms(), self, lr, f, fb);
        }
    } else if (kind == 5) {
        g_last_time = f;
        if (c <= 5) fprintf(stderr, "[rail-probe] GetTime #%ld this=0x%08X lr=0x%08X -> %.3f\n", c, self, lr, f);
    }
}

static uint64_t r64(uint32_t ea)
{
    return ((uint64_t)r32(ea) << 32) | r32(ea + 4);
}

/* 0x7B3CD0 entry (this = r3 = W, the GameModule singleton *[0x13A6D7C]). Pass 53 (RE pass 117, pacing s.8): one line
 * per second plus every change of +0x180, the time scale +0x88 or the pause counter +8 (up to 900 lines); adds +8, the
 * movie state u32 [0x13BC8D0] and u32 [0x1516B30]. */
void kz3_rail_probe_frame(uint32_t self, uint32_t lr)
{
    if (!kz3_rail_probe_on()) return;
    static long n, lines;
    static unsigned long long last_ms;
    static uint32_t last_state = 0xFFFFFFFFu, last_scale = 0xFFFFFFFFu, last_cnt = 0xFFFFFFFFu;
    const uint32_t st = r32(self + 0x180), sc = r32(self + 0x88), cnt = r32(self + 8);
    const unsigned long long t = now_ms();
    n++;
    if ((n <= 5 || t - last_ms >= 1000 || st != last_state || sc != last_scale || cnt != last_cnt) && ++lines <= 900) {
        last_ms = t;
        fprintf(stderr,
                "[rail-probe] frame #%ld t=%llu this=0x%08X lr=0x%08X pause8=%d state180=%u acc78=%llu step_e8=%llu "
                "e4=%u scale88=%.4f fc=%.4f movie13BC8D0=%u g1516B30=0x%08X logicstep=%ld\n",
                n, t, self, lr, (int)cnt, st, (unsigned long long)r64(self + 0x78),
                (unsigned long long)r64(self + 0xE8), ok(self) ? vm_base[self + 0xE4] : 0xDEu, rf(self + 0x88),
                rf(self + 0xFC), r32(0x13BC8D0u), r32(0x1516B30u), g_n[4]);
        fflush(stderr);
    }
    last_state = st;
    last_scale = sc;
    last_cnt = cnt;
}

/* Pass 53 (RE pass 117, pacing s.8): who holds the world paused. kind 0 = GameModule vf18 Pause(W, r4) 0x7B1C00,
 * 1 = vf19 Resume(W) 0x7B1D30, 2 = SetGameTimeScale native 0x849FB8 (f = f1 at entry; W read from *[0x13A6D7C]),
 * 3 = level-start state machine 0x7BA8C8 (logged only when +8/+0x88 changed across the call), 4 = movie start 0x8C4E7C,
 * 5 = movie stop 0x8C5808. after = 0 at entry, 1 on return. Kinds 0-2, 4, 5: every call up to 400 lines. */
void kz3_rail_probe_pause(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr, int after)
{
    if (!kz3_rail_probe_on() || kind < 0 || kind > 5) return;
    static const char* nm[6] = {"Pause",       "Resume",    "SetGameTimeScale", "levelstart-7BA8C8",
                                "movie-start", "movie-stop"};
    static long lines;
    static __thread uint32_t b_cnt, b_sc;
    const uint32_t w = (kind == 0 || kind == 1) ? self : r32(0x13A6D7Cu);
    const uint32_t cnt = r32(w + 8), sc = r32(w + 0x88);
    if (kind == 3) {
        if (!after) {
            b_cnt = cnt;
            b_sc = sc;
            return;
        }
        if (b_cnt == cnt && b_sc == sc) return;
    }
    if (++lines > 400) return;
    fprintf(stderr,
            "[pause-probe] %s %s t=%llu r3=0x%08X W=0x%08X r4=0x%08X lr=0x%08X f1=%.4f pause8=%d scale88=%.4f fc=%.4f "
            "movie13BC8D0=%u g1516B30=0x%08X state180=%u%s\n",
            nm[kind], after ? "exit" : "entry", now_ms(), self, w, r4, lr, f, (int)cnt, rf(w + 0x88), rf(w + 0xFC),
            r32(0x13BC8D0u), r32(0x1516B30u), r32(w + 0x180), kind == 3 ? " (changed across the call)" : "");
    if (kind <= 1 && !after) {
        /* Pass 53 (2): the unpaired Pause comes from 0x7B1394 (lr 0x7B13F0): it pauses when the system object
         * S = *[0x1335B4C] vf+0x74 (static: 0xAE1B10 = byte [0x1335B8C], set by the sysutil callback on 0x131 SYSTEM_MENU_OPEN,
         * cleared on 0x132) differs from W.b[0xF8]; gated by Q = *[0x1587FE8] +0x104/+0x108 and 0x4CAAEC
         * (*[0x1587FFC] +0x10 in 1..3). Dump what the run sees. */
        const uint32_t S = r32(0x1335B4Cu), vt = r32(S), d = r32(vt + 0x74), q = r32(0x1587FE8u), q2 = r32(0x1587FFCu);
        fprintf(
            stderr,
            "[pause-probe]   S=0x%08X vt=0x%08X vf74 desc=0x%08X fn=0x%08X b1335B8C=%u/%u/%u W.f8=%u Q=0x%08X q104=%u "
            "q108=%u Q2=0x%08X q2_10=%u\n",
            S, vt, d, r32(d), ok(0x1335B8Cu) ? vm_base[0x1335B8Cu] : 0xDEu, vm_base[0x1335B8Du], vm_base[0x1335B8Eu],
            ok(w) ? vm_base[w + 0xF8] : 0xDEu, q, ok(q) ? vm_base[q + 0x104] : 0xDEu,
            ok(q) ? vm_base[q + 0x108] : 0xDEu, q2, r32(q2 + 0x10));
    }
    fflush(stderr);
}

/* Pass 53 (3): the unpaired Pause is the game-state Enter 0x7B8B2C: it pauses while u32 [0x1516B30] (= 0x1514AD0
 * +0x2060, an atomic count of objects whose ready byte +0xC8 is 0; 0x1C56C0(0x1514AD0, r4): r4 = 0 -> +1, else -1)
 * is non-zero; the resume is the state's Exit 0x7B8C88. Hooks: 0x1C56C0 (caller's r30/r31 = the object, read at
 * entry before the callee saves them) and 0x0F7418 ready(item) -> 0 (which resource id of item+0x80 is not loaded:
 * table 0x13F50F0[id], the test of 0xF7418 read with rizin this round). Up to 120 lines each. */
static uint16_t r16(uint32_t ea)
{
    return ok(ea) ? (uint16_t)((vm_base[ea] << 8) | vm_base[ea + 1]) : 0xDEADu;
}
void kz3_rail_probe_count(uint32_t obj, uint32_t r4, uint32_t r31, uint32_t lr)
{
    if (!kz3_rail_probe_on()) return;
    static long lines;
    if (++lines > 120) return;
    fprintf(stderr,
            "[ready-probe] count %s t=%llu lr=0x%08X obj(r30)=0x%08X vt=0x%08X c8=%u r31=0x%08X vt31=0x%08X now=%d\n",
            r4 & 0xFF ? "-1" : "+1", now_ms(), lr, obj, r32(obj), ok(obj) ? vm_base[obj + 0xC8] : 0xDEu, r31, r32(r31),
            (int)r32(0x1516B30u));
    fflush(stderr);
}
void kz3_rail_probe_ready(uint32_t item, uint32_t ret, uint32_t lr)
{
    if (!kz3_rail_probe_on() || ret) return;
    static long lines, calls;
    calls++;
    if (lines >= 120 || (lines >= 20 && calls % 600)) return;
    lines++;
    const uint32_t d = r32(item + 0x80);
    const unsigned cnt = r16(d + 0x1C);
    char b[400];
    int k = snprintf(b, sizeof b,
                     "[ready-probe] notready #%ld t=%llu lr=0x%08X item=0x%08X vt=0x%08X desc=0x%08X n=%u:", calls,
                     now_ms(), lr, item, r32(item), d, cnt);
    for (unsigned i = 0; i < cnt && i < 6 && k < (int)sizeof b - 80; i++) {
        const unsigned id = r16(d + 0x1E + 2 * i);
        const uint32_t res = r32(0x13F50F0u + id * 4);
        k += snprintf(b + k, sizeof b - (size_t)k, " id%u res=0x%08X [0]=0x%08X e=%u [[18]+10]=0x%08X;", id, res,
                      r32(res), ok(res) ? vm_base[res + 0xE] : 0xDEu, r32(r32(res + 0x18) + 0x10));
    }
    fprintf(stderr, "%s\n", b);
    fflush(stderr);
}

/* Pass 65 (RE pass 123, the RE notes s.7.5): the Hammer's mover is an InertiaPathMover, which has no Lua rows of
 * its own and uses PathMover's: SetPath 0x3859D0, Teleport 0x385E14, BlendToPath 0x385E58, GetTime 0xB9B078. SetPath
 * stores the looked-up path object at +0x60 (0 = name not found; the object's +0xC = point count, +0x10 = points,
 * 32 bytes each, read in 0xCD250/0xCD328), sets the dirty byte +0x68 when it changed, clears +0x6A; +0x64 = path mode
 * (GetTime returns +0x6C when it is 0), +0x6C time. vf48 0x384D10(this, out) builds the movement record (out +0x20,
 * +0x24 floats, +0x28 byte) and returns early when +0x68 is 0. kind: 0 SetPath, 1 Teleport, 2 BlendToPath, 3 GetTime,
 * 4 vf48 (on return), 5 the name lookup 0x31FB8C called from SetPath or CreateMover (r4 = the name string object,
 * ret = the object found), 6 CreateMover native 0x845D50 (r4 = type name, ret = the new mover). Lines: the first 20
 * and every 120th per kind (SetPath, lookup, CreateMover: every call up to 200). */
static void pm_fields(char* b, int n, uint32_t m)
{
    const uint32_t p = r32(m + 0x60);
    snprintf(b, n,
             "vt=0x%08X path60=0x%08X(pts=%u arr=0x%08X) mode64=%u dirty68=%u 69=%u 6A=%u 6B=%u t6C=%.3f 70=%.3f "
             "74=%.3f",
             r32(m), p, p ? r32(p + 0xC) : 0u, p ? r32(p + 0x10) : 0u, r32(m + 0x64), ok(m) ? vm_base[m + 0x68] : 0xDEu,
             ok(m) ? vm_base[m + 0x69] : 0xDEu, ok(m) ? vm_base[m + 0x6A] : 0xDEu, ok(m) ? vm_base[m + 0x6B] : 0xDEu,
             rf(m + 0x6C), rf(m + 0x70), rf(m + 0x74));
}
static void pm_fields_fwd(char* b, int n, uint32_t m) { pm_fields(b, n, m); }
void kz3_rail_probe_pm(int kind, uint32_t self, uint32_t r4, float f, uint32_t lr, uint32_t ret)
{
    if (!kz3_rail_probe_on() || kind < 0 || kind > 6) return;
    static const char* nm[7] = {"pm-SetPath", "pm-Teleport", "pm-BlendToPath", "pm-GetTime",
                                "pm-vf48",    "pm-lookup",   "pm-CreateMover"};
    const long c = __atomic_add_fetch(&g_pm[kind], 1, __ATOMIC_RELAXED);
    const int every = kind == 0 || kind == 5 || kind == 6;
    if (every ? c > 200 : (c > 20 && c % 120)) return;
    char fb[260], s1[48], s2[48];
    str_at(r4, s1, sizeof s1);
    str_at(r32(r4), s2, sizeof s2);
    if (kind == 0 && (!strcmp(s1, "Hammer") || !strcmp(s2, "Hammer"))) g_hammer = self;
    switch (kind) {
    case 0:
    case 1:
    case 2:
        pm_fields(fb, sizeof fb, self);
        fprintf(stderr, "[rail-probe] %s #%ld t=%llu this=0x%08X lr=0x%08X r4=0x%08X '%s' *r4='%s' -> %s\n", nm[kind],
                c, now_ms(), self, lr, r4, s1, s2, fb);
        break;
    case 3:
        pm_fields(fb, sizeof fb, self);
        fprintf(stderr, "[rail-probe] %s #%ld t=%llu this=0x%08X lr=0x%08X -> f1=%.3f | %s\n", nm[kind], c, now_ms(),
                self, lr, f, fb);
        break;
    case 4:
        pm_fields(fb, sizeof fb, self);
        fprintf(stderr,
                "[rail-probe] %s #%ld t=%llu this=0x%08X lr=0x%08X out=0x%08X out20=%.4f out24=%.4f out28=%u ret=%u | "
                "%s\n",
                nm[kind], c, now_ms(), self, lr, r4, rf(r4 + 0x20), rf(r4 + 0x24), ok(r4) ? vm_base[r4 + 0x28] : 0xDEu,
                ret, fb);
        break;
    case 5:
        fprintf(stderr,
                "[rail-probe] %s #%ld t=%llu table=0x%08X lr=0x%08X name obj=0x%08X '%s' *='%s' -> 0x%08X vt=0x%08X "
                "pts=%u\n",
                nm[kind], c, now_ms(), self, lr, r4, s1, s2, ret, r32(ret), ret ? r32(ret + 0xC) : 0u);
        break;
    default:
        fprintf(stderr, "[rail-probe] %s #%ld t=%llu r3=0x%08X lr=0x%08X type '%s' *='%s' -> mover 0x%08X vt=0x%08X\n",
                nm[kind], c, now_ms(), self, lr, s1, s2, ret, r32(ret));
        break;
    }
    fflush(stderr);
}
