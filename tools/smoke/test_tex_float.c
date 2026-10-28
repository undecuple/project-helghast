/* Patch 2014 host test: float textures and the F_X32 colour surface through ps3recomp's live-draw engine.
 *
 * Builds a command ring on a real 4 GB guest reservation (as host_kz3.c --live), runs it through
 * cellGcm_rsx_process_fifo into rsx_live_draw, then reads the host surfaces back raw with
 * rsx_live_draw_debug_read_surface (added by 2014) and compares texel by texel:
 *   A  W16Z16Y16X16_FLOAT 4x4 swizzled, remap 0xAAE4  -> F_W16Z16Y16X16 surface: every half bit-exact
 *      (values up to 1920.0 and negatives; the old RGBA8 decode clamped them to [0,1]);
 *   B  the same texture, remap 0xA96C (R<-B, G<-G, B<-R, A<-ONE) -> FP16 surface: (Z, Y, X, 1.0) bit-exact;
 *   C  Y16_X16_FLOAT 4x4 linear, pitch 32, remap 0x29E4 (KZ3's) -> FP16 surface: (X, Y, 0, 1.0) bit-exact
 *      (unsupported before 2014: the unit read white);
 *   D  texture A -> F_X32 surface (R32_FLOAT since 2014; RGBA8 before): R32 == float(X) exactly.
 * One fragment program, TEX r0, TC0, unit 0 with 32-bit exports, point sampling, one 4x4 quad per case.
 * Usage: test_tex_float.exe (exit code 0 and "[test_tex_float] PASS" = all texels match). Built by
 * tools/smoke/build-host.sh, run under Proton with tools/proton-test.sh (GPU lock).
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
#define TEXA_OFFSET 0x00020000u  /* W16Z16Y16X16_F 4x4 swizzled (128 bytes) */
#define TEXC_OFFSET 0x00021000u  /* Y16_X16_F 4x4 linear, pitch 32 */
#define FP_OFFSET  0x00030000u
/* Local-memory surfaces, 4x4, pitch 64. */
#define SURF_A 0x00000000u
#define SURF_B 0x00001000u
#define SURF_C 0x00002000u
#define SURF_D 0x00003000u
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
static uint16_t half_c(u32 x, u32 y, u32 c) { return (uint16_t)(half_a(3 - x, y, c) ^ 0x8000u); }
static float half_to_float(uint16_t h)   /* normal halves only */
{
    const uint32_t f = ((uint32_t)(h & 0x8000u) << 16) | ((((h >> 10) & 0x1Fu) - 15u + 127u) << 23) |
                       ((uint32_t)(h & 0x3FFu) << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static u32 swz4(u32 x, u32 y)   /* RSX swizzle for 4x4: x0 y0 x1 y1 */
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2);
}

static void emit_case(u32 color_fmt, u32 surf, u32 tex_off, u32 tex_fmt, u32 pitch, u32 remap)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      color_fmt | (0x1u << 5) | (0x1u << 8));   /* Z16, pitch */
    emit(NV4097_SET_SURFACE_PITCH_A,     64u);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, surf);
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

    emit(NV4097_SET_TEXTURE_OFFSET,       tex_off);
    emit(NV4097_SET_TEXTURE_FORMAT,       2u | (2u << 4) | (tex_fmt << 8) | (1u << 16));   /* main, 2D, 1 mip */
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));                    /* clamp to edge */
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     remap);
    emit(NV4097_SET_TEXTURE_CONTROL3,     pitch);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));            /* nearest */
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);

    emit(NV4097_SET_SHADER_PROGRAM, FP_OFFSET | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

static int g_fail, g_pass;
static void check_u16(const char* tag, u32 x, u32 y, u32 c, uint16_t got, uint16_t want)
{
    if (got == want) { g_pass++; return; }
    if (g_fail++ < 24)
        printf("[test_tex_float] FAIL %s (%u,%u).%c got 0x%04X want 0x%04X\n", tag, x, y, "RGBA"[c], got, want);
}

int main(void)
{
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_tex_float] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_tex_float") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_tex_float] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* Quad over the 4x4 target, uv 0..1 (v down), QUADS order TL, TR, BR, BL. */
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++) {
                gw16(IO_ADDR + TEXA_OFFSET + swz4(x, y) * 8u + c * 2u, half_a(x, y, c));
                if (c < 2) gw16(IO_ADDR + TEXC_OFFSET + y * 32u + x * 4u + c * 2u, half_c(x, y, c));
            }
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
    emit_case(0xB, SURF_A, TEXA_OFFSET, 0x9A, 0, 0xAAE4);
    emit_case(0xB, SURF_B, TEXA_OFFSET, 0x9A, 0, 0xA96C);
    emit_case(0xB, SURF_C, TEXC_OFFSET, 0x9F | 0x20, 32, 0x29E4);
    emit_case(0xD, SURF_D, TEXA_OFFSET, 0x9A, 0, 0xAAE4);
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
    rsx_live_draw_flush();
    printf("[test_tex_float] ring 0x%X bytes, get=0x%X\n", g_head, vm_read32(ctrl_ea + 4));

    static uint8_t buf[4 * 4 * 16];
    static const u32 surf[4] = { SURF_A, SURF_B, SURF_C, SURF_D };
    for (u32 k = 0; k < 4; k++) {
        u32 w = 0, h = 0;
        const char tag[2] = { (char)('A' + k), 0 };
        const int texel = rsx_live_draw_debug_read_surface(0, surf[k], buf, sizeof buf, &w, &h);
        const int want_texel = k == 3 ? 4 : 8;
        if (texel != want_texel || w != 4 || h != 4) {
            printf("[test_tex_float] FAIL %s: surface read texel=%d %ux%u (want %d 4x4)\n", tag, texel, w, h, want_texel);
            g_fail++; continue;
        }
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                const uint8_t* p = buf + (y * 4 + x) * (u32)texel;
                if (k == 3) {
                    float got; memcpy(&got, p, 4);
                    const float want = half_to_float(half_a(x, y, 0));
                    if (got == want) g_pass++;
                    else if (g_fail++ < 24)
                        printf("[test_tex_float] FAIL D (%u,%u).R got %g want %g\n", x, y, got, want);
                    continue;
                }
                for (u32 c = 0; c < 4; c++) {
                    uint16_t got; memcpy(&got, p + c * 2, 2);   /* host little-endian half */
                    uint16_t want;
                    if (k == 0) want = half_a(x, y, c);
                    else if (k == 1) want = c == 0 ? half_a(x, y, 2) : c == 1 ? half_a(x, y, 1)
                                          : c == 2 ? half_a(x, y, 0) : 0x3C00u;
                    else want = c < 2 ? half_c(x, y, c) : c == 2 ? 0x0000u : 0x3C00u;
                    check_u16(tag, x, y, c, got, want);
                }
            }
        if (k == 0) {
            uint16_t h0; memcpy(&h0, buf, 2);
            printf("[test_tex_float] A texel (0,0).R = 0x%04X (%g)\n", h0, half_to_float(h0));
        }
    }
    printf("[test_tex_float] %d texel channels match, %d mismatch\n", g_pass, g_fail);
    printf("[test_tex_float] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
