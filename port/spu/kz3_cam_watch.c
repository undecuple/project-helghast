/* KZ3_CAM_WATCH=1 (pass 56, diagnostic, default off): who writes the K2 render camera's matrix.
 *
 * The camera (vt 0xEBA1D8, derived ctor @0x0031EB04 with r4 = this, base ctor @0x0031DD00 stores an identity at
 * cam+0x10..0x4F with stvx) reads 0xFFF50000 in all 16 matrix words in K2 (pass 55). Lifted stvx is a plain memcpy and
 * SPU DMA is a memcpy too, so neither PPU_WVAL nor PPU_WWATCH sees the store. This watch makes the camera's 4 KB host
 * page read-only after the ctor; a vectored exception handler catches each write to the page, logs the slot's first two writes to
 * cam+0x10..0x4F and every write that makes a matrix word exponent-255 (host RIP as an offset into kz3.exe, resolve with nm; plus a host back trace), lets the one
 * instruction run (trap flag), dumps the matrix's first words and protects the page again. Windows (Proton) only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;

static int s_on = -1;
int kz3_cam_watch_on(void)
{
    if (s_on < 0) {
        const char* e = getenv("KZ3_CAM_WATCH");
        s_on = e && *e == '1';
    }
    return s_on;
}

#ifdef _WIN32
#define NSLOT 32
static struct {
    uintptr_t page;
    uint32_t lo, hi;
} s_slot[NSLOT];
static volatile LONG s_nslot, s_hits;
static __thread int t_pending; /* slot + 1 while one write is single-stepped */
static __thread int t_hit;
static __thread uint32_t t_ea, t_before[16];
static __thread unsigned long long t_rip;
static __thread void* t_bt[16];
static __thread USHORT t_nbt;
static volatile LONG s_slot_hits[NSLOT], s_slot_intro[NSLOT];

static void arm_range(uint32_t cam);
static uint32_t rd(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}

static int find_slot(uintptr_t a)
{
    for (int i = 0; i < s_nslot && i < NSLOT; i++)
        if (s_slot[i].page && a - s_slot[i].page < 0x1000u) return i;
    return -1;
}

