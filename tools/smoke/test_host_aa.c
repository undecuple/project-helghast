/* Patch 2023 host test: the host anti-aliasing pass (FXAA, ENHANCEMENT) on the scene surface right before an
 * NV0039 copy of the whole surface writes it back (Killzone 3: the 1280x720 scene leaving for the SPU post chain).
 * Harness as test_wb_batch (ps3recomp FIFO walker + live-draw on a real 4 GB guest reservation). Per frame: a
 * 1280x720 local A8R8G8B8 scene = a rotated, wrapped 2-colour checker (nearest filtering: hard diagonal edges,
 * flat cell interiors, alpha 0x40/0xC0 per colour), read back from the GPU before the copy (pre), then the NV0039
 * copy into main memory (post). Checks on post vs pre: every flat pixel (3x3 neighbourhood equal in pre) is
 * bit-identical, alpha is unchanged everywhere, and at least 25 % of the edge pixels changed.
 * Usage: test_host_aa.exe [--arm] (default: KZ3_HOST_AA from the environment; --arm calls
 * rsx_live_draw_host_aa_arm(1) as the port does after skipping SPU MLAA). Expected: KZ3_HOST_AA=fxaa PASS,
 * unset + --arm PASS; controls KZ3_HOST_AA=off FAIL, unset without --arm FAIL (no pass without the MLAA skip).
 * Exit 0 and "[test_host_aa] PASS". Built by tools/smoke/build-host.sh, run under Proton (GPU lock).
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
extern void  rsx_live_draw_present(u32 buffer_id);
extern u32   rsx_live_draw_debug_host_aa_passes(void);
extern void  rsx_live_draw_host_aa_arm(int on);

uint8_t* vm_base = NULL;

#define VM_TOTAL   0x100000000ull
#define IO_ADDR    0x01000000u
#define IO_SIZE    0x01000000u
#define CMD_SIZE   0x00080000u   /* 512 KB: about 1.5 KB per frame, no wrap */
#define HEAP_BASE  0x00400000u
/* Main-memory data, IO offsets. */
#define VTX_OFFSET 0x00080000u   /* 4 vertices: float4 pos, float4 colour, float4 uv; stride 48 */
#define FP_OFFSET  0x00090000u
/* Local zeta. */
#define ZETA   0x01000000u   /* local, away from the scene */

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


