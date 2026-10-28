/* KZ3 GPU-pool probe (SPU pass 54; RE pass 120, the RE notes s.9.4): KZ3_POOL_PROBE=1, 1.00 only.
 *
 * K2's level-start state waits for six GPU-resource handles (ids 1531/1535/1536/1565/1569/1573 of the manager
 * 0x13F4FE0's table +0x110 = 0x13F50F0) whose storage res[0] is never allocated. Hooks:
 *   0x4BC70(mgr, handle)          handle registration (id = mgr+0x6110++ or the free list): logs ids 1480..1580.
 *   0x497F4(mgr, handle, size, align) pool allocation (writes res[0]): logs ids 1480..1580 and every 0 return with
 *                                 the pool words ([mgr+0xA8] used, [0x13F5004], mgr+0x1C/+0x20/+0x24/+0x48, the
 *                                 guard's TLS word [r13-0x6FF0], [mgr+0x100]) and the time spent (0 ms = guard path).
 *   0x4CA24                       vertex-buffer create: logs 0 returns.
 *   0x1B4CB8 / 0x1B4E14           Vertex/IndexArrayResource load vf15: counts; logs the first 5 and failures.
 *   0x4A200(S = mgr+0x60 = 0x13F5040) the per-frame stream-request service (Renderer::FrameTick 0x6A4EC, 0x1ECEE8):
 *                                 counted; 0x49D2C(S, handle) (alloc via 0x49C74 + queue a read 0x48FB8): ids logged.
 *   Monitor thread (pass 54 run 2): once a second, when it changed (or every 20 s), the streamer state: S.b[0],
 *     S+0x4C (pending), S+0x6C request count + its u16 ids at S+0x60, S+0x20 queue count, reader R = S+4: R+4 busy,
 *     R+0 index, R+0x18 handle in flight (id), queue R+0x20 entries (0x14 bytes, +0x10 handle: id, stamp +0x14),
 *     mgr+0x38 / mgr+0x3C (the reader skips an entry while [mgr+0x38] < 2 and stamp+1 >= [mgr+0x3C]).
 * Up to 400 lines in total; a summary line every 2 s while anything changed. */
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
static volatile long g_lines, g_calls, g_fails, g_regs, g_vb_fail, g_load[2];

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
static unsigned long tid(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)pthread_self();
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
static unsigned r16(uint32_t ea)
{
    return ok(ea) ? (unsigned)((vm_base[ea] << 8) | vm_base[ea + 1]) : 0xDEADu;
}
static int line_ok(void)
{
    return __sync_add_and_fetch(&g_lines, 1) <= 400;
}
static int watched(unsigned id)
{
    return id >= 1480 && id <= 1580;
}

static volatile long g_svc, g_req_lines;

static void state(char* b, int n)
{
    const uint32_t S = 0x13F5040u, R = S + 4, M = 0x13F4FE0u;
    const unsigned cnt = r32(S + 0x6C), q = r32(R + 0x1C), h = r32(R + 0x18);
    int k = snprintf(b, (size_t)n, "on=%u pend=%u req=%u:", vm_base[S], r32(S + 0x4C), cnt);
    for (unsigned i = 0; i < cnt && i < 8; i++) k += snprintf(b + k, (size_t)(n - k), " %u", r16(S + 0x60 + 2 * i));
    k += snprintf(b + k, (size_t)(n - k), " | busy=%u idx=%d infl=0x%08X(id %u e=%u) q=%u:", vm_base[R + 4],
                  (int)r32(R), h, h ? r16(h + 0xC) : 0, ok(h) ? vm_base[h + 0xE] : 0, q);
    const uint32_t qa = r32(R + 0x20);
    for (unsigned i = 0; i < q && i < 6 && k < n - 60; i++) {
        const uint32_t e = qa + 0x14 * i, eh = r32(e + 0x10);
        k += snprintf(b + k, (size_t)(n - k), " [%u st%u sz0x%X]", r16(eh + 0xC), r32(eh + 0x14), r32(e + 0xC));
    }
    snprintf(b + k, (size_t)(n - k), " | m38=%u m3C=%u rd8=%08X/%08X/%08X lst=0x%08X", r32(M + 0x38), r32(M + 0x3C),
             r32(R + 8), r32(R + 0xC), r32(R + 0x10), r32(R + 0x28));
}

