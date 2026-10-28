/* Patch 2022 host test: three ways a zeta read as a texture lost its depth in Killzone 3's level (pass 23).
 *   A. a 4x4 pass with depth write at Z16 zeta ZA: the zeta (and its R32F snapshot) is created with a 4x4 clip
 *      (KZ3: the 1280x720 scene zeta 0x25C0000 is first bound with a 512x512 clip);
 *   B. a 16x16 pass at ZA whose colour target is ZA itself (KZ3: 0x2980000 is the shadow map's colour target and
 *      zeta), depth test ALWAYS + write, depth export: depth = tc0.z, 0.1 at the left edge to 0.9 at the right;
 *   C. a 16x16 pass into FP16 surface E with ZA still bound as its zeta, depth test on and depth write off (KZ3:
 *      deferred lighting and soft particles), reading ZA as a 16x16 X16 texture with texel coordinates:
 *      E.r at column x must be 0.1 + 0.8 (x + 0.5) / 16.
 * Before 2022: C read guest VRAM (own zeta: 0), the colour surface at ZA (colour alias tried first), and a 4x4
 * snapshot (never grown). Negative controls, each must FAIL: LD_NO_ZSELF_SNAP=1, LD_NO_ZETA_PREFER=1,
 * LD_NO_ZSNAP_GROW=1. Built by tools/smoke/build-host.sh, run under Proton with tools/proton-test.sh (GPU lock).
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

uint8_t* vm_base = NULL;

#define VM_TOTAL   0x100000000ull
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u
/* Main-memory data, IO offsets. */
#define VTX_OFFSET 0x00010000u   /* 4 vertices: float4 pos, float4 colour, float4 tc0; stride 48 */
#define FP_DE      0x00030000u   /* MOV r1, TC0 ; MOV r0, COL0 (depth export) */
#define FP_TX      0x00030100u   /* TEX r0, TC0, unit 0 */
#define FP_COL     0x00030200u   /* MOV r0, COL0 */
/* Local memory. */
#define SURF_A     0x00000000u   /* 4x4 FP16, pitch 128 */
#define SURF_E     0x00003000u   /* 16x16 FP16 (X16 read result) */
#define ZA         0x00010000u   /* Z16, pitch 64, also pass B's colour target */

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


static int g_fail, g_pass;

static float half_to_float(uint16_t h)
{
    const uint32_t e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    if (!e) return 0.0f;
    const uint32_t f = ((uint32_t)(h & 0x8000u) << 16) | ((e - 15u + 127u) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

static void pass(u32 color, u32 zeta, u32 w, u32 h, u32 fp, u32 ctrl, int dtest, int dwrite)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      0xBu | (0x1u << 5) | (0x1u << 8));   /* FP16 colour, Z16, linear */
    emit(NV4097_SET_SURFACE_PITCH_A,     128u);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, color);
    emit(NV4097_SET_SURFACE_PITCH_Z,     64u);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, zeta);
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, w << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   h << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, w << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   h << 16);
    emit(NV4097_SET_SCISSOR_HORIZONTAL,  w << 16);
    emit(NV4097_SET_SCISSOR_VERTICAL,    h << 16);
    emit_f32(0x0A20, w * 0.5f); emit_f32(0x0A24, h * 0.5f);  emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);
    emit_f32(0x0A30, w * 0.5f); emit_f32(0x0A34, h * -0.5f); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f);
    emit(NV4097_SET_DEPTH_TEST_ENABLE, dtest ? 1u : 0u);
    emit(NV4097_SET_DEPTH_FUNC, 0x207u);                                    /* ALWAYS */
    emit(NV4097_SET_DEPTH_MASK, dwrite ? 1u : 0u);
    emit(NV4097_SET_SHADER_CONTROL, ctrl);
    emit(NV4097_SET_SHADER_PROGRAM, fp | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_zeta_reads] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_zeta_reads") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_zeta_reads] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 16, 16, 0 }, vs[4] = { 0, 0, 16, 16 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], xs[i] < 0 ? 0.1f : 0.9f, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    uint8_t* fpm = vm_base + IO_ADDR;
    rsx_test_fp_instr(fpm + FP_DE,
                      RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_OUTREG(1),
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_mov_r0_col0(fpm + FP_DE + 16, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(fpm + FP_TX, 0);
    rsx_test_fp_mov_r0_col0(fpm + FP_COL, RSX_TEST_FP_SWZ_IDENT);
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
    const u32 X32 = CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    pass(SURF_A, ZA, 4, 4, FP_COL, X32, 1, 1);                 /* A: zeta created with a 4x4 clip */
    pass(ZA, ZA, 16, 16, FP_DE, X32 | 0xEu, 1, 1);             /* B: colour = zeta address, depth = tc0.z */
    emit(NV4097_SET_TEXTURE_OFFSET,       ZA);
    emit(NV4097_SET_TEXTURE_FORMAT,       1u | (2u << 4) | (0xF4u << 8) | (1u << 16));   /* local, 2D, X16 LN UN */
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     64u);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (16u << 16) | 16u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
    pass(SURF_E, ZA, 16, 16, FP_TX, X32, 1, 0);                /* C: own zeta, test on, write off */
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
    rsx_live_draw_flush();
    printf("[test_zeta_reads] ring 0x%X bytes, get=0x%X\n", g_head, vm_read32(ctrl_ea + 4));
    static uint8_t buf[64 * 64 * 8];
    u32 w = 0, h = 0;
    const int texel = rsx_live_draw_debug_read_surface(0, SURF_E, buf, sizeof buf, &w, &h);
    if (texel != 8 || w < 16 || h < 16) {
        printf("[test_zeta_reads] FAIL surface E: texel %d %ux%u\n", texel, w, h);
        return 1;
    }
    for (u32 y = 0; y < 16; y++) {
        if (!(y % 5)) printf("[test_zeta_reads] row %2u:", y);
        for (u32 x = 0; x < 16; x++) {
            uint16_t r; memcpy(&r, buf + (y * w + x) * 8u, 2);
            const float got = half_to_float(r), want = 0.1f + 0.8f * (x + 0.5f) / 16.0f;
            if (!(y % 5) && !(x % 3)) printf(" %.4f", got);
            if (fabsf(got - want) <= 2e-3f) g_pass++; else g_fail++;
        }
        if (!(y % 5)) printf("\n");
    }
    printf("[test_zeta_reads] %d of 256 depths match (want 0.1 + 0.8 (x + 0.5) / 16 per column)\n", g_pass);
    printf("[test_zeta_reads] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
