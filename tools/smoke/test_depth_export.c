/* Patch 2021 host test: fragment depth export and the texel size of a zeta-snapshot bind.
 * Killzone 3's half-resolution depth downsample writes the fragment depth (r1.z, SHADER_CONTROL depth-export bits
 * 0xE) into a 640x360 Z16 zeta that shares its offset with the 1408x1408 shadow map; the DoF/soft-particle passes
 * read it back as X16 with texel coordinates (pass 22). Same shape, small:
 *   1. a 16x16 pass with depth write at Z16 zeta ZETA (makes the host depth resource 16x16; depth 0.5);
 *   2. a 4x4 pass at the same zeta, depth test ALWAYS + write, program MOV r1, TC0 ; MOV r0, COL0 with depth export:
 *      fragment depth = tc0.z, 0.1 at the left edge to 0.9 at the right, so pixel column x gets 0.2/0.4/0.6/0.8;
 *   3. a 4x4 pass into FP16 surface E reading ZETA as a 4x4 X16 texture (format 0xF4: linear, unnormalised) with
 *      texel coordinates 0..4: E.r must be column x's depth.
 * Before 2021 step 2 wrote the rasterised depth 0.5 (no SV_Depth) and step 3 divided the texel coordinates by 4x4
 * instead of 16x16. Negative controls: --no-export (step 2 without the 0xE bits) and LD_NO_ZALIAS_UNNORM_FIX=1
 * must FAIL. Built by tools/smoke/build-host.sh, run under Proton with tools/proton-test.sh (GPU lock).
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
#define SURF_BIG   0x00000000u   /* 16x16 FP16, pitch 128 */
#define SURF_S     0x00002000u   /* 4x4 FP16 (depth-export pass colour) */
#define SURF_E     0x00003000u   /* 4x4 FP16 (X16 read result) */
#define ZETA       0x00010000u   /* Z16, pitch 64: the shared zeta (KZ3 0x2980000) */
#define ZETA2      0x00011000u   /* zeta of the read pass (depth test off) */

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
static void gw16(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
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

static void pass(u32 color, u32 zeta, u32 w, u32 h, u32 fp, u32 ctrl, int dtest)
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
    emit(NV4097_SET_DEPTH_MASK, dtest ? 1u : 0u);
    emit(NV4097_SET_SHADER_CONTROL, ctrl);
    emit(NV4097_SET_SHADER_PROGRAM, fp | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

int main(int argc, char** argv)
{
    const int no_export = argc > 1 && !strcmp(argv[1], "--no-export");
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_depth_export] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_depth_export") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_depth_export] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 4, 4, 0 },   vs[4] = { 0, 0, 4, 4 };
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
    pass(SURF_BIG, ZETA, 16, 16, FP_COL, X32, 1);                        /* 1: 16x16, depth 0.5 */
    pass(SURF_S, ZETA, 4, 4, FP_DE, X32 | (no_export ? 0u : 0xEu), 1);   /* 2: depth = tc0.z */
    emit(NV4097_SET_TEXTURE_OFFSET,       ZETA);
    emit(NV4097_SET_TEXTURE_FORMAT,       1u | (2u << 4) | (0xF4u << 8) | (1u << 16));   /* local, 2D, X16 LN UN */
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     64u);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
    pass(SURF_E, ZETA2, 4, 4, FP_TX, X32, 0);                           /* 3: read ZETA as X16 */
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
    rsx_live_draw_flush();
    printf("[test_depth_export] ring 0x%X bytes, get=0x%X, %s\n", g_head, vm_read32(ctrl_ea + 4),
           no_export ? "no depth export (negative control)" : "depth export on");
    static uint8_t buf[16 * 16 * 8];
    u32 w = 0, h = 0;
    const int texel = rsx_live_draw_debug_read_surface(0, SURF_E, buf, sizeof buf, &w, &h);
    if (texel != 8 || w != 4 || h != 4) {
        printf("[test_depth_export] FAIL surface E: texel %d %ux%u\n", texel, w, h);
        return 1;
    }
    for (u32 y = 0; y < 4; y++) {
        printf("[test_depth_export] row %u:", y);
        for (u32 x = 0; x < 4; x++) {
            uint16_t r; memcpy(&r, buf + (y * 4 + x) * 8u, 2);
            const float got = half_to_float(r), want = 0.1f + 0.8f * (x + 0.5f) / 4.0f;
            printf(" %.4f", got);
            if (fabsf(got - want) <= 2e-3f) g_pass++; else g_fail++;
        }
        printf("\n");
    }
    printf("[test_depth_export] %d of 16 depths match (want 0.2/0.4/0.6/0.8 per column)\n", g_pass);
    printf("[test_depth_export] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