static LONG CALLBACK cam_veh(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        const uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        const int i = find_slot(a);
        if (i < 0) return EXCEPTION_CONTINUE_SEARCH;
        const uint32_t ea = (uint32_t)(a - (uintptr_t)vm_base);
        t_hit = 0;
        int m = -1;
        for (int k = 0; k < s_nslot && k < NSLOT; k++)
            if (s_slot[k].page == s_slot[i].page && ea + 16 > s_slot[k].lo && ea < s_slot[k].hi) m = k;
        if (m >= 0) {
            /* Remember where and what; the step handler prints the store only when it is one of the slot's first
             * two, or when it makes a matrix word exponent-255 (the 0xFFF50000 question). */
            const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
            t_ea = ea;
            t_rip = (unsigned long long)(ep->ContextRecord->Rip - base);
            t_nbt = RtlCaptureStackBackTrace(0, 16, t_bt, NULL);
            for (int k = 0; k < 16; k++) t_before[k] = rd(s_slot[m].lo + 4 * k);
            t_hit = m + 1;
        }
        DWORD old;
        VirtualProtect((void*)s_slot[i].page, 0x1000, PAGE_READWRITE, &old);
        t_pending = (m >= 0 ? m : i) + 1;
        ep->ContextRecord->EFlags |= 0x100; /* trap after the one store */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_pending) {
        const int i = t_pending - 1;
        t_pending = 0;
        if (t_hit) {
            const int m = t_hit - 1;
            const uint32_t c = s_slot[m].lo - 0x10;
            /* EXP255: this store left an exponent-255 word in its 16 bytes of the watched range. */
            int intro = 0;
            for (int k = 0; k < 16; k++) {
                const uint32_t a = c + 0x10 + 4 * k, w = rd(a);
                intro |= ((w & 0x7F800000u) == 0x7F800000u) && a + 4 > t_ea && a < t_ea + 16;
            }
            const LONG n = InterlockedIncrement(&s_slot_hits[m]);
            const LONG ni = intro ? InterlockedIncrement(&s_slot_intro[m]) : 0;
            if ((n <= 2 || (intro && ni <= 8)) && s_hits < 96) {
                const LONG h = InterlockedIncrement(&s_hits);
                fprintf(stderr, "[cam-watch] hit %ld (slot %d #%ld%s) ea=0x%08X cam=0x%08X tid=%lu rip=+0x%llx bt:", h,
                        m, n, intro ? " EXP255" : "", t_ea, c, (unsigned long)GetCurrentThreadId(), t_rip);
                const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
                for (USHORT k = 0; k < t_nbt; k++)
                    fprintf(stderr, " +0x%llx", (unsigned long long)((uintptr_t)t_bt[k] - base));
                fprintf(stderr,
                        "\n[cam-watch] after %ld: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | "
                        "%08X %08X %08X %08X\n",
                        h, rd(c + 0x10), rd(c + 0x14), rd(c + 0x18), rd(c + 0x1C), rd(c + 0x20), rd(c + 0x24),
                        rd(c + 0x28), rd(c + 0x2C), rd(c + 0x30), rd(c + 0x34), rd(c + 0x38), rd(c + 0x3C),
                        rd(c + 0x40), rd(c + 0x44), rd(c + 0x48), rd(c + 0x4C));
                fflush(stderr);
            }
            t_hit = 0;
        }
        DWORD old;
        VirtualProtect((void*)s_slot[i].page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Called after the derived ctor @0x0031EB04 (cam = r4). Arms the first NSLOT (32) cameras whose matrix does not cross a
 * page boundary. */
static void arm_range(uint32_t cam);
void kz3_cam_watch_arm(uint32_t cam, uint32_t lr)
{
    if (!kz3_cam_watch_on() || cam < 0x10000u) return;
    fprintf(stderr, "[cam-watch] ctor cam=0x%08X lr=0x%08X m0=%08X m5=%08X\n", cam, lr, rd(cam + 0x10), rd(cam + 0x24));
    arm_range(cam);
}

/* Pass 56: @0x0038C3D8 (r3 out, r4 camera component) copies comp+0x60..0x9F into the render-camera description. The
 * first 4 components whose matrix has an exponent-255 word: dump them and watch comp+0x60..0x9F (slot "cam" = comp+0x50,
 * so the printed matrix is comp+0x60). */
void kz3_cam_watch_comp(uint32_t comp, uint32_t lr)
{
    static uint32_t seen[4];
    static LONG nseen;
    if (!kz3_cam_watch_on() || comp < 0x10000u) return;
    int bad = 0;
    for (int k = 0; k < 16; k++) bad |= (rd(comp + 0x60 + 4 * k) & 0x7F800000u) == 0x7F800000u;
    if (!bad) return;
    for (int k = 0; k < 4; k++)
        if (seen[k] == comp) return;
    const LONG i = InterlockedIncrement(&nseen) - 1;
    if (i >= 4) return;
    seen[i] = comp;
    const uint32_t vt = rd(comp);
    fprintf(stderr, "[cam-watch] comp 0x%08X vt=0x%08X lr=0x%08X\n", comp, vt, lr);
    for (uint32_t o = 0; o < 0x220; o += 0x20)
        fprintf(stderr, "[cam-watch] comp +0x%03X: %08X %08X %08X %08X %08X %08X %08X %08X\n", o, rd(comp + o),
                rd(comp + o + 4), rd(comp + o + 8), rd(comp + o + 12), rd(comp + o + 16), rd(comp + o + 20),
                rd(comp + o + 24), rd(comp + o + 28));
    arm_range(comp + 0x50);
}

static void arm_range(uint32_t cam)
{
    static LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) AddVectoredExceptionHandler(1, cam_veh);
    const uintptr_t h0 = (uintptr_t)(vm_base + cam + 0x10), h1 = (uintptr_t)(vm_base + cam + 0x4F);
    if ((h0 & ~(uintptr_t)0xFFF) != (h1 & ~(uintptr_t)0xFFF)) return;
    const LONG i = InterlockedIncrement(&s_nslot) - 1;
    if (i >= NSLOT) return;
    s_slot[i].lo = cam + 0x10;
    s_slot[i].hi = cam + 0x50;
    s_slot[i].page = h0 & ~(uintptr_t)0xFFF;
    DWORD old;
    const BOOL okp = VirtualProtect((void*)s_slot[i].page, 0x1000, PAGE_READONLY, &old);
    fprintf(stderr, "[cam-watch] armed slot %ld cam=0x%08X protect=%d old=0x%lx\n", i, cam, (int)okp,
            (unsigned long)old);
    fflush(stderr);
}
/* Pass 56: @0x005BCB90 (r3 = this, an entity part whose camera component's matrix comp+0x60 it rewrites from a
 * vt+0x68 getter): the first 6 calls, this + vtable + the objects it reads ([+0x12C], [+0x1EC]). */
void kz3_cam_watch_part(uint32_t self, uint32_t lr)
{
    static volatile LONG n;
    if (!kz3_cam_watch_on() || self < 0x10000u || InterlockedIncrement(&n) > 6) return;
    const uint32_t a = rd(self + 0x12C), b = rd(self + 0x1EC);
    fprintf(stderr,
            "[cam-watch] part 0x%08X vt=0x%08X lr=0x%08X [+0x12C]=0x%08X vt=0x%08X [+0x1EC]=0x%08X vt=0x%08X "
            "[b+0xB4]=0x%08X\n",
            self, rd(self), lr, a, a >= 0x10000u ? rd(a) : 0, b, b >= 0x10000u ? rd(b) : 0,
            b >= 0x10000u ? rd(b + 0xB4) : 0);
    fflush(stderr);
}
/* Pass 56: @0x005E0C88 = part vt+0x68 (r3 part, r4 out matrix): the gunner view from two skeleton bones (indices
 * [[skel+0xC]+0x2C] and [+0x24], matrices [skel + skel.b[0xB0]*64 + 0x40] + idx*64, @0x855804) and the smoothed yaw /
 * pitch at part+0x2E8/0x2EC (targets [[part+0x12C]+0x37C/0x380], rate part+0x124). Logs the inputs before and the
 * output after for the first 3 calls and the first 4 whose output has an exponent-255 word. */
static void words(const char* tag, uint32_t ea, unsigned n)
{
    fprintf(stderr, "[cam-watch] %s @0x%08X:", tag, ea);
    for (unsigned k = 0; k < n; k++) fprintf(stderr, " %08X", ea >= 0x10000u ? rd(ea + 4 * k) : 0);
    fprintf(stderr, "\n");
}
void kz3_cam_watch_view(uint32_t part, uint32_t out, int after, long call)
{
    static volatile LONG nbad;
    if (!kz3_cam_watch_on() || part < 0x10000u) return;
    int bad = 0;
    for (int k = 0; k < 16 && after && out >= 0x10000u; k++) bad |= (rd(out + 4 * k) & 0x7F800000u) == 0x7F800000u;
    if (after && !(call <= 3 || (bad && InterlockedIncrement(&nbad) <= 4))) return;
    if (!after && !(call <= 3)) {
        /* inputs are printed for the first 3 calls only, plus (below) whenever an exponent-255 output is reported */
    }
    if (!after) return;
    const uint32_t tgt = rd(part + 0x12C), skel = rd(part + 0x1EC);
    const uint32_t info = skel >= 0x10000u ? rd(skel + 0xC) : 0;
    const int32_t ia = info >= 0x10000u ? (int32_t)rd(info + 0x2C) : -2,
                  ib = info >= 0x10000u ? (int32_t)rd(info + 0x24) : -2;
    const uint32_t mb = skel >= 0x10000u ? rd(skel + ((rd(skel + 0xB0) >> 24) & 0xFF) * 64 + 0x40) : 0;
    fprintf(stderr,
            "[cam-watch] view #%ld part=0x%08X out=0x%08X bad=%d yaw=%08X pitch=%08X rate=%08X tgt=0x%08X "
            "tyaw=%08X tpitch=%08X skel=0x%08X info=0x%08X ia=%d ib=%d mats=0x%08X\n",
            call, part, out, bad, rd(part + 0x2E8), rd(part + 0x2EC), rd(part + 0x124), tgt,
            tgt >= 0x10000u ? rd(tgt + 0x37C) : 0, tgt >= 0x10000u ? rd(tgt + 0x380) : 0, skel, info, ia, ib, mb);
    if (mb >= 0x10000u && ia >= 0) words("boneA", mb + (uint32_t)ia * 64, 16);
    /* calls 1 and 2 see both buffers of the double-buffered bone matrices: watch bone A in each */
    if (call <= 2 && mb >= 0x10000u && ia >= 0) arm_range(mb + (uint32_t)ia * 64 - 0x10);
    if (mb >= 0x10000u && ib >= 0) words("boneB", mb + (uint32_t)ib * 64, 16);
    words("out", out, 16);
    fflush(stderr);
}
#else
void kz3_cam_watch_view(uint32_t part, uint32_t out, int after, long call)
{
    (void)part;
    (void)out;
    (void)after;
    (void)call;
}
void kz3_cam_watch_part(uint32_t self, uint32_t lr)
{
    (void)self;
    (void)lr;
}
void kz3_cam_watch_arm(uint32_t cam, uint32_t lr)
{
    (void)cam;
    (void)lr;
}
void kz3_cam_watch_comp(uint32_t comp, uint32_t lr)
{
    (void)comp;
    (void)lr;
}
#endif
