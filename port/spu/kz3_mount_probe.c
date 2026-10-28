/* KZ3 gunner-mount chain probe (SPU pass 51; RE pass 112 s.2/s.3, K2 Pyrrhus Evac black: the player is the ISA
 * Hammer's gunner, MountedGun ISA_HammerTurret_MountGun): KZ3_MOUNT_PROBE=1.
 *
 * Hooks (1.00 addresses only; rows in port/spu/ppu_overrides.txt, glue in kz3_ppu_overrides.cpp), in chain order:
 *   0  0x5CBAE4 MountableEntity::Mount(gun, humanoid, name, b6, b7) - entry: the gates C1-C7 of RE pass 112 s.2
 *      (gun+0x270 state, gun+0x230 user, humanoid+0xFC mover and its vtable slot 0 function, res = gun+0xE4,
 *      res+0x138 count, gun+0x218 mount-point count, name string).
 *   1  0x322940 readiness test(entity) - logged only when called from Mount (lr in 0x5CBAE4..0x5CBE00): the entity,
 *      the result, entity+0x10C (world node) and node+0x4D (flag).
 *   2  0x331648 event send(mgr, event, 0xFE, 0) - logged from Mount; also counts every id 0x2D event (halfword at
 *      event+0) from anywhere.
 *   3  0x5CB61C immediate mount path, 4 0x5DCC6C gun vf60 AttachUser (ret), 5 0x694700 HumanoidSubStateUsingInter-
 *      activeEntity vf103 (enter), 6 0x5A5778 ControlledEntity::TransferController, 7 0x5C8C10 controller camera
 *      activate (this+0x14 = camera after), 8 0x5A00CC FirstPersonCameraEntity ctor thunk.
 * Read-only on guest memory. First KZ3_MOUNT_PROBE_N (default 20) calls of each hook are printed, then every 500th,
 * and a summary line with all counts every 5 s once anything changed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

extern uint8_t* vm_base;

#define NK 9
static const char* const k_name[NK] = {"Mount",        "ready",      "evsend",       "mount-now", "AttachUser",
                                       "substate-enter", "TransferCtl", "cam-activate", "FPCam-ctor"};
static int g_on = -1;
static long g_first = 20;
static volatile long g_n[NK], g_ev2d, g_ready_fail;
static volatile long g_lock;
/* The local player's mount (pass 51 second pass): controller with a camera, its camera, the gun and the humanoid. */
static uint32_t g_ctl, g_cam, g_gun, g_hum;

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static uint16_t rd16(uint32_t ea)
{
    uint16_t v;
    memcpy(&v, vm_base + ea, 2);
    return __builtin_bswap16(v);
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
static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}
static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}
static uint32_t w(uint32_t base, uint32_t off)
{
    return guest_ok(base) && guest_ok(base + off) ? rd32(base + off) : 0xDEADDEADu;
}
/* 28 words from ea+0x70 as floats (ASSUMPTION, pass 50: the entity transform block) and the bad-float count. */
static void xform(const char* tag, uint32_t ent)
{
    if (!guest_ok(ent)) return;
    char line[1100];
    unsigned bad = 0;
    int k = snprintf(line, sizeof line, "[mount-probe]   %s 0x%08X vt=0x%08X +70:", tag, ent, rd32(ent));
    for (unsigned i = 0; i < 28 && k < (int)sizeof line - 16; i++) {
        uint32_t v = rd32(ent + 0x70 + 4 * i);
        float f;
        memcpy(&f, &v, 4);
        if (f != f || f > 1e6f || f < -1e6f) bad++;
        k += snprintf(line + k, sizeof line - (size_t)k, " %.4g", (double)f);
    }
    fprintf(stderr, "%s bad=%u\n", line, bad);
}

#ifdef _WIN32
static DWORD WINAPI probe_thread(LPVOID p)
#else
static void* probe_thread(void* p)
#endif
{
    (void)p;
    long last = -1;
    for (;;) {
        sleep_ms(5000);
        long sum = g_ev2d + g_ready_fail;
        for (int i = 0; i < NK; i++) sum += g_n[i];
        if (sum == last && !g_ctl) continue;
        last = sum;
        lock();
        if (g_ctl) {
            fprintf(stderr, "[mount-probe] local mount t=%llu ctl=0x%08X ctl+14=0x%08X cam=0x%08X gun+270=%u gun+230=0x%08X\n",
                    now_ms(), g_ctl, w(g_ctl, 0x14), g_cam, w(g_gun, 0x270), w(g_gun, 0x230));
            xform("cam", g_cam);
            xform("gun", g_gun);
            xform("hum", g_hum);
        }
        fprintf(stderr,
                "[mount-probe] t=%llu Mount=%ld ready=%ld(fail %ld) evsend=%ld ev2d-all=%ld mount-now=%ld AttachUser=%ld "
                "substate-enter=%ld TransferCtl=%ld cam-activate=%ld FPCam-ctor=%ld\n",
                now_ms(), g_n[0], g_n[1], g_ready_fail, g_n[2], g_ev2d, g_n[3], g_n[4], g_n[5], g_n[6], g_n[7], g_n[8]);
        fflush(stderr);
        unlock();
    }
    return 0;
}

int kz3_mount_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_MOUNT_PROBE");
        const char* n = getenv("KZ3_MOUNT_PROBE_N");
        if (n) g_first = atol(n);
        g_on = e && *e && *e != '0';
        if (g_on) {
#ifdef _WIN32
            CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
#else
            pthread_t t;
            pthread_create(&t, NULL, probe_thread, NULL);
#endif
        }
    }
    return g_on;
}