#ifdef _WIN32
static DWORD WINAPI mon(LPVOID p)
#else
static void* mon(void* p)
#endif
{
    (void)p;
    static char last[600], cur[600];
    unsigned long long lastp = 0;
    for (int i = 0; i < 100000; i++) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        if (!vm_base) continue;
        state(cur, sizeof cur);
        const unsigned long long t = now_ms();
        if ((strcmp(cur, last) || t - lastp >= 20000) && line_ok()) {
            fprintf(stderr, "[pool-probe] stream t=%llu svc=%ld %s\n", t, g_svc, cur);
            fflush(stderr);
            memcpy(last, cur, sizeof last);
            lastp = t;
        }
    }
    return 0;
}

int kz3_pool_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_POOL_PROBE");
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

/* 0x4A200 entry */
void kz3_pool_probe_svc(void)
{
    __sync_add_and_fetch(&g_svc, 1);
}

/* 0x49D2C(S, handle) -> 1 queued / 0 */
void kz3_pool_probe_req(uint32_t handle, uint32_t ret, uint32_t lr)
{
    const unsigned id = r16(handle + 0xC);
    if (!(watched(id) || __sync_add_and_fetch(&g_req_lines, 1) <= 10) || !line_ok()) return;
    fprintf(stderr, "[pool-probe] req id=%u ret=%u t=%llu tid=%lu lr=0x%08X h=0x%08X [0]=0x%08X e=%u rec=0x%08X\n", id,
            ret, now_ms(), tid(), lr, handle, r32(handle), ok(handle) ? vm_base[handle + 0xE] : 0, r32(handle + 0x18));
    fflush(stderr);
}

static void summary(uint32_t mgr)
{
    static unsigned long long last;
    static long last_calls = -1;
    const unsigned long long t = now_ms();
    if (t - last < 2000 || g_calls == last_calls) return;
    last = t;
    last_calls = g_calls;
    if (!line_ok()) return;
    fprintf(stderr,
            "[pool-probe] sum t=%llu calls=%ld fails=%ld regs=%ld vbfail=%ld vload=%ld iload=%ld used(A8)=0x%08X "
            "budget-used=0x%08X lim(13F5004)=0x%08X next=%u free=%u busy100=%u\n",
            t, g_calls, g_fails, g_regs, g_vb_fail, g_load[0], g_load[1], r32(mgr + 0xA8), 0xBB00000u - r32(mgr + 0xA8),
            r32(0x13F5004u), r32(mgr + 0x6110), r32(mgr + 0x6114), r32(mgr + 0x100));
    fflush(stderr);
}

/* 0x4BC70 */
void kz3_pool_probe_reg(uint32_t mgr, uint32_t handle, uint32_t lr)
{
    __sync_add_and_fetch(&g_regs, 1);
    const unsigned id = r16(handle + 0xC);
    if (!watched(id) || !line_ok()) return;
    fprintf(stderr,
            "[pool-probe] reg id=%u t=%llu tid=%lu lr=0x%08X mgr=0x%08X h=0x%08X [0]=0x%08X sz=0x%08X next=%u\n", id,
            now_ms(), tid(), lr, mgr, handle, r32(handle), r32(handle + 4), r32(mgr + 0x6110));
    fflush(stderr);
}

