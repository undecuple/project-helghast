/* Patch 2017 host test: guest writes that live-draw did not issue, through ps3recomp's FIFO walker and
 * live-draw on a real 4 GB guest reservation (harness as test_nv0039).
 * Every draw is a 4x4 quad sampling one A8R8G8B8 texture into its own 4x4 RGBA8 surface; surfaces are read back.
 *   S  frame 1: draw pattern P1 into surface S. Frame 2: the guest overwrites S's memory with P2 (plain memcpy,
 *      no FIFO, no hook), then a draw samples S: must see P2 (sampled-hash validation re-uploads the bands).
 *   N  no false positive: surface N drawn with P1 in frame 1, untouched; frame 2 an NV0039 copy out of N (write-
 *      back) and a draw sampling N: must see P1.
 *   H  same frame, explicit hook: draw P1 into H, guest writes P2 into H's memory and calls
 *      cellGcm_note_guest_write_ea (the SPU DMA hook), draw sampling H: must see P2.
 *   T  texture (not a surface) in main memory, same frame: bind with P1, guest writes P2 + hook, bind again:
 *      second draw must see P2. Next frame: the guest writes P3 with no hook: must see P3 (per-frame re-hash).
 *   M  frame 3: draw P1 into a colour surface in MAIN memory, then a back-end semaphore release (0x1D6C/0x1D70):
 *      the guest bytes must hold P1 as A,R,G,B before the label is written (SPU readers of RSX output, KZ3's
 *      post-chain inputs). LD_MAIN_SURFACE_WRITEBACK=0: still zero, FAIL.
 *   Q  KZ3 patch 2033 (quiet-texture hashing; the test forces LD_TEX_HASH_SPAN=3, so a quiet texture is hashed,
 *      skipped, skipped, hashed): texture Q bound with P1 for four frames (quiet = 3 after the third hash). Q1: a
 *      write with no hook, then a bind on the first skip frame: must still show P1 (the rate limit engages). Q2:
 *      two frames later (the next hash frame): P2 (bounded staleness, N-1 frames). Q3: quiet again, then a write
 *      + rsx_live_draw_note_guest_write_pages (the SPU DMA page note) before a bind on a skip frame: P3 the same
 *      frame. Q4: quiet again, a bind on a skip frame, then the write + page note after it; the next frame (also
 *      a skip frame): P4 (the flip carries the page bits over). The older cases run in span mode too.
 *      Q5 (patch 2046): quiet again, then a no-hook write on the first skip frame: stale P4 in the untiered mode.
 *   KZ3_GW_TIER (patch 2046, the size tier LD_TEX_HASH_BIG): unset/0 = LD_TEX_HASH_BIG=0, the untiered 2033 rule
 *      above; 1 = LD_TEX_HASH_BIG=64 (the 4x4 ARGB texture's 64 bytes count as big): Q1-Q4 as above, Q5 shows P5
 *      the same frame (Q3's content change made the texture dynamic: hashed every frame from then on); 2 =
 *      LD_TEX_HASH_BIG=65 (the texture is small: hashed every frame): Q1 shows P2 and Q5 P5. Modes 0-2 set
 *      LD_TEX_CLEAN=0 (patch 2079 off). Run all four.
 *   KZ3_GW_TIER=3 (patch 2079, clean-bitmap skip, LD_TEX_CLEAN=1, LD_TEX_CLEAN_PERIOD=8; replaces Q on texture
 *      C = TEXM4): C1 a no-hook write the frame after creation: stale P1 (clean, never hashed whatever the size).
 *      C2 write + page note, same frame: P3. C3 page note after a bind, next frame: P4 (flip carry). C4 (patch
 *      2078) write + the hook cellGcmSys installs through fs_set_guest_write_hook (what cellFsRead calls after
 *      its fread; this test defines fs_set_guest_write_hook to capture it), same frame: P5. C5 no-hook write:
 *      stale P5 the next frame, P6 after 8 more frames (the periodic net, P/2+1..P frames). C6 the net saw an
 *      unseen writer, so the next no-hook write shows the same frame: P7 (pinned until quiet).
 * Negative control: LD_GUEST_WRITE_CHECK=0 makes S and H (and T's same-frame part) show the stale P1: FAIL.
 * Usage: test_guest_write.exe (exit 0 and "[test_guest_write] PASS"). Built by tools/smoke/build-host.sh, run
 * under Proton with tools/proton-test.sh (GPU lock).
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

static u32 g_color_dma = CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER;
static void emit_case(u32 color_fmt, u32 surf, u32 tex_loc, u32 tex_off, u32 tex_fmt, u32 pitch)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, g_color_dma);
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


extern void rsx_live_draw_present(u32 buffer_id);
extern void cellGcm_note_guest_write_ea(u32 ea, u32 size);
extern void rsx_live_draw_note_guest_write_pages(u32 location, u32 offset, u32 size);

#define TEXM1 0x00050000u   /* main: 4x4 A8R8G8B8 pitch 16, pattern P1 */
#define TEXM2 0x00050100u   /* main: texture T (rewritten by the guest) */
#define TEXM3 0x00050200u   /* main: NV0039 write-back destination of N */
#define TEXM4 0x00050300u   /* main: texture Q (patch 2033 quiet-texture hashing) */
#define MAINS 0x00060000u   /* main: colour surface M (pitch 64) */
#define SURF_S 0x00000000u
#define SURF_N 0x00001000u
#define SURF_H 0x00002000u
#define D_S    0x00003000u
#define D_N    0x00004000u
#define D_H    0x00005000u
#define D_T1   0x00006000u
#define D_T2   0x00007000u
#define D_T3   0x00008000u
#define D_T0   0x00009000u
#define D_Q0   0x0000A000u
#define D_Q1   0x0000B000u
#define D_Q2   0x0000C000u
#define D_Q3   0x0000D000u
#define D_Q4   0x0000E000u
#define D_Q5   0x0000F000u   /* patch 2046 */
#define D_C5S  0x00010000u   /* patch 2079 */
#define D_C6   0x00011000u   /* patch 2079 */