static int want(long n)
{
    return n <= g_first || n % 500 == 0;
}

/* Entry of Mount 0x5CBAE4 (r3 gun, r4 humanoid, r5 name, r6, r7). */
void kz3_mount_probe_mount(uint32_t gun, uint32_t hum, uint32_t name, uint32_t b6, uint32_t b7, uint32_t lr)
{
    const long n = __atomic_add_fetch(&g_n[0], 1, __ATOMIC_RELAXED);
    if (!want(n)) return;
    char s[40] = "";
    if (guest_ok(name))
        for (int i = 0; i < 39; i++) {
            const char c = (char)vm_base[name + (uint32_t)i];
            if (!c) break;
            s[i] = (c >= 32 && c < 127) ? c : '?';
        }
    const uint32_t mover = w(hum, 0xFC);
    const uint32_t mvt = guest_ok(mover) ? rd32(mover) : 0;
    const uint32_t mslot0 = guest_ok(mvt) ? rd32(mvt) : 0; /* function descriptor of vtable slot 0 */
    const uint32_t res = w(gun, 0xE4);
    lock();
    fprintf(stderr,
            "[mount-probe] Mount #%ld t=%llu lr=0x%08X gun=0x%08X vt=0x%08X hum=0x%08X vt=0x%08X name='%s' b6=%u b7=%u "
            "C2 gun+270=%u C3 gun+230=0x%08X C5 mover=0x%08X mvt=0x%08X slot0=0x%08X(fn 0x%08X) res=0x%08X "
            "res+138=%u gun+218=%u gun+10C=0x%08X hum+10C=0x%08X\n",
            n, now_ms(), lr, gun, w(gun, 0), hum, w(hum, 0), s, b6 & 0xFF, b7 & 0xFF, w(gun, 0x270), w(gun, 0x230),
            mover, mvt, mslot0, guest_ok(mslot0) ? rd32(mslot0) : 0, res, w(res, 0x138), w(gun, 0x218), w(gun, 0x10C),
            w(hum, 0x10C));
    fflush(stderr);
    unlock();
}

/* After the readiness test 0x322940(entity) returned ret (only Mount's call sites are printed). */
void kz3_mount_probe_ready(uint32_t ent, uint32_t ret, uint32_t lr)
{
    if (lr < 0x5CBAE4u || lr >= 0x5CBE00u) return;
    const long n = __atomic_add_fetch(&g_n[1], 1, __ATOMIC_RELAXED);
    if (!(ret & 0xFF)) __atomic_add_fetch(&g_ready_fail, 1, __ATOMIC_RELAXED);
    if (!want(n)) return;
    const uint32_t node = w(ent, 0x10C);
    lock();
    fprintf(stderr, "[mount-probe] ready #%ld t=%llu lr=0x%08X ent=0x%08X ret=%u node=0x%08X node+4D=%u\n", n, now_ms(),
            lr, ent, ret & 0xFF, node, guest_ok(node) ? vm_base[node + 0x4D] : 0xFFu);
    fflush(stderr);
    unlock();
}

/* Entry of the event send 0x331648(mgr, ev, ...). */
void kz3_mount_probe_evsend(uint32_t mgr, uint32_t ev, uint32_t lr)
{
    const uint16_t id = guest_ok(ev) ? rd16(ev) : 0xFFFF;
    if (id == 0x2D) __atomic_add_fetch(&g_ev2d, 1, __ATOMIC_RELAXED);
    if (lr < 0x5CBAE4u || lr >= 0x5CBE00u) return;
    const long n = __atomic_add_fetch(&g_n[2], 1, __ATOMIC_RELAXED);
    if (!want(n)) return;
    lock();
    fprintf(stderr, "[mount-probe] evsend #%ld t=%llu lr=0x%08X mgr=0x%08X id=0x%X size=0x%X gun=0x%08X hum=0x%08X\n", n,
            now_ms(), lr, mgr, id, guest_ok(ev) ? rd16(ev + 2) : 0, w(ev, 4), w(ev, 8));
    fflush(stderr);
    unlock();
}

/* After hooks 3..8 (kind), r3 at entry = self, ret = r3 after. */
void kz3_mount_probe_mark(int kind, uint32_t self, uint32_t r4, uint32_t lr, uint32_t ret)
{
    if (kind < 3 || kind >= NK) return;
    const long n = __atomic_add_fetch(&g_n[kind], 1, __ATOMIC_RELAXED);
    if (!want(n)) return;
    lock();
    fprintf(stderr, "[mount-probe] %s #%ld t=%llu lr=0x%08X this=0x%08X vt=0x%08X r4=0x%08X ret=0x%08X", k_name[kind],
            n, now_ms(), lr, self, w(self, 0), r4, ret);
    if (kind == 7) {
        fprintf(stderr, " cam(+14)=0x%08X", w(self, 0x14));
        if (!g_ctl && guest_ok(w(self, 0x14))) {
            g_ctl = self;
            g_cam = w(self, 0x14) - 0xC; /* ASSUMPTION: +0x14 holds camera+0xC (gun+0x230 holds humanoid+0xC alike) */
        }
    }
    if (kind == 4 && !g_gun && (ret & 0xFF)) {
        g_gun = self;
        g_hum = r4;
    }
    if (kind == 4) fprintf(stderr, " gun+270=%u gun+230=0x%08X", w(self, 0x270), w(self, 0x230));
    fprintf(stderr, "\n");
    fflush(stderr);
    unlock();
}