/* 0x497F4: called before (after=0, returns a start time) and after the lifted body. */
unsigned long long kz3_pool_probe_alloc(int after, unsigned long long t0, uint32_t mgr, uint32_t handle, uint32_t size,
                                        uint32_t align, uint32_t ret, uint32_t r13, uint32_t lr)
{
    if (!after) return now_ms();
    __sync_add_and_fetch(&g_calls, 1);
    const unsigned id = r16(handle + 0xC);
    if (!ret) __sync_add_and_fetch(&g_fails, 1);
    if ((ret == 0 || watched(id)) && line_ok()) {
        const unsigned long long t = now_ms();
        fprintf(
            stderr,
            "[pool-probe] alloc %s id=%u t=%llu dt=%llums tid=%lu lr=0x%08X h=0x%08X size=0x%X align=0x%X [0]=0x%08X "
            "m1=%u used(A8)=0x%08X lim(13F5004)=0x%08X m1C=0x%08X m20=0x%08X m24=0x%08X m48=0x%08X busy100=%u "
            "tls(r13-6FF0)=0x%08X fails=%ld/%ld\n",
            ret ? "ok" : "FAIL", id, t, t - t0, tid(), lr, handle, size, align, r32(handle),
            ok(mgr) ? vm_base[mgr + 1] : 0xDE, r32(mgr + 0xA8), r32(0x13F5004u), r32(mgr + 0x1C), r32(mgr + 0x20),
            r32(mgr + 0x24), r32(mgr + 0x48), r32(mgr + 0x100), r32(r13 - 0x6FF0u), g_fails, g_calls);
        fflush(stderr);
    }
    summary(mgr);
    return 0;
}

/* 0x4CA24 */
void kz3_pool_probe_vb(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t ret, uint32_t lr)
{
    if (ret) return;
    __sync_add_and_fetch(&g_vb_fail, 1);
    if (!line_ok()) return;
    fprintf(stderr, "[pool-probe] vbcreate FAIL t=%llu tid=%lu lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
            now_ms(), tid(), lr, r3, r4, r5, r6);
    fflush(stderr);
}

/* 0x1B4CB8 (kind 0) / 0x1B4E14 (kind 1) */
void kz3_pool_probe_load(int kind, uint32_t self, uint32_t lr)
{
    const long n = __sync_add_and_fetch(&g_load[kind], 1);
    const uint32_t r8 = r32(self + 8);
    if ((n > 5 && r8) || !line_ok()) return;
    fprintf(stderr, "[pool-probe] %s-load #%ld t=%llu tid=%lu lr=0x%08X this=0x%08X vt=0x%08X [+8]=0x%08X\n",
            kind ? "index" : "vertex", n, now_ms(), tid(), lr, self, r32(self), r8);
    fflush(stderr);
}

/* Pass 54 run 4: 0x1C5208(obj = 0x1514AD0) builds the Gfx.TextureStreaming job (module 42, RenderDataStreamingSPU,
 * image job_01181700) with user data obj+0x2070 (0x40 B: bank pointers +0x2070.., +0x2080 bank count = obj+0x14,
 * +0x2090 the S+0x50 lock, +0x2098 table 0x13F50F0, +0x209C [0x13FB0F0] id count, +0x20A0 budget, +0x20A4 request
 * list 0x13F50A0). Logs obj+4/+8/+0xC/+0x14, the bank heads and where K2's six ids sit in the banks (first 6 calls
 * with banks, then every 300th, up to 40 lines). */
