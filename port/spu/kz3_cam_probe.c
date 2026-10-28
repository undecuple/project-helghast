/* KZ3 follow-camera probe (SPU pass 50; RE pass 109 s.3, K2 Pyrrhus Evac black after the level start): KZ3_CAM_PROBE=1.
 *
 * Hooks (1.00 addresses only; port/spu/ppu_overrides.txt rows, kz3_ppu_overrides.cpp):
 *   0x597170 ControlledFollowCameraEntity::vf18(this, f1 = dt) - the follow-camera update. DirectionElastic...::vf18
 *            0x599068 (vtable 0xECB938 slot 18, the K2 Hammer camera class) calls it between 0x598F70 and 0x598EAC, so
 *            this hook sees both classes.
 *   0x5B1558 CameraEntity FOV helper(this) -> f1 (the RE notes: FOV +0x164, view = (this+0x148)+0x80).
 * Printed (read-only on guest memory): class by vtable, this+0x70..0xDC (28 floats: ASSUMPTION the entity transform
 * block, 0x597170 reads target+0xA0..0xC8 and parent+0x70..0x78/0xB0..0xB8 as 3-vectors), the follow target
 * *(this+0xF0) and its same block, the camera state this+0x1B0..0x1EC, a NaN/Inf/|x|>1e6 flag over all of them.
 * The first KZ3_CAM_PROBE_N (default 30) calls of each hook, the first 3 calls of each new `this`, every 300th call,
 * and the first 40 calls with a bad float; a summary line every 5 s from a thread. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

extern uint8_t* vm_base;

static int g_on = -1;
static unsigned g_first = 30;
static volatile long g_n[2], g_bad[2], g_lines_bad;
static volatile long g_k[4]; /* second pass counters, see kz3_cam_probe_mark */
static uint32_t g_this[16];
static unsigned g_this_n[16];
static volatile long g_lock;

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static float rdf(uint32_t ea)
{
    uint32_t v = rd32(ea);
    float f;
    memcpy(&f, &v, 4);
    return f;
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

static const char* cls(uint32_t vt)
{
    switch (vt) {
    case 0xECB568:
        return "ControlledFollowCam";
    case 0xECB938:
        return "DirElasticCtrlFollowCam";
    case 0xECBE50:
        return "ElasticVehicleFollowCam";
    case 0xECC218:
        return "PivotFollowCam";
    case 0xECE038:
        return "FollowCam";
    default:
        return "?";
    }
}

/* Count bad floats in [ea, ea + 4n). */
static unsigned bad_floats(uint32_t ea, unsigned n)
{
    unsigned b = 0;
    if (!guest_ok(ea)) return 0;
    for (unsigned i = 0; i < n; i++) {
        const float f = rdf(ea + 4 * i);
        if (isnan(f) || isinf(f) || fabsf(f) > 1e6f) b++;
    }
    return b;
}

static void floats(const char* tag, uint32_t ea, unsigned n)
{
    char line[1200];
    int k = snprintf(line, sizeof line, "[cam-probe]   %s 0x%08X:", tag, ea);
    for (unsigned i = 0; i < n && k < (int)sizeof line - 16; i++) {
        if (!guest_ok(ea)) {
            k += snprintf(line + k, sizeof line - (size_t)k, " (bad ea)");
            break;
        }
        k += snprintf(line + k, sizeof line - (size_t)k, " %.4g", (double)rdf(ea + 4 * i));
    }
    fprintf(stderr, "%s\n", line);
}

/* Returns 1 when `self` was not seen before (and records it). */
static int seen_new(uint32_t self, unsigned* calls)
{
    lock();
    for (int i = 0; i < 16; i++) {
        if (g_this[i] == self) {
            *calls = ++g_this_n[i];
            unlock();
            return 0;
        }
        if (!g_this[i]) {
            g_this[i] = self;
            g_this_n[i] = 1;
            *calls = 1;
            unlock();
            return 1;
        }
    }
    unlock();
    *calls = 0;
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI probe_thread(LPVOID p)
#else
static void* probe_thread(void* p)
#endif
{
    (void)p;
    long last0 = -1, last1 = -1;
    for (;;) {
        sleep_ms(5000);
        const long sum = g_n[0] + g_n[1] + g_k[0] + g_k[1] + g_k[2] + g_k[3];
        if (sum == last0) continue;
        last0 = sum;
        (void)last1;
        char line[600];
        int k = snprintf(line, sizeof line,
                         "[cam-probe] t=%llu follow=%ld bad=%ld fov=%ld bad=%ld decam-ctor=%ld veh-ctor=%ld "
                         "veh-vf18=%ld veh-vf136=%ld this:",
                         now_ms(), g_n[0], g_bad[0], g_n[1], g_bad[1], g_k[0], g_k[1], g_k[2], g_k[3]);
        for (int i = 0; i < 16 && g_this[i] && k < (int)sizeof line - 40; i++)
            k += snprintf(line + k, sizeof line - (size_t)k, " %08X(%s)x%u", g_this[i], cls(rd32(g_this[i])),
                          g_this_n[i]);
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
    return 0;
}

int kz3_cam_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_CAM_PROBE");
        const char* n = getenv("KZ3_CAM_PROBE_N");
        if (n) g_first = (unsigned)atoi(n);
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

/* After 0x597170 returned. */
void kz3_cam_probe_follow(uint32_t self, float dt, uint32_t lr)
{
    const long n = __atomic_add_fetch(&g_n[0], 1, __ATOMIC_RELAXED);
    unsigned calls;
    const int fresh = seen_new(self, &calls);
    const uint32_t vt = guest_ok(self) ? rd32(self) : 0;
    const uint32_t tgt = guest_ok(self) ? rd32(self + 0xF0) : 0;
    unsigned bad =
        bad_floats(self + 0x70, 28) + bad_floats(self + 0x1B0, 16) + (guest_ok(tgt) ? bad_floats(tgt + 0x70, 28) : 0);
    int show = n <= (long)g_first || fresh || calls <= 3 || n % 300 == 0;
    if (bad) {
        __atomic_add_fetch(&g_bad[0], 1, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&g_lines_bad, 1, __ATOMIC_RELAXED) <= 40) show = 1;
    }
    if (!show) return;
    lock();
    fprintf(
        stderr,
        "[cam-probe] follow #%ld t=%llu this=0x%08X vt=0x%08X %s dt=%.4g lr=0x%08X res=0x%08X tgt=0x%08X tvt=0x%08X "
        "+1D8=0x%08X bad=%u\n",
        n, now_ms(), self, vt, cls(vt), (double)dt, lr, guest_ok(self) ? rd32(self + 0xE4) : 0, tgt,
        guest_ok(tgt) ? rd32(tgt) : 0, guest_ok(self) ? rd32(self + 0x1D8) : 0, bad);
    floats("cam+70", self + 0x70, 28);
    floats("cam+1B0", self + 0x1B0, 16);
    if (guest_ok(tgt)) floats("tgt+70", tgt + 0x70, 28);
    fflush(stderr);
    unlock();
}

/* After 0x5B1558 returned f1. */
void kz3_cam_probe_fov(uint32_t self, float fov, uint32_t lr)
{
    const long n = __atomic_add_fetch(&g_n[1], 1, __ATOMIC_RELAXED);
    const uint32_t vt = guest_ok(self) ? rd32(self) : 0;
    const uint32_t v148 = guest_ok(self) ? rd32(self + 0x148) : 0;
    unsigned bad = bad_floats(self + 0x70, 28) + (isnan(fov) || isinf(fov) ? 1u : 0u) +
                   (guest_ok(v148) ? bad_floats(v148 + 0x80, 16) : 0);
    int show = n <= (long)g_first || n % 600 == 0;
    if (bad) {
        __atomic_add_fetch(&g_bad[1], 1, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&g_lines_bad, 1, __ATOMIC_RELAXED) <= 40) show = 1;
    }
    if (!show) return;
    lock();
    fprintf(stderr,
            "[cam-probe] fov #%ld t=%llu this=0x%08X vt=0x%08X %s fov=%.4g lr=0x%08X +194=0x%08X +148=0x%08X bad=%u\n",
            n, now_ms(), self, vt, cls(vt), (double)fov, lr, guest_ok(self) ? rd32(self + 0x194) : 0, v148, bad);
    floats("cam+70", self + 0x70, 28);
    if (guest_ok(v148)) floats("view(+148)+80", v148 + 0x80, 16);
    fflush(stderr);
    unlock();
}

/* Pass 50 second pass (the first K2 run saw no call of 0x597170 / 0x5B1558 at all): counters on the K2 Hammer's
 * objects. kind 0 = DirectionElasticControlledFollowCameraEntity::ctor 0x59FDA0, 1 = VehicleOnRails::ctor 0x64043C,
 * 2 = VehicleOnRails::vf18 0x63C784 (this), 3 = VehicleOnRails::vf136 0x63CB40 (returns the matrix EA: 16 floats,
 * position = +0x30..+0x38 as in kz3_gameplay.h's vf136 reader). First KZ3_CAM_PROBE_N calls per kind, then every 300th. */
static const char* const k_name[4] = {"DECam::ctor", "VehicleOnRails::ctor", "VehicleOnRails::vf18",
                                      "VehicleOnRails::vf136"};
void kz3_cam_probe_mark(int kind, uint32_t self, uint32_t lr, uint32_t ret)
{
    if (kind < 0 || kind > 3) return;
    const long n = __atomic_add_fetch(&g_k[kind], 1, __ATOMIC_RELAXED);
    if (n > (long)g_first && n % 300 != 0) return;
    lock();
    fprintf(stderr, "[cam-probe] %s #%ld t=%llu this=0x%08X vt=0x%08X lr=0x%08X ret=0x%08X\n", k_name[kind], n,
            now_ms(), self, guest_ok(self) ? rd32(self) : 0, lr, ret);
    if (kind == 3 && guest_ok(ret)) {
        floats("matrix", ret, 16);
        if (bad_floats(ret, 16)) fprintf(stderr, "[cam-probe]   matrix has %u bad float(s)\n", bad_floats(ret, 16));
    }
    if (kind == 2 && guest_ok(self)) floats("veh+70", self + 0x70, 28);
    fflush(stderr);
    unlock();
}