/* Patch 2078: cellGcmSys installs its page-note hook here (the fs layer's setter; ppu_fs.cpp is not linked). */
extern void cellGcm_spu_guest_write_hook_install(void);
static void (*g_fs_hook)(uint32_t ea, uint32_t size);
void fs_set_guest_write_hook(void (*hook)(uint32_t ea, uint32_t size)) { g_fs_hook = hook; }

static int g_fail, g_pass;
/* Pattern k texel (x,y) in host RGBA order. */
static uint8_t pat(int k, u32 x, u32 y, u32 c) { return (uint8_t)(k * 61u + x * 37u + y * 11u + c * 71u + 5u); }
/* Write pattern k as guest A8R8G8B8 bytes (A,R,G,B) at ea with the given pitch. */
static void put_pat(u32 ea, u32 pitch, int k)
{
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            uint8_t* p = vm_base + ea + y * pitch + x * 4u;
            p[0] = pat(k, x, y, 3); p[1] = pat(k, x, y, 0); p[2] = pat(k, x, y, 1); p[3] = pat(k, x, y, 2);
        }
}
static u32 g_ctrl_ea, g_cdata;
static void kick(void)
{
    gw32(g_cdata + 0x8, IO_ADDR + g_head);
    vm_write32(g_ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
}
static void check_surface(const char* tag, u32 off, int k)
{
    static uint8_t buf[4 * 4 * 4];
    u32 w = 0, h = 0;
    rsx_live_draw_flush();
    const int texel = rsx_live_draw_debug_read_surface(0, off, buf, sizeof buf, &w, &h);
    if (texel != 4 || w != 4 || h != 4) {
        printf("[test_guest_write] FAIL %s: surface read texel=%d %ux%u\n", tag, texel, w, h); g_fail++; return;
    }
    int bad = 0, stale = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++)
            for (u32 c = 0; c < 4; c++) {
                const uint8_t got = buf[(y * 4 + x) * 4 + c];
                if (got == pat(k, x, y, c)) { g_pass++; continue; }
                bad++; g_fail++;
                if (got == pat(1, x, y, c)) stale++;
            }
    printf("[test_guest_write] %s: %d/64 bytes differ from P%d%s\n", tag, bad, k,
           bad && stale == bad ? " (all equal the stale P1)" : "");
}