void kz3_pool_probe_job(uint32_t obj, int after)
{
    static long calls, lines;
    const uint32_t nb = r32(obj + 0x14);
    if (!nb || after) return;
    calls++;
    if (lines >= 40 || (calls > 6 && calls % 300)) return;
    if (!line_ok()) return;
    lines++;
    static const unsigned six[6] = {1531, 1535, 1536, 1565, 1569, 1573};
    char b[700];
    int k = snprintf(b, sizeof b, "[pool-probe] job42 #%ld t=%llu obj+4=0x%08X +8=0x%08X +C=0x%08X banks=%u", calls,
                     now_ms(), r32(obj + 4), r32(obj + 8), r32(obj + 0xC), nb);
    for (unsigned i = 0; i < nb && i < 4; i++) {
        const uint32_t bk = obj + 0x20 + 0x810 * i;
        k += snprintf(b + k, sizeof b - (size_t)k, " | b%u:", i);
        for (unsigned w = 0; w < 6; w++) k += snprintf(b + k, sizeof b - (size_t)k, " %08X", r32(bk + 4 * w));
        for (uint32_t o = 0; o + 2 <= 0x810 && k < (int)sizeof b - 30; o += 2)
            for (int s = 0; s < 6; s++)
                if (r16(bk + o) == six[s]) k += snprintf(b + k, sizeof b - (size_t)k, " id%u@+0x%X", six[s], o);
    }
    fprintf(stderr, "%s\n", b);
    fflush(stderr);
}

/* Pass 55: deep dump of one input set of the TextureStreaming job (calls 100/250/400/550 with views):
 * the user data obj+0x2070 (0x40 B), view 0 (obj+0x20) words 0..0x3C and 0x7F0..0x82C, the resource-user list
 * ([obj+8] = array of [obj+4] entry pointers; entry {+0 listener, +4/+8/+0xC ?, +0xE u16 group count, groups from
 * +0x10 {12 B header, +0xC u16 n, n u16 ids}}: layout from 0x1C5704), every entry that holds one of K2's six ids
 * (header words, the words behind +4/+8 when they point to memory, each group header), and the six ids' table
 * entries (0x13F50F0 + 4*id -> handle words 0..0x1C, stream record [h+0x18]). */
static void wline(const char* tag, uint32_t ea, unsigned nw)
{
    char b[400];
    int k = snprintf(b, sizeof b, "[pool-probe] deep %s @0x%08X:", tag, ea);
    for (unsigned w = 0; w < nw && k < (int)sizeof b - 12; w++) k += snprintf(b + k, sizeof b - (size_t)k, " %08X", r32(ea + 4 * w));
    fprintf(stderr, "%s\n", b);
}
void kz3_pool_probe_deep(uint32_t obj)
{
    static long calls, dumps;
    if (!r32(obj + 0x14)) return;
    ++calls;
    if (dumps >= 4 || (calls != 100 && calls != 250 && calls != 400 && calls != 550)) return;
    dumps++;
    static const unsigned six[6] = {1531, 1535, 1536, 1565, 1569, 1573};
    fprintf(stderr, "[pool-probe] deep #%ld call=%ld t=%llu\n", dumps, calls, now_ms());
    wline("ud", obj + 0x2070, 16);
    wline("v0a", obj + 0x20, 16);
    wline("v0b", obj + 0x20 + 0x40, 16);
    wline("v0z", obj + 0x20 + 0x7F0, 16);
    const uint32_t n = r32(obj + 4), arr = r32(obj + 8);
    unsigned nz = 0, ids = 0, hit = 0;
    for (uint32_t i = 0; i < n && i < 0x2000; i++) {
        const uint32_t e = r32(arr + 4 * i);
        if (!ok(e)) continue;
        nz++;
        const unsigned ng = r16(e + 0xE);
        uint32_t g = e + 0x10;
        int has = 0;
        for (unsigned j = 0; j < ng && j < 64; j++) {
            const unsigned c = r16(g + 0xC);
            for (unsigned q = 0; q < c && q < 512; q++) {
                ids++;
                for (int s6 = 0; s6 < 6; s6++) if (r16(g + 0xE + 2 * q) == six[s6]) has = 1;
            }
            g += 0xE + 2 * c;
        }
        if (!has || hit >= 6) continue;
        hit++;
        char t[32];
        snprintf(t, sizeof t, "ent%u", i);
        wline(t, e, 8);
        if (ok(r32(e + 4))) wline("  [e+4]", r32(e + 4), 12);
        if (ok(r32(e + 8))) wline("  [e+8]", r32(e + 8), 12);
        if (ok(r32(e))) wline("  lsn", r32(e), 8);
        g = e + 0x10;
        for (unsigned j = 0; j < ng && j < 8; j++) {
            const unsigned c = r16(g + 0xC);
            char b[300];
            int k = snprintf(b, sizeof b, "[pool-probe] deep   grp%u hdr %08X %08X %08X n=%u ids:", j, r32(g), r32(g + 4),
                             r32(g + 8), c);
            for (unsigned q = 0; q < c && q < 16; q++) k += snprintf(b + k, sizeof b - (size_t)k, " %u", r16(g + 0xE + 2 * q));
            fprintf(stderr, "%s\n", b);
            g += 0xE + 2 * c;
        }
    }
    fprintf(stderr, "[pool-probe] deep users n=%u nonnull=%u ids=%u with-six=%u\n", n, nz, ids, hit);
    for (int s6 = 0; s6 < 6; s6++) {
        const uint32_t h = r32(0x13F50F0u + 4 * six[s6]);
        char t[32];
        snprintf(t, sizeof t, "id%u", six[s6]);
        wline(t, h, 8);
        if (ok(r32(h + 0x18))) wline("  rec", r32(h + 0x18), 6);
    }
    fflush(stderr);
}

