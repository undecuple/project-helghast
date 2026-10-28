/* Patch 2025 host test: conditional rendering (NV4097 SET_RENDER_ENABLE mode 2) from ZPASS occlusion reports.
 * Killzone 3's level frame tests one proxy per light / shadow cascade (CLEAR_REPORT_VALUE 1, one colour-masked draw,
 * GET_REPORT type 1 at offset X) and then draws the light's shadow map and the light with SET_RENDER_ENABLE
 * (2 << 24) | X; the RSX skips them when the count is 0 (395 draws per level frame in the RPCS3 captures).
 * Same shape, 4x4 FP16 surface, Z16 zeta, ZPASS counting on:
 *   init: depth 0.99 everywhere, colour black; occluder: full-screen blue at depth 0.2 (LESS, write);
 *   proxy A at 0.1 (in front: visible) -> report A; proxy B at 0.8 (behind: occluded) -> report B;
 *   report C with no draw since its clear (count 0);
 *   (command-list flush here: the resolves must work across lists)
 *   then, depth test off: TL red under report A, TR green under report B, BL yellow under report C,
 *   BR white with mode 1 (unconditional).
 * Want: TL red, TR blue, BL blue, BR white. Before 2025 (or with LD_COND_RENDER=0, the negative control) TR is green
 * and BL yellow. Usage: test_cond_render.exe (exit 0 + "[test_cond_render] PASS").
 * Patch 2027: the report context is main memory (SET_CONTEXT_DMA_REPORT 0xFEED0001, as KZ3 sets it) and the test
 * also reads the three CellGcmReportData values the title would read: A > 0 (16 samples), B 0, C 0. Before 2027
 * (or with LD_COND_RENDER=0 / LD_OQ_READBACK=0, the negative controls) A and B read 0xFFFF.
 * Built by tools/smoke/build-host.sh, run under Proton with tools/proton-test.sh (GPU lock).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_test_programs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void cellGcm_rsx_process_fifo(void);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);
extern int   rsx_live_draw_init(void* hwnd, u32 w, u32 h,
                                const uint8_t* (*guest_ptr)(void*, u32, u32, u32), void* user);
extern void  rsx_live_draw_flush(void);
extern int   rsx_live_draw_debug_read_surface(u32 location, u32 offset, void* out, u32 out_bytes,
                                              u32* out_w, u32* out_h);
extern int   rsx_null_backend_init(u32 w, u32 h, const char* title);
extern void* rsx_null_backend_get_hwnd(void);
extern void  rsx_null_backend_suppress_present(int on);
extern u32   cellGcmResolveLocated(int local, u32 offset);
extern u32   cellGcmResolveIO(u32 offset);
extern void  rsx_live_draw_debug_cond_render_stats(u64 out[6]);
extern void  rsx_live_draw_debug_report_readback_stats(u64 out[6]);

uint8_t* vm_base = NULL;

#define VM_TOTAL   0x100000000ull
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u
/* Main-memory data, IO offsets. */
#define VTX_OFFSET 0x00010000u   /* 8 quads x 4 vertices: float4 pos, float4 colour, float4 tc0; stride 48 */
#define FP_COL     0x00030000u   /* MOV r0, COL0 */
/* Local memory. */
#define SURF       0x00000000u   /* 4x4 FP16, pitch 128 */
#define ZETA       0x00010000u   /* Z16, pitch 64 */