/* Draw a full-surface quad sampling the 4x4 texture TEXM1 into colour surface (dma, off) of w x h. */
#define TEXM1   0x000A0000u          /* IO: 4x4 A8R8G8B8 pitch 16 */
static void emit_draw(u32 dma, u32 color_fmt, u32 surf, u32 pitch, u32 w, u32 h)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, dma);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      color_fmt | (0x1u << 5) | (0x1u << 8));   /* Z16, pitch */
    emit(NV4097_SET_SURFACE_PITCH_A,     pitch);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, surf);
    emit(NV4097_SET_SURFACE_PITCH_Z,     w * 2u < 64u ? 64u : w * 2u);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, ZETA);
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, w << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   h << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, w << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   h << 16);
    emit(NV4097_SET_SCISSOR_HORIZONTAL,  w << 16);
    emit(NV4097_SET_SCISSOR_VERTICAL,    h << 16);
    const float hw = (float)w * 0.5f, hh = (float)h * 0.5f;
    emit_f32(0x0A20, hw); emit_f32(0x0A24, hh);  emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);
    emit_f32(0x0A30, hw); emit_f32(0x0A34, -hh); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f);
    emit(NV4097_SET_TEXTURE_OFFSET,       TEXM1);
    emit(NV4097_SET_TEXTURE_FORMAT,       2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_ADDRESS,      1u | (1u << 8) | (1u << 16));   /* wrap */
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     16u);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
    emit(NV4097_SET_SHADER_PROGRAM, FP_OFFSET | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

static u32 g_ctrl_ea, g_cdata;
static void kick(void)
{
    gw32(g_cdata + 0x8, IO_ADDR + g_head);
    vm_write32(g_ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
}
static int g_fail;
#define SCENE  0x00100000u   /* local 1280x720 A8R8G8B8, pitch 5120 */
#define CPY    0x00500000u   /* IO: NV0039 destination, 1280x720 pitch 5120 */
#define SW 1280u
#define SH 720u

int main(int argc, char** argv)
{
    const int arm = argc > 1 && !strcmp(argv[1], "--arm");
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_host_aa] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_host_aa") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_host_aa] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    g_cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    /* UV = rotation by 0.35 rad of (1.6x, 0.9y) + 0.13: a wrapped 4x4 checker of 100-pixel cells with
     * diagonal edges (stair steps for the AA pass) and flat interiors. */
    const float ca = 0.9394f, sa = 0.3429f;
    for (int i = 0; i < 4; i++) {
        const float px = xs[i] * 1.6f, py = ys[i] * 0.9f;
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  ca * px - sa * py + 0.13f, sa * px + ca * py + 0.13f, 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            uint8_t* p = vm_base + IO_ADDR + TEXM1 + y * 16u + x * 4u;
            static const uint8_t dark[4] = { 0x40, 30, 40, 50 }, bright[4] = { 0xC0, 230, 210, 190 };   /* A,R,G,B */
            memcpy(p, ((x ^ y) & 1u) ? bright : dark, 4);
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
    kick();

    if (arm) rsx_live_draw_host_aa_arm(1);
    static uint8_t pre[SW * SH * 4];
    for (int f = 0; f < 3; f++) {
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER, 0x8, SCENE, 5120, SW, SH);
        kick();
        u32 w = 0, h = 0;
        if (rsx_live_draw_debug_read_surface(0, SCENE, pre, sizeof pre, &w, &h) != 4 || w != SW || h != SH) {
            printf("[test_host_aa] FAIL frame %d: scene readback failed (%ux%u)\n", f, w, h); g_fail++; break;
        }
        memset(vm_base + IO_ADDR + CPY, 0, 5120u * SH);
        emit_copy(0xFEED0000u, 0xFEED0001u, SCENE, CPY, 5120, 5120, 5120, SH);
        kick();
        u32 flat = 0, flat_chg = 0, edge = 0, edge_chg = 0, alpha_chg = 0, maxd = 0;
        for (u32 y = 0; y < SH; y++)
            for (u32 x = 0; x < SW; x++) {
                const uint8_t* a = pre + ((size_t)y * SW + x) * 4u;                    /* host R,G,B,A */
                const uint8_t* b = vm_base + IO_ADDR + CPY + (size_t)y * 5120u + x * 4u;  /* guest A,R,G,B */
                int is_flat = 1;
                for (int dy = -1; dy <= 1 && is_flat; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = (int)x + dx, yy = (int)y + dy;
                        if (xx < 0 || yy < 0 || xx >= (int)SW || yy >= (int)SH) continue;
                        if (memcmp(pre + ((size_t)yy * SW + (u32)xx) * 4u, a, 4)) { is_flat = 0; break; }
                    }
                const int chg = b[1] != a[0] || b[2] != a[1] || b[3] != a[2];
                for (int c = 0; c < 3; c++) { const u32 d = (u32)abs((int)b[1 + c] - (int)a[c]); if (d > maxd) maxd = d; }
                if (b[0] != a[3]) alpha_chg++;
                if (is_flat) { flat++; flat_chg += (u32)chg; } else { edge++; edge_chg += (u32)chg; }
            }
        const int ok = flat > 0 && edge > 0 && flat_chg == 0 && alpha_chg == 0 && edge_chg * 4u >= edge;
        printf("[test_host_aa] frame %d: flat %u (changed %u), edge %u (changed %u = %.1f %%), alpha changed %u, "
               "max channel delta %u, passes %u: %s\n", f, flat, flat_chg, edge, edge_chg,
               edge ? 100.0 * edge_chg / edge : 0.0, alpha_chg, maxd, rsx_live_draw_debug_host_aa_passes(),
               ok ? "ok" : "FAIL");
        if (!ok) g_fail++;
        if (f == 0) {   /* a 48x24 crop around an edge, before and after, for the notes */
            FILE* fp = fopen("test_host_aa_crop.ppm", "wb");
            if (fp) {
                fprintf(fp, "P6 96 24 255\n");
                for (u32 y = 300; y < 324; y++) {
                    for (u32 x = 600; x < 648; x++) fwrite(pre + ((size_t)y * SW + x) * 4u, 1, 3, fp);
                    for (u32 x = 600; x < 648; x++) fwrite(vm_base + IO_ADDR + CPY + (size_t)y * 5120u + x * 4u + 1, 1, 3, fp);
                }
                fclose(fp);
            }
        }
        rsx_live_draw_present(0);
    }
    const char* e = getenv("KZ3_HOST_AA");
    printf("[test_host_aa] KZ3_HOST_AA=%s arm=%d: %s\n", e ? e : "(unset)", arm, g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