/* Pass 55: nw big-endian words at ea, tagged with the hook's call number (frustum hooks 0x31E328 / 0x1D0D50). */
void kz3_pool_probe_words(const char* tag, uint32_t ea, unsigned nw, long call)
{
    char t[48];
    snprintf(t, sizeof t, "%s #%ld", tag, call);
    for (unsigned i = 0; i < nw; i += 16) wline(t, ea + 4 * i, nw - i < 16 ? nw - i : 16);
    fflush(stderr);
}

/* Pass 55: the render view's camera ([rv+0x10]): vtable, vt+0x44, and its first 0x200 bytes. */
void kz3_pool_probe_cam(uint32_t rv, long call)
{
    const uint32_t cam = r32(rv + 0x10), vt = ok(cam) ? r32(cam) : 0;
    fprintf(stderr, "[pool-probe] deep cam #%ld rview=0x%08X cam=0x%08X vt=0x%08X vt44=0x%08X\n", call, rv, cam, vt,
            ok(vt) ? r32(vt + 0x44) : 0);
    kz3_pool_probe_words("rview", rv, 32, call);
    if (ok(cam)) kz3_pool_probe_words("cam", cam, 128, call);
}

/* Pass 56: camera vt+0x40 (@0x0031DFB8, r3 camera, r4 source description) builds the matrix at cam+0x10..0x4F from
 * r4 (0x36C30 copies 11 words, 0x75850 makes the matrix) and stores it with stvx (not seen by PPU_WVAL). Logs the
 * first 4 calls and the first 24 whose stored matrix has an exponent-255 word: caller, camera, source and result. */
static int exp255(uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u; }
void kz3_pool_probe_camset(uint32_t cam, uint32_t src, uint32_t lr, long call)
{
    static int nbad;
    int bad = 0;
    for (unsigned i = 0; i < 16 && ok(cam); i++) bad |= exp255(r32(cam + 0x10 + 4 * i));
    if (!(call <= 4 || (bad && nbad < 24))) return;
    if (bad) nbad++;
    int sbad = 0;
    for (unsigned i = 0; i < 16 && ok(src); i++) sbad |= exp255(r32(src + 4 * i));
    fprintf(stderr, "[pool-probe] camset #%ld cam=0x%08X vt=0x%08X src=0x%08X lr=0x%08X bad=%d srcbad=%d\n", call, cam,
            ok(cam) ? r32(cam) : 0, src, lr, bad, sbad);
    char t[48];
    snprintf(t, sizeof t, "camset-src #%ld", call);
    if (ok(src)) wline(t, src, 16);
    snprintf(t, sizeof t, "camset-mat #%ld", call);
    if (ok(cam)) wline(t, cam + 0x10, 16);
    fflush(stderr);
}
