/* Patch 2016 host test: antialiased surfaces (SURFACE_FORMAT bits 12-15) at their sample resolution in live-draw.
 * Killzone 3 renders a 320x180 Z16 pass with SQUARE_ROTATED_4 (5) and samples its zeta as a 640x360 X16 texture
 * (RPCS3 capture, pass 18): on the RSX an AA surface's memory holds the sample grid. Two 4x4 draws of a 4x4
 * W16Z16Y16X16_FLOAT texture (point sampling) into FP16 surfaces:
 *   Q  SQUARE_ROTATED_4: the host surface must be 8x8 and texel (x, y) == texture (x/2, y/2), bit-exact;
 *   D  DIAGONAL_CENTERED_2 (3): 8x4, texel (x, y) == texture (x/2, y);
 *   N  CENTER_1 (0): 4x4, identity (no regression).
 * Before 2016 every case was 4x4 (AA ignored). Usage: test_surface_aa.exe (exit 0 + "[test_surface_aa] PASS").
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
#define VTX_OFFSET 0x00010000u   /* 4 vertices: float4 pos, float4 colour, float4 uv; stride 48 */
#define SRC1_OFFSET 0x00020000u  /* W16Z16Y16X16_F 4x4 swizzled (128 bytes), contents 1 */
#define SRC2_OFFSET 0x00020400u  /* the same texture, contents 2 */
#define MAINB_OFFSET 0x00040000u /* main-memory destination of the local->main copy */
#define MAINW_OFFSET 0x00041000u /* main copy of FP16 surface A (written back) */
#define MAINW2_OFFSET 0x00042000u /* main copy of RGBA8 surface D (written back) */
#define FP_OFFSET  0x00030000u
/* Local-memory surfaces, 4x4, pitch 64. */
#define SURF_A 0x00000000u
#define SURF_B 0x00001000u
#define SURF_C 0x00002000u
#define SURF_D 0x00003000u
#define SURF_E 0x00004000u
#define TEXL   0x00020000u       /* local copy of the texture (NV0039 destination) */
#define LBUF   0x00030000u       /* local source of the local->main copy */
#define LFLIP  0x00034000u       /* local destination of the negative-pitch copy */
#define ZETA   0x00010000u

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

/* Texel (x, y) channel c of texture A: exact halves (1 + m/8) * 2^e, some negative, up to 1920.0. */
static uint16_t half_a(u32 x, u32 y, u32 c)
{
    const u32 i = (y * 4 + x) * 4 + c;
    const u32 m = i % 8, e = (i % 15);           /* exponent -4 .. 10 */
    return (uint16_t)(((i % 7 == 3) ? 0x8000u : 0u) | ((e - 4 + 15) << 10) | (m << 7));
}
static __attribute__((unused)) uint16_t half_c(u32 x, u32 y, u32 c) { return (uint16_t)(half_a(3 - x, y, c) ^ 0x8000u); }
static __attribute__((unused)) float half_to_float(uint16_t h)   /* normal halves only */
{
    const uint32_t f = ((uint32_t)(h & 0x8000u) << 16) | ((((h >> 10) & 0x1Fu) - 15u + 127u) << 23) |
                       ((uint32_t)(h & 0x3FFu) << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static u32 swz4(u32 x, u32 y)   /* RSX swizzle for 4x4: x0 y0 x1 y1 */
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2);
}


static int g_fail, g_pass;

static void emit_case(u32 aa, u32 surf)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      0xBu | (0x1u << 5) | (0x1u << 8) | (aa << 12));
    emit(NV4097_SET_SURFACE_PITCH_A,     128u);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, surf);
    emit(NV4097_SET_SURFACE_PITCH_Z,     64u);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, ZETA + surf);
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 4u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   4u << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, 4u << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   4u << 16);
    emit(NV4097_SET_SCISSOR_HORIZONTAL,  4u << 16);
    emit(NV4097_SET_SCISSOR_VERTICAL,    4u << 16);
    emit_f32(0x0A20, 2.0f); emit_f32(0x0A24, 2.0f);  emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);
    emit_f32(0x0A30, 2.0f); emit_f32(0x0A34, -2.0f); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f);
    emit(NV4097_SET_TEXTURE_OFFSET,       SRC1_OFFSET);
    emit(NV4097_SET_TEXTURE_FORMAT,       2u | (2u << 4) | (0x9Au << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     0u);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
    emit(NV4097_SET_SHADER_PROGRAM, FP_OFFSET | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

int main(void)
{
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_surface_aa] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_surface_aa") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_surface_aa] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++)
                gw16(IO_ADDR + SRC1_OFFSET + swz4(x, y) * 8u + c * 2u, half_a(x, y, c));
    rsx_test_fp_tex_r0_tc0(vm_base + IO_ADDR + FP_OFFSET, 0);
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
    emit(NV4097_SET_SHADER_CONTROL, CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS);
    for (u32 a = 0; a < 3; a++) {
        static const u32 attr[3] = { 0, 3, 8 };
        emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attr[a] * 4, 0x80000000u | (VTX_OFFSET + a * 16u));
        emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr[a] * 4, 2u | (4u << 4) | (48u << 8));
    }
    static const u32 aa[3] = { 5, 3, 0 }, surf[3] = { SURF_A, SURF_B, SURF_C };
    static const u32 sx[3] = { 2, 2, 1 }, sy[3] = { 2, 1, 1 };
    static const char* tag[3] = { "Q", "D", "N" };
    for (u32 k = 0; k < 3; k++) emit_case(aa[k], surf[k]);
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
    rsx_live_draw_flush();
    printf("[test_surface_aa] ring 0x%X bytes, get=0x%X\n", g_head, vm_read32(ctrl_ea + 4));
    static uint8_t buf[8 * 8 * 8];
    for (u32 k = 0; k < 3; k++) {
        u32 w = 0, h = 0;
        const int texel = rsx_live_draw_debug_read_surface(0, surf[k], buf, sizeof buf, &w, &h);
        printf("[test_surface_aa] %s: aa %u -> host surface %ux%u (want %ux%u)\n", tag[k], aa[k], w, h, 4 * sx[k], 4 * sy[k]);
        if (texel != 8 || w != 4 * sx[k] || h != 4 * sy[k]) { g_fail++; continue; }
        int bad = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                for (u32 c = 0; c < 4; c++) {
                    uint16_t got; memcpy(&got, buf + (y * w + x) * 8u + c * 2u, 2);
                    const uint16_t want = half_a(x / sx[k], y / sy[k], c);
                    if (got == want) { g_pass++; continue; }
                    bad++;
                    if (g_fail++ < 16)
                        printf("[test_surface_aa] FAIL %s (%u,%u).%c got 0x%04X want 0x%04X\n", tag[k], x, y, "RGBA"[c], got, want);
                }
        printf("[test_surface_aa] %s: %d halves differ\n", tag[k], bad);
    }
    printf("[test_surface_aa] %d texel channels match, %d mismatch\n", g_pass, g_fail);
    printf("[test_surface_aa] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
