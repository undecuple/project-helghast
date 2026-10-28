/* Patch 2047 host test: the live renderer's draw payload cache (LD_VTX_CACHE) through ps3recomp's FIFO walker and
 * live-draw on a real 4 GB guest reservation (harness as test_guest_write). Every draw is a 4x4 quad sampling the
 * 4x4 A8R8G8B8 texture P1 into its own 4x4 surface; the vertex array (8 vertices: 0-3 and 4-7, float4 pos/colour/uv,
 * stride 48) and the u16 index buffer live in LOCAL memory, so the cache takes them. A vertex with uv u = 0..1 left
 * to right gives P1 (N); the mirrored uv gives P1 mirrored in x (M). Each step: arrays draw A, indexed draw I.
 *   F1 A1/I1 N (key seen) . F2 A2/I2 N (filled) . F3 A3/I3 N (hits); then the guest writes mirrored uvs into 0-3
 *   with the page note (rsx_live_draw_note_guest_write_pages: the SPU DMA hook) and A4/I4 the same frame: M.
 *   F4 A5/I5 M (refill) . F5 A6/I6 M (hits); then 0-3 back to normal uvs with NO hook: A7/I7 N (sampled hash).
 *   F6 A8/I8 N (hits); the indices switch to 4-7 (mirrored) with the page note: I9 M. F7 I10 M (I's entry is now
 *   dynamic: fetched every time), A9 N (hit). F8 A10 N (hit); then 0-3 get mirrored uvs with the page note and the
 *   frame ends with no draw: F9 A11 M (the page's frame stamp from the flip; inval-tracked must rise).
 * Counters (rsx_live_draw_debug_vc_stats): hits >= 8, inval-tracked >= 4, inval-sample >= 2 in mode 1; in verify
 * mode every hit is fetched again and compared: verify-bad must be 0 and verify-ok >= 8.
 * KZ3_VC_MODE = 1 (default) | verify | 0 (cache off: every case still passes, the counters stay 0).
 * Usage: test_vtx_cache.exe (exit 0 and "[test_vtx_cache] PASS"). Built by tools/smoke/build-host.sh, run under
 * Proton with tools/proton-test.sh inside tools/agents/lock.sh gpu.
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
#define FP_OFFSET  0x00030000u
/* Local-memory surfaces, 4x4, pitch 64. */
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

static __attribute__((unused)) void emit_sub(u32 subch, u32 method, const u32* w, u32 n)
{
    emit_word((n << 18) | (subch << 13) | (method & 0x1FFCu));
    for (u32 i = 0; i < n; i++) emit_word(w[i]);
}
/* cellGcmSetTransferData's shape (KZ3 0x008D2DAC): 0x2184 x2 then 0x230C x8. */
static __attribute__((unused)) void emit_copy(u32 in_dma, u32 out_dma, u32 off_in, u32 off_out, s32 pin, s32 pout, u32 len, u32 n)
{
    const u32 d[2] = { in_dma, out_dma };
    emit_sub(1, 0x0184, d, 2);
    const u32 w[8] = { off_in, off_out, (u32)pin, (u32)pout, len, n, 0x101u, 0u };
    emit_sub(1, 0x030C, w, 8);
}


#define VTXL   0x00200000u   /* local: 8 vertices, stride 48 */
#define IDXL   0x00300000u   /* local: 4 u16 indices */
#define TEXM1  0x00050000u   /* main: 4x4 A8R8G8B8 pitch 16, pattern P1 */
#define DST0   0x00020000u   /* local: destination surfaces, 0x1000 apart */

extern void rsx_live_draw_present(u32 buffer_id);
extern void rsx_live_draw_note_guest_write_pages(u32 location, u32 offset, u32 size);
extern void rsx_live_draw_debug_vc_stats(u64 out[6]);

static int g_fail, g_pass;
static uint8_t pat(u32 x, u32 y, u32 c) { return (uint8_t)(61u + x * 37u + y * 11u + c * 71u + 5u); }
static u32 g_ctrl_ea, g_cdata, g_local, g_dst;