int main(void)
{
    _putenv("RSX_LIVE_DRAW=1");
    _putenv("LD_TEX_HASH_SPAN=3");   /* patch 2033: case Q; the other cases must hold in span mode too */
    const char* tier_env = getenv("KZ3_GW_TIER");   /* patch 2046: the size tier, three modes (header) */
    const int tier = tier_env ? atoi(tier_env) : 0;
    _putenv(tier == 0 || tier == 3 ? "LD_TEX_HASH_BIG=0" : tier == 1 ? "LD_TEX_HASH_BIG=64" : "LD_TEX_HASH_BIG=65");
    _putenv(tier == 3 ? "LD_TEX_CLEAN=1" : "LD_TEX_CLEAN=0");   /* patch 2079 */
    _putenv("LD_TEX_CLEAN_PERIOD=8");
    printf("[test_guest_write] tier mode %d (LD_TEX_HASH_BIG=%s, LD_TEX_CLEAN=%d)\n", tier,
           tier == 0 || tier == 3 ? "0" : tier == 1 ? "64" : "65", tier == 3);
    if (vm_reserve() != 0) { printf("[test_guest_write] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_guest_write") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_guest_write] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    g_cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    const u32 local = cellGcmResolveLocated(1, 0);
    cellGcm_spu_guest_write_hook_install();   /* as cellGcmInit (patch 2033/2078) */
    if (!g_fs_hook) { printf("[test_guest_write] FAIL: no fs guest-write hook installed (patch 2078)\n"); g_fail++; }

    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    put_pat(IO_ADDR + TEXM1, 16, 1);
    put_pat(IO_ADDR + TEXM2, 16, 1);
    put_pat(IO_ADDR + TEXM4, 16, 1);
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
    const u32 ARGB = 0x85u | 0x20u;   /* A8R8G8B8, linear */
    /* Frame 1: P1 into S and N; the guest bytes under them stay as they were (zero). */
    emit_case(0x8, SURF_S, 2u, TEXM1, ARGB, 16);
    emit_case(0x8, SURF_N, 2u, TEXM1, ARGB, 16);
    kick();
    rsx_live_draw_present(0);

    /* Frame 2. S: the guest overwrites S's memory (surface pitch 64) behind live-draw's back. */
    put_pat(local + SURF_S, 64, 2);
    emit_case(0x8, D_S, 1u, SURF_S, ARGB, 64);
    /* N: copy out of N (write-back), then sample N. */
    emit_copy(0xFEED0000u, 0xFEED0001u, SURF_N, TEXM3, 64, 16, 16, 4);
    emit_case(0x8, D_N, 1u, SURF_N, ARGB, 64);
    /* H: draw P1 into H, then (between FIFO segments) the guest writes P2 + hook. */
    emit_case(0x8, SURF_H, 2u, TEXM1, ARGB, 16);
    /* T: texture TEXM2 (P1) bound once this frame. */
    emit_case(0x8, D_T0, 2u, TEXM2, ARGB, 16);
    kick();
    put_pat(local + SURF_H, 64, 2);
    cellGcm_note_guest_write_ea(local + SURF_H, 64u * 3u + 16u);
    put_pat(IO_ADDR + TEXM2, 16, 2);
    cellGcm_note_guest_write_ea(IO_ADDR + TEXM2, 64u);
    emit_case(0x8, D_H, 1u, SURF_H, ARGB, 64);
    emit_case(0x8, D_T1, 2u, TEXM2, ARGB, 16);
    kick();
    /* T without a hook, same frame: stays stale until the next frame (reported, not checked). */
    put_pat(IO_ADDR + TEXM2, 16, 3);
    emit_case(0x8, D_T2, 2u, TEXM2, ARGB, 16);
    kick();
    rsx_live_draw_present(0);
    /* Frame 3: the per-frame re-hash picks up P3. */
    emit_case(0x8, D_T3, 2u, TEXM2, ARGB, 16);
    /* M: a main-memory colour surface, then a back-end semaphore release. */
    memset(vm_base + IO_ADDR + MAINS, 0, 64 * 4);
    g_color_dma = CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER;
    emit_case(0x8, MAINS, 2u, TEXM1, ARGB, 16);
    g_color_dma = CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER;
    emit(0x1D6C, 0x100u);
    emit(0x1D70, 1u);
    kick();
    { int bad = 0;   /* checked right after the FIFO segment with the release, before any flip */
      for (u32 y = 0; y < 4; y++)
          for (u32 x = 0; x < 4; x++) {
              const uint8_t* p = vm_base + IO_ADDR + MAINS + y * 64u + x * 4u;
              const int ok = p[0] == pat(1, x, y, 3) && p[1] == pat(1, x, y, 0) && p[2] == pat(1, x, y, 1) &&
                             p[3] == pat(1, x, y, 2);
              if (ok) g_pass++; else { bad++; g_fail++; }
          }
      printf("[test_guest_write] M (main-memory surface at a semaphore release): %d/16 texels differ\n", bad); }
    rsx_live_draw_present(0);
    /* Q (patch 2033), N = 3. Four frames of Q with P1: created (quiet 0), then hashed unchanged three times
     * (quiet 3); the next two frames are skip frames (last hash 1 and 2 frames ago), the third is a hash frame. */
    if (tier == 3) {   /* patch 2079: case C on TEXM4 (header) */
        for (int f = 0; f < 2; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
        put_pat(IO_ADDR + TEXM4, 16, 2);                                   /* C1: no hook */
        emit_case(0x8, D_Q1, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        put_pat(IO_ADDR + TEXM4, 16, 3);                                   /* C2: page note, same frame */
        rsx_live_draw_note_guest_write_pages(1u, TEXM4, 64u);
        emit_case(0x8, D_Q2, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick();                 /* C3: note after the bind */
        put_pat(IO_ADDR + TEXM4, 16, 4);
        rsx_live_draw_note_guest_write_pages(1u, TEXM4, 64u);
        rsx_live_draw_present(0);
        emit_case(0x8, D_Q3, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        put_pat(IO_ADDR + TEXM4, 16, 5);                                   /* C4: the fs hook (cellFsRead) */
        if (g_fs_hook) g_fs_hook(IO_ADDR + TEXM4, 64u);
        emit_case(0x8, D_Q4, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        /* One frame for the flip-carried flag of C4's note (hashed, unchanged), then C5. */
        emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        put_pat(IO_ADDR + TEXM4, 16, 6);                                   /* C5: no hook: stale, then the net */
        emit_case(0x8, D_C5S, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        for (int f = 0; f < 8; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
        emit_case(0x8, D_Q5, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
        put_pat(IO_ADDR + TEXM4, 16, 7);                                   /* C6: pinned, same frame */
        emit_case(0x8, D_C6, 2u, TEXM4, ARGB, 16); kick();
    } else {
    for (int f = 0; f < 4; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
    /* First skip frame: no-hook write, bind -> the stale P1. */
    put_pat(IO_ADDR + TEXM4, 16, 2);
    emit_case(0x8, D_Q1, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
    /* Second skip frame (still P1, not checked), then the hash frame -> P2. */
    emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
    emit_case(0x8, D_Q2, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
    /* Quiet again (three unchanged hashes), then on the first skip frame: write + page note, then the bind. */
    for (int f = 0; f < 3; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
    put_pat(IO_ADDR + TEXM4, 16, 3);
    rsx_live_draw_note_guest_write_pages(1u, TEXM4, 64u);
    emit_case(0x8, D_Q3, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
    /* Quiet again. First skip frame: bind first, then the write + page note; the second skip frame must show P4. */
    for (int f = 0; f < 3; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
    emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick();
    put_pat(IO_ADDR + TEXM4, 16, 4);
    rsx_live_draw_note_guest_write_pages(1u, TEXM4, 64u);
    rsx_live_draw_present(0);
    emit_case(0x8, D_Q4, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0);
    /* Q5 (patch 2046): quiet again, then a no-hook write on the first skip frame, then the bind. */
    for (int f = 0; f < 3; f++) { emit_case(0x8, D_Q0, 2u, TEXM4, ARGB, 16); kick(); rsx_live_draw_present(0); }
    put_pat(IO_ADDR + TEXM4, 16, 5);
    emit_case(0x8, D_Q5, 2u, TEXM4, ARGB, 16); kick();
    }
    rsx_live_draw_flush();
    printf("[test_guest_write] ring 0x%X bytes, get=0x%X\n", g_head, vm_read32(g_ctrl_ea + 4));

    check_surface("S (guest write, next frame)", D_S, 2);
    check_surface("N (no false positive after write-back)", D_N, 1);
    check_surface("H (guest write + hook, same frame)", D_H, 2);
    check_surface("T0 (texture P1)", D_T0, 1);
    check_surface("T1 (texture rewritten + hook, same frame)", D_T1, 2);
    { const int f0 = g_fail, p0 = g_pass;
      check_surface("T2 (texture rewritten, no hook, same frame; informational)", D_T2, 3);
      g_fail = f0; g_pass = p0; }
    check_surface("T3 (next frame, no hook)", D_T3, 3);
    if (tier == 3) {   /* patch 2079 */
        check_surface("C1 (clean bitmap, no-hook write, next frame: hash skipped, stale P1 expected)", D_Q1, 1);
        check_surface("C2 (write + page note, same frame)", D_Q2, 3);
        check_surface("C3 (page note after the bind, next frame: flip carry)", D_Q3, 4);
        check_surface("C4 (write + fs hook as cellFsRead, same frame; patch 2078)", D_Q4, 5);
        check_surface("C5a (no-hook write, next frame: stale P5 expected)", D_C5S, 5);
        check_surface("C5b (periodic net within LD_TEX_CLEAN_PERIOD=8 frames)", D_Q5, 6);
        check_surface("C6 (unseen writer pinned: no-hook write, same frame)", D_C6, 7);
    } else {
    check_surface(tier == 2 ? "Q1 (small texture under the tier: hashed every frame, P2 expected)"
                            : "Q1 (quiet texture, no-hook write, next frame: hash skipped, stale P1 expected)",
                  D_Q1, tier == 2 ? 2 : 1);
    check_surface("Q2 (the next hash frame, two frames later)", D_Q2, 2);
    check_surface("Q3 (skip frame, write + page note before the bind, same frame)", D_Q3, 3);
    check_surface("Q4 (write + page note after a skip-frame bind, next frame)", D_Q4, 4);
    check_surface(tier ? "Q5 (tier: dynamic or small texture, no-hook write, same frame: P5 expected)"
                       : "Q5 (untiered: quiet again, no-hook write on a skip frame: stale P4 expected)",
                  D_Q5, tier ? 5 : 4);
    }
    /* The write-back put N's pixels (P1) into main memory as A,R,G,B. */
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            const uint8_t* p = vm_base + IO_ADDR + TEXM3 + y * 16u + x * 4u;
            const int ok = p[0] == pat(1, x, y, 3) && p[1] == pat(1, x, y, 0) && p[2] == pat(1, x, y, 1) &&
                           p[3] == pat(1, x, y, 2);
            if (ok) g_pass++; else if (g_fail++ < 24) printf("[test_guest_write] FAIL N write-back texel %u,%u\n", x, y);
        }
    printf("[test_guest_write] %d checks match, %d mismatch\n", g_pass, g_fail);
    printf("[test_guest_write] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