static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1], base = (uintptr_t)vm_base;
    if (!vm_base || fault < base || fault >= base + VM_TOTAL) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void*)(fault & ~(uintptr_t)0xFFFF), 0x10000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static int vm_reserve(void)
{
    for (int a = 0; a < 8 && !vm_base; a++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)(VM_TOTAL * 2), MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uint8_t* al = (uint8_t*)(((uintptr_t)probe + (uintptr_t)VM_TOTAL - 1u) & ~((uintptr_t)VM_TOTAL - 1u));
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc(al, (SIZE_T)VM_TOTAL, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!vm_base) return -1;
    AddVectoredExceptionHandler(1, vm_commit_veh);
    return 0;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static __attribute__((unused)) void gw16(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
static void gf32(uint32_t ea, float f) { uint32_t w; memcpy(&w, &f, 4); gw32(ea, w); }

static uint32_t g_heap = HEAP_BASE;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap; g_heap += size; return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static uint32_t g_head;
static void emit_word(uint32_t v) { gw32(IO_ADDR + g_head, v); g_head += 4; }
static void emit(uint32_t method, uint32_t data)
{
    emit_word((1u << 18) | (((method >> 2) & 0x7FFu) << 2));
    emit_word(data);
}
static void emit_f32(u32 method, float f) { u32 w; memcpy(&w, &f, 4); emit(method, w); }

static const uint8_t* live_guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    u32 ea;
    if (location == 0) ea = cellGcmResolveLocated(1, offset);
    else { ea = cellGcmResolveIO(offset); if (!ea) ea = cellGcmResolveLocated(0, offset); }
    if (!vm_base || !ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return NULL;
    return vm_base + ea;
}


static int g_fail;

static float half_to_float(uint16_t h)
{
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (!e) return 0.0f;
    const uint32_t f = ((uint32_t)(h & 0x8000u) << 16) | ((e - 15u + 127u) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

/* quad q: rectangle [x0,x1]x[y0,y1] in NDC, window depth zw, colour rgb */
static void put_quad(u32 q, float x0, float y0, float x1, float y1, float zw, float r, float g, float b)
{
    const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y1, y1, y0, y0 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 2.0f * zw - 1.0f, 1,  r, g, b, 1,  0, 0, 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)((q * 4 + i) * 48 + k * 4), v[k]);
    }
}
static void draw_quad(u32 q)
{
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | (q * 4u));
    emit(NV4097_SET_BEGIN_END, 0u);
}
static void state(int dtest, u32 func, int dwrite, u32 cmask)
{
    emit(NV4097_SET_DEPTH_TEST_ENABLE, dtest ? 1u : 0u);
    emit(NV4097_SET_DEPTH_FUNC, func);
    emit(NV4097_SET_DEPTH_MASK, dwrite ? 1u : 0u);
    emit(0x0324u, cmask);                                                   /* SET_COLOR_MASK */
}
#define M_CLEAR_REPORT 0x17C8u
#define M_ZPASS_ENABLE 0x17CCu
#define M_GET_REPORT   0x1800u
#define M_RENDER_EN    0x1E98u
#define REP_A 0x40010u   /* IO offsets: main-memory reports (patch 2027) */
#define REP_B 0x40020u
#define REP_C 0x40030u

static void kick(u32 cdata, uint32_t ctrl_ea)
{
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
}

