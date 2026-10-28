/* Patch 2015 host test: the NV0039 buffer copy (cellGcmSetTransferData, subchannel 1) through ps3recomp's
 * FIFO walker and live-draw engine, on a real 4 GB guest reservation (as test_tex_float).
 *   A  NV0039 main->local copy of a 4x4 W16Z16Y16X16_FLOAT texture (4 lines of 32 bytes), then a draw sampling
 *      the LOCAL copy into an FP16 surface: every half bit-exact (before 2015 the copy never ran and its words
 *      reached NV4097 as ALPHA_REF..STENCIL_TEST_ENABLE).
 *   B  same frame: a second copy with different contents over the same local texture, then the same draw into
 *      another surface: bit-exact to the NEW contents (live-draw hashes textures once per frame; 2015 marks the
 *      overlapping cache entry so the second bind re-hashes).
 *   C  a draw sampling the main-memory source directly after all copies: bit-exact (no 3D state corrupted by
 *      the subchannel-1 words: COLOR_MASK would read FORMAT 0x101).
 *   M  guest memory: local->main copy, 5 lines of 40 bytes, pitch in 64 / out 48 (gap bytes untouched), and a
 *      local->local copy with pitch out -16 (vertical flip).
 * Usage: test_nv0039.exe (exit code 0 and "[test_nv0039] PASS"). Built by tools/smoke/build-host.sh, run under
 * Proton with tools/proton-test.sh (GPU lock).
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
static uint16_t half_c(u32 x, u32 y, u32 c) { return (uint16_t)(half_a(3 - x, y, c) ^ 0x8000u); }
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


static void emit_sub(u32 subch, u32 method, const u32* w, u32 n)
{
    emit_word((n << 18) | (subch << 13) | (method & 0x1FFCu));
    for (u32 i = 0; i < n; i++) emit_word(w[i]);
}
/* cellGcmSetTransferData's shape (KZ3 0x008D2DAC): 0x2184 x2 then 0x230C x8. */
static void emit_copy(u32 in_dma, u32 out_dma, u32 off_in, u32 off_out, s32 pin, s32 pout, u32 len, u32 n)
{
    const u32 d[2] = { in_dma, out_dma };
    emit_sub(1, 0x0184, d, 2);
    const u32 w[8] = { off_in, off_out, (u32)pin, (u32)pout, len, n, 0x101u, 0u };
    emit_sub(1, 0x030C, w, 8);
}