static void kick(void)
{
    gw32(g_cdata + 0x8, IO_ADDR + g_head);
    vm_write32(g_ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
}
static void put_verts(u32 first, int mirror)
{
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (u32 i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  mirror ? 1.0f - us[i] : us[i], vs[i], 0, 1 };
        for (u32 k = 0; k < 12; k++) gf32(g_local + VTXL + (first + i) * 48u + k * 4u, v[k]);
    }
}
static void put_indices(u32 first)
{
    for (u32 i = 0; i < 4; i++) gw16(g_local + IDXL + i * 2u, (uint16_t)(first + i));
}
/* One draw into the next destination surface; returns its offset. */
static u32 draw(int indexed)
{
    const u32 surf = DST0 + g_dst; g_dst += 0x1000u;
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      0x8u | (0x1u << 5) | (0x1u << 8));
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
    emit(NV4097_SET_TEXTURE_OFFSET,       TEXM1);
    emit(NV4097_SET_TEXTURE_FORMAT,       2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_CONTROL3,     16u);
    emit(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT,   (4u << 16) | 4u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
    emit(NV4097_SET_SHADER_PROGRAM, FP_OFFSET | 2u);
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    if (indexed) {
        emit(NV4097_SET_INDEX_ARRAY_ADDRESS, IDXL);
        emit(NV4097_SET_INDEX_ARRAY_DMA, 0u | (1u << 4));   /* local, u16 */
        emit(NV4097_DRAW_INDEX_ARRAY, (3u << 24) | 0u);
    } else
        emit(NV4097_DRAW_ARRAYS, (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
    return surf;
}
static void check(const char* tag, u32 off, int mirror)
{
    static uint8_t buf[4 * 4 * 4];
    u32 w = 0, h = 0;
    rsx_live_draw_flush();
    const int texel = rsx_live_draw_debug_read_surface(0, off, buf, sizeof buf, &w, &h);
    if (texel != 4 || w != 4 || h != 4) {
        printf("[test_vtx_cache] FAIL %s: surface read texel=%d %ux%u\n", tag, texel, w, h); g_fail++; return;
    }
    int bad = 0, other = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++) {
                const uint8_t got = buf[(y * 4 + x) * 4 + c];
                if (got == pat(mirror ? 3u - x : x, y, c)) { g_pass++; continue; }
                bad++; g_fail++;
                if (got == pat(mirror ? x : 3u - x, y, c)) other++;
            }
    printf("[test_vtx_cache] %s: expect %s, %d/64 bytes differ%s\n", tag, mirror ? "M" : "N", bad,
           bad && other == bad ? " (all equal the other orientation: stale payload)" : "");
}
#define FRAME_END() do { kick(); rsx_live_draw_present(0); } while (0)

int main(void)
{
    const char* mode_env = getenv("KZ3_VC_MODE");
    const char* mode = mode_env ? mode_env : "1";
    char envbuf[64]; snprintf(envbuf, sizeof envbuf, "LD_VTX_CACHE=%s", mode);
    _putenv("RSX_LIVE_DRAW=1");
    _putenv(envbuf);
    printf("[test_vtx_cache] %s\n", envbuf);
    if (vm_reserve() != 0) { printf("[test_vtx_cache] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_vtx_cache") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_vtx_cache] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    g_cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    g_local = cellGcmResolveLocated(1, 0);

    put_verts(0, 0); put_verts(4, 1); put_indices(0);
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            uint8_t* p = vm_base + IO_ADDR + TEXM1 + y * 16u + x * 4u;
            p[0] = pat(x, y, 3); p[1] = pat(x, y, 0); p[2] = pat(x, y, 1); p[3] = pat(x, y, 2);
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
        emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attr[a] * 4, VTXL + a * 16u);   /* bit 31 clear: local */
        emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr[a] * 4, 2u | (4u << 4) | (48u << 8));
    }

    u32 a, i;
    a = draw(0); i = draw(1); FRAME_END(); check("F1 A1", a, 0); check("F1 I1", i, 0);
    a = draw(0); i = draw(1); FRAME_END(); check("F2 A2", a, 0); check("F2 I2", i, 0);
    a = draw(0); i = draw(1); kick(); check("F3 A3 (hit)", a, 0); check("F3 I3 (hit)", i, 0);
    put_verts(0, 1); rsx_live_draw_note_guest_write_pages(0u, VTXL, 4u * 48u);
    a = draw(0); i = draw(1); FRAME_END(); check("F3 A4 (tracked write, same frame)", a, 1); check("F3 I4", i, 1);
    a = draw(0); i = draw(1); FRAME_END(); check("F4 A5", a, 1); check("F4 I5", i, 1);
    a = draw(0); i = draw(1); kick(); check("F5 A6 (hit)", a, 1); check("F5 I6 (hit)", i, 1);
    put_verts(0, 0);   /* no hook: the sampled content hash */
    a = draw(0); i = draw(1); FRAME_END(); check("F5 A7 (untracked write)", a, 0); check("F5 I7", i, 0);
    a = draw(0); i = draw(1); kick(); check("F6 A8", a, 0); check("F6 I8", i, 0);
    put_indices(4); rsx_live_draw_note_guest_write_pages(0u, IDXL, 8u);
    i = draw(1); FRAME_END(); check("F6 I9 (index switch, tracked)", i, 1);
    i = draw(1); a = draw(0); FRAME_END(); check("F7 I10", i, 1); check("F7 A9 (hit)", a, 0);
    a = draw(0); kick(); check("F8 A10 (hit)", a, 0);
    put_verts(0, 1); rsx_live_draw_note_guest_write_pages(0u, VTXL, 4u * 48u);
    FRAME_END();
    u64 s0[6]; rsx_live_draw_debug_vc_stats(s0);
    a = draw(0); FRAME_END(); check("F9 A11 (tracked write in the previous frame)", a, 1);
    u64 s1[6]; rsx_live_draw_debug_vc_stats(s1);
    if (!strcmp(mode, "1") && s1[2] <= s0[2]) {
        printf("[test_vtx_cache] FAIL F9: the frame stamp did not invalidate (inval-tracked %llu -> %llu)\n",
               (unsigned long long)s0[2], (unsigned long long)s1[2]); g_fail++;
    }

    u64 st[6]; rsx_live_draw_debug_vc_stats(st);
    printf("[test_vtx_cache] counters hits=%llu filled=%llu inval-tracked=%llu inval-sample=%llu verify=%llu/%llu\n",
           (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2], (unsigned long long)st[3],
           (unsigned long long)st[4], (unsigned long long)st[5]);
    if (!strcmp(mode, "1")) {
        if (st[0] < 8 || st[2] < 4 || st[3] < 2) {
            printf("[test_vtx_cache] FAIL counters: want hits>=8, inval-tracked>=4, inval-sample>=2\n"); g_fail++; }
    } else if (!strcmp(mode, "verify")) {
        if (st[5] || st[4] < 8) { printf("[test_vtx_cache] FAIL counters: want verify-bad=0, verify-ok>=8\n"); g_fail++; }
    } else if (st[0] || st[1]) { printf("[test_vtx_cache] FAIL counters: cache off but used\n"); g_fail++; }
    printf("[test_vtx_cache] %d checks match, %d mismatch\n", g_pass, g_fail);
    printf("[test_vtx_cache] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