int main(void)
{
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_cond_render] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_cond_render") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_cond_render] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    put_quad(0, -1, -1, 1, 1, 0.99f, 0, 0, 0);       /* init */
    put_quad(1, -1, -1, 1, 1, 0.20f, 0, 0, 1);       /* occluder, blue */
    put_quad(2, -1, -1, 1, 1, 0.10f, 1, 0, 1);       /* proxy A (in front) */
    put_quad(3, -1, -1, 1, 1, 0.80f, 1, 0, 1);       /* proxy B (behind) */
    put_quad(4, -1,  0, 0, 1, 0.50f, 1, 0, 0);       /* TL red    under A */
    put_quad(5,  0,  0, 1, 1, 0.50f, 0, 1, 0);       /* TR green  under B */
    put_quad(6, -1, -1, 0, 0, 0.50f, 1, 1, 0);       /* BL yellow under C */
    put_quad(7,  0, -1, 1, 0, 0.50f, 1, 1, 1);       /* BR white  mode 1 */
    rsx_test_fp_mov_r0_col0(vm_base + IO_ADDR + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    g_head = 0;
    u8 vp[48];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    emit(NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0u);
    for (u32 k = 0; k < sizeof vp / 4u; k++) {
        const u8* w = vp + k * 4;
        emit(NV4097_SET_TRANSFORM_PROGRAM + k * 4,
             (u32)w[0] | ((u32)w[1] << 8) | ((u32)w[2] << 16) | ((u32)w[3] << 24));
    }
    emit(NV4097_SET_TRANSFORM_PROGRAM_START, 0u);
    for (u32 a = 0; a < 3; a++) {
        static const u32 attr[3] = { 0, 3, 8 };
        emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attr[a] * 4, 0x80000000u | (VTX_OFFSET + a * 16u));
        emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr[a] * 4, 2u | (4u << 4) | (48u << 8));
    }
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      0xBu | (0x1u << 5) | (0x1u << 8));   /* FP16 colour, Z16, linear */
    emit(NV4097_SET_SURFACE_PITCH_A,     128u);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, SURF);
    emit(NV4097_SET_SURFACE_PITCH_Z,     64u);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, ZETA);
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 4u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   4u << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, 4u << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   4u << 16);
    emit(NV4097_SET_SCISSOR_HORIZONTAL,  4u << 16);
    emit(NV4097_SET_SCISSOR_VERTICAL,    4u << 16);
    emit_f32(0x0A20, 2.0f); emit_f32(0x0A24, 2.0f);  emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);
    emit_f32(0x0A30, 2.0f); emit_f32(0x0A34, -2.0f); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f);
    emit(NV4097_SET_SHADER_CONTROL, CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS);
    emit(NV4097_SET_SHADER_PROGRAM, FP_COL | 2u);
    emit(0x01A8u, 0xFEED0001u);                                             /* SET_CONTEXT_DMA_REPORT: main */
    emit(M_RENDER_EN, 1u << 24);
    state(1, 0x207u, 1, 0x01010101u); draw_quad(0);                        /* init: ALWAYS + write */
    state(1, 0x201u, 1, 0x01010101u); draw_quad(1);                        /* occluder: LESS + write */
    emit(M_ZPASS_ENABLE, 1u);
    state(1, 0x201u, 0, 0u);
    emit(M_CLEAR_REPORT, 1u); draw_quad(2); emit(M_GET_REPORT, (1u << 24) | REP_A);
    emit(M_CLEAR_REPORT, 1u); draw_quad(3); emit(M_GET_REPORT, (1u << 24) | REP_B);
    emit(M_CLEAR_REPORT, 1u);               emit(M_GET_REPORT, (1u << 24) | REP_C);
    kick(cdata, ctrl_ea);
    rsx_live_draw_flush();                                                  /* resolve across command lists */
    state(0, 0x207u, 0, 0x01010101u);
    emit(M_RENDER_EN, (2u << 24) | REP_A); draw_quad(4);
    emit(M_RENDER_EN, (2u << 24) | REP_B); draw_quad(5);
    emit(M_RENDER_EN, (2u << 24) | REP_C); draw_quad(6);
    emit(M_RENDER_EN, 1u << 24);           draw_quad(7);
    kick(cdata, ctrl_ea);
    rsx_live_draw_flush();
    printf("[test_cond_render] ring 0x%X bytes, get=0x%X, LD_COND_RENDER=%s\n", g_head, vm_read32(ctrl_ea + 4),
           getenv("LD_COND_RENDER") ? getenv("LD_COND_RENDER") : "(unset)");
    static uint8_t buf[4 * 4 * 8];
    u32 w = 0, h = 0;
    const int texel = rsx_live_draw_debug_read_surface(0, SURF, buf, sizeof buf, &w, &h);
    if (texel != 8 || w != 4 || h != 4) {
        printf("[test_cond_render] FAIL surface: texel %d %ux%u\n", texel, w, h);
        return 1;
    }
    static const char* names[4] = { "TL", "TR", "BL", "BR" };
    static const float want[4][3] = { {1, 0, 0}, {0, 0, 1}, {0, 0, 1}, {1, 1, 1} };
    int bad_px = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            const u32 qd = (y >= 2 ? 2u : 0u) + (x >= 2 ? 1u : 0u);
            float c[3];
            for (u32 k = 0; k < 3; k++) { uint16_t v; memcpy(&v, buf + (y * 4 + x) * 8u + k * 2u, 2); c[k] = half_to_float(v); }
            const int ok = fabsf(c[0] - want[qd][0]) < 1e-3f && fabsf(c[1] - want[qd][1]) < 1e-3f &&
                           fabsf(c[2] - want[qd][2]) < 1e-3f;
            if (!ok) { bad_px++; g_fail++; }
            if (x % 2 == 0 && y % 2 == 0)
                printf("[test_cond_render] %s pixel (%u,%u) = (%.2f, %.2f, %.2f) want (%.0f, %.0f, %.0f) %s\n", names[qd],
                       x, y, c[0], c[1], c[2], want[qd][0], want[qd][1], want[qd][2], ok ? "ok" : "WRONG");
        }
    { /* patch 2027: the values the title reads (big-endian u32 at report + 8) */
      static const u32 rep[3] = { REP_A, REP_B, REP_C };
      static const char* rn[3] = { "A (visible)", "B (occluded)", "C (no draw)" };
      for (u32 k = 0; k < 3; k++) {
          const u32 v = vm_read32(IO_ADDR + rep[k] + 8u);
          const int ok = k == 0 ? (v > 0u && v <= 16u) : v == 0u;
          if (!ok) g_fail++;
          printf("[test_cond_render] report %s value %u (0x%X) want %s %s\n", rn[k], v, v, k == 0 ? "1..16" : "0",
                 ok ? "ok" : "WRONG");
      }
      u64 rb[6]; rsx_live_draw_debug_report_readback_stats(rb);
      printf("[test_cond_render] readback queued=%llu written=%llu zero=%llu dropped=%llu never=%llu walk-last=%llu\n",
             (unsigned long long)rb[0], (unsigned long long)rb[1], (unsigned long long)rb[2], (unsigned long long)rb[3],
             (unsigned long long)rb[4], (unsigned long long)rb[5]); }
    u64 st[6];
    rsx_live_draw_debug_cond_render_stats(st);
    printf("[test_cond_render] predicated=%llu skipped=%llu unconditional=%llu resolves=%llu reports=%llu segments=%llu\n",
           (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3],
           (unsigned long long)st[4], (unsigned long long)st[5]);
    printf("[test_cond_render] %d of 16 pixels wrong\n", bad_px);
    printf("[test_cond_render] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