static void emit_case(u32 color_fmt, u32 surf, u32 tex_loc, u32 tex_off, u32 tex_fmt, u32 pitch)
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
    emit(NV4097_SET_TEXTURE_FORMAT,       tex_loc | (2u << 4) | (tex_fmt << 8) | (1u << 16));   /* 2D, 1 mip */
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     pitch);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);

    emit(NV4097_SET_SHADER_PROGRAM, FP_OFFSET | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

static int g_fail, g_pass;
static void check_u8(const char* tag, u32 i, uint8_t got, uint8_t want)
{
    if (got == want) { g_pass++; return; }
    if (g_fail++ < 24) printf("[test_nv0039] FAIL %s byte %u got 0x%02X want 0x%02X\n", tag, i, got, want);
}
static uint16_t tex_half(int which, u32 x, u32 y, u32 c) { return which == 1 ? half_a(x, y, c) : half_c(x, y, c); }

int main(void)
{
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_nv0039] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_nv0039") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_nv0039] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    const u32 local = cellGcmResolveLocated(1, 0);

    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++) {
                gw16(IO_ADDR + SRC1_OFFSET + swz4(x, y) * 8u + c * 2u, tex_half(1, x, y, c));
                gw16(IO_ADDR + SRC2_OFFSET + swz4(x, y) * 8u + c * 2u, tex_half(2, x, y, c));
            }
    for (u32 i = 0; i < 64 * 8; i++) vm_base[local + LBUF + i] = (uint8_t)(i * 7u + 3u);
    memset(vm_base + IO_ADDR + MAINB_OFFSET, 0xCD, 64 * 8);
    memset(vm_base + local + LFLIP, 0xCD, 64);
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
    /* A: main -> local, 4 lines of 32 bytes (a swizzled 4x4 FP16 texture is 128 contiguous bytes). */
    emit_copy(0xFEED0001u, 0xFEED0000u, SRC1_OFFSET, TEXL, 32, 32, 32, 4);
    emit_case(0xB, SURF_A, 1u, TEXL, 0x9A, 0);
    /* B: same frame, new contents over the same local texture, as one 128-byte line. */
    emit_copy(0xFEED0001u, 0xFEED0000u, SRC2_OFFSET, TEXL, 0, 0, 128, 1);
    emit_case(0xB, SURF_B, 1u, TEXL, 0x9A, 0);
    /* M: local -> main with pitches, and local -> local flipped. */
    emit_copy(0xFEED0000u, 0xFEED0001u, LBUF, MAINB_OFFSET, 64, 48, 40, 5);
    emit_copy(0xFEED0000u, 0xFEED0000u, LBUF, LFLIP + 32, 16, -16, 16, 3);
    /* C: the main source directly, after all subchannel-1 traffic. */
    emit_case(0xB, SURF_C, 2u, SRC1_OFFSET, 0x9A, 0);
    /* W: copies OUT of live surfaces (written back from the GPU first). FP16 surface A -> main, 4 rows of 32
     * bytes, pitch 64 -> 32; RGBA8 surface D (texture 1 clamped) -> main, pitch 64 -> 16, then sampled from
     * main as A8R8G8B8 into RGBA8 surface E: E must equal D byte for byte (layout round trip). */
    emit_copy(0xFEED0000u, 0xFEED0001u, SURF_A, MAINW_OFFSET, 64, 32, 32, 4);
    emit_case(0x8, SURF_D, 2u, SRC1_OFFSET, 0x9A, 0);
    emit_copy(0xFEED0000u, 0xFEED0001u, SURF_D, MAINW2_OFFSET, 64, 16, 16, 4);
    emit_case(0x8, SURF_E, 2u, MAINW2_OFFSET, 0x85 | 0x20, 16);
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
    rsx_live_draw_flush();
    printf("[test_nv0039] ring 0x%X bytes, get=0x%X\n", g_head, vm_read32(ctrl_ea + 4));

    /* Guest memory first (independent of the renderer). */
    for (u32 i = 0; i < 128; i++)
        check_u8("A-guest", i, vm_base[local + TEXL + i], vm_base[IO_ADDR + SRC2_OFFSET + i]);
    for (u32 y = 0; y < 6; y++)
        for (u32 x = 0; x < 48; x++) {
            const uint8_t want = (y < 5 && x < 40) ? (uint8_t)((y * 64u + x) * 7u + 3u) : 0xCD;
            check_u8("M-main", y * 48 + x, vm_base[IO_ADDR + MAINB_OFFSET + y * 48 + x], want);
        }
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 16; x++) {
            const uint8_t want = y < 3 ? (uint8_t)(((2u - y) * 16u + x) * 7u + 3u) : 0xCD;
            check_u8("M-flip", y * 16 + x, vm_base[local + LFLIP + y * 16 + x], want);
        }

    static uint8_t buf[4 * 4 * 16];
    static const u32 surf[3] = { SURF_A, SURF_B, SURF_C };
    static const int which[3] = { 1, 2, 1 };
    for (u32 k = 0; k < 3; k++) {
        u32 w = 0, h = 0;
        const char tag[2] = { (char)('A' + k), 0 };
        const int texel = rsx_live_draw_debug_read_surface(0, surf[k], buf, sizeof buf, &w, &h);
        if (texel != 8 || w != 4 || h != 4) {
            printf("[test_nv0039] FAIL %s: surface read texel=%d %ux%u (want 8 4x4)\n", tag, texel, w, h);
            g_fail++; continue;
        }
        int bad = 0;
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++)
                for (u32 c = 0; c < 4; c++) {
                    uint16_t got; memcpy(&got, buf + (y * 4 + x) * 8u + c * 2u, 2);
                    const uint16_t want = tex_half(which[k], x, y, c);
                    if (got == want) { g_pass++; continue; }
                    bad++;
                    if (g_fail++ < 24)
                        printf("[test_nv0039] FAIL %s (%u,%u).%c got 0x%04X want 0x%04X (other contents 0x%04X)\n",
                               tag, x, y, "RGBA"[c], got, want, tex_half(3 - which[k], x, y, c));
                }
        printf("[test_nv0039] surface %s: %d/64 halves differ\n", tag, bad);
    }
    /* W: FP16 surface A in main memory as big-endian halves, X first. */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++) {
                const uint8_t* p = vm_base + IO_ADDR + MAINW_OFFSET + y * 32u + x * 8u + c * 2u;
                const uint16_t got = (uint16_t)((p[0] << 8) | p[1]), want = tex_half(1, x, y, c);
                check_u8("W-fp16-hi", (y * 4 + x) * 4 + c, (uint8_t)(got >> 8), (uint8_t)(want >> 8));
                check_u8("W-fp16-lo", (y * 4 + x) * 4 + c, (uint8_t)got, (uint8_t)want);
            }
    {
        static uint8_t d[64], e[64];
        u32 w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        const int t1 = rsx_live_draw_debug_read_surface(0, SURF_D, d, sizeof d, &w1, &h1);
        const int t2 = rsx_live_draw_debug_read_surface(0, SURF_E, e, sizeof e, &w2, &h2);
        if (t1 != 4 || t2 != 4 || w1 != 4 || w2 != 4 || h1 != 4 || h2 != 4) {
            printf("[test_nv0039] FAIL W-rgba8: surface reads %d/%d\n", t1, t2); g_fail++;
        } else {
            for (u32 i = 0; i < 64; i++) check_u8("W-rgba8", i, e[i], d[i]);
            for (u32 i = 0; i < 4; i++)   /* and the guest bytes are A,R,G,B */
                check_u8("W-argb", i, vm_base[IO_ADDR + MAINW2_OFFSET + i], d[(i + 3) & 3]);
            printf("[test_nv0039] D texel 0 RGBA = %02X %02X %02X %02X\n", d[0], d[1], d[2], d[3]);
        }
    }
    printf("[test_nv0039] %d checks match, %d mismatch\n", g_pass, g_fail);
    printf("[test_nv0039] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
