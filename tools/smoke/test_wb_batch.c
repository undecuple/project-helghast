/* Patch 2018 host test and benchmark: batched write-backs of main-memory render targets at a guest sync point,
 * through ps3recomp's FIFO walker and live-draw on a real 4 GB guest reservation (harness as test_guest_write).
 * One frame has Killzone 3's shape (RSX pass 19, RPCS3 level capture): a 1280x720 scene in local memory, an
 * NV0039 copy of it into main memory, four colour surfaces in MAIN memory (640x360, 320x180, 640x360 A8R8G8B8 and
 * 320x180 F_X32), then a back-end semaphore release (0x1D6C/0x1D70) and a flip. Every draw samples a 4x4 pattern
 * texture with nearest filtering, so each surface is a 4x4 grid of solid blocks.
 * Checks per frame, right after the release: the block centres of the four main surfaces hold the pattern in the
 * guest layout (A,R,G,B bytes; F_X32 = big-endian float of the red channel), and the NV0039 destination holds the
 * scene. Timing: wall time of the FIFO segment with the release, and the write-back totals
 * (rsx_live_draw_debug_writeback_stats). LD_WRITEBACK_BATCH=0 runs the per-surface path for the A/B.
 * Patch 2029 (no-wait chunked submits): every frame also draws EXTRA quads into the scene first (default 150), so
 * the walker submits in chunks (LD_SUBMIT_CHUNK, default 128) and at every idle point, and frames alternate between
 * two textures with different patterns, so a write-back that read the GPU's result too early shows the previous
 * frame's pattern. Negative control: LD_WB_UNSAFE_NOWAIT=1 (the write-back does not wait for its copies) must FAIL;
 * LD_SUBMIT_CHUNK=0 is the pre-2029 path. The run prints the no-wait submit count.
 * Usage: test_wb_batch.exe [frames] [extra] (default 60 150; exit 0 and "[test_wb_batch] PASS"). Built by
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
extern void  rsx_live_draw_present(u32 buffer_id);
extern void  rsx_live_draw_debug_writeback_stats(double* us_total, u32* batches, u32* surfaces, u32* gpu_waits);
extern unsigned long long rsx_live_draw_debug_chunk_submits(void);   /* patch 2029 */

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
#define TEXM2   0x000B0000u          /* IO: the same with the second pattern (odd frames, patch 2029) */
static u32 g_tex = TEXM1;
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
    emit(NV4097_SET_TEXTURE_OFFSET,       g_tex);
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
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
    emit(NV4097_SET_BEGIN_END, 0u);
}

static u32 g_par;   /* frame parity: which pattern the frame draws */
static uint8_t pat(u32 x, u32 y, u32 c) { return (uint8_t)(61u + x * 37u + y * 11u + c * 71u + 5u + g_par * 97u); }
static u32 g_ctrl_ea, g_cdata;
static void kick(void)
{
    gw32(g_cdata + 0x8, IO_ADDR + g_head);
    vm_write32(g_ctrl_ea + 0, g_head);
    cellGcm_rsx_process_fifo();
}
static int g_fail, g_pass;
/* Block centres of a w x h guest surface at IO offset off: A,R,G,B bytes, or a BE float of red (F_X32). */
static void check_main(const char* tag, u32 off, u32 pitch, u32 w, u32 h, int fx32, int frame)
{
    int bad = 0;
    for (u32 ty = 0; ty < 4; ty++)
        for (u32 tx = 0; tx < 4; tx++) {
            const u32 x = (2u * tx + 1u) * w / 8u, y = (2u * ty + 1u) * h / 8u;
            const uint8_t* p = vm_base + IO_ADDR + off + y * pitch + x * (fx32 ? 4u : 4u);
            int ok;
            if (fx32) {
                const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
                float f; memcpy(&f, &v, 4);
                const float want = (float)pat(tx, ty, 0) / 255.0f;
                ok = f > want - 0.002f && f < want + 0.002f;
            } else
                ok = p[0] == pat(tx, ty, 3) && p[1] == pat(tx, ty, 0) && p[2] == pat(tx, ty, 1) && p[3] == pat(tx, ty, 2);
            if (ok) g_pass++; else { g_fail++; bad++; }
        }
    if (bad && g_fail < 64) printf("[test_wb_batch] FAIL frame %d %s: %d/16 blocks wrong\n", frame, tag, bad);
}

#define SCENE  0x00100000u   /* local 1280x720 A8R8G8B8, pitch 5120 */
#define MS0    0x00100000u   /* IO: 640x360 pitch 2560 */
#define MS1    0x00200000u   /* IO: 320x180 pitch 1280 */
#define MS2    0x00300000u   /* IO: 640x360 pitch 2560 */
#define MS3    0x00400000u   /* IO: 320x180 F_X32 pitch 1280 */
#define CPY    0x00500000u   /* IO: NV0039 destination, 1280x720 pitch 5120 */

int main(int argc, char** argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    const int extra = argc > 2 ? atoi(argv[2]) : 150;
    if (frames < 3) frames = 3;
    {   /* the command buffer (512 KB, no wrap): ~1.5 KB per frame + 24 bytes per extra draw */
        const int cap = 500 * 1024 / (1536 + 24 * (extra > 0 ? extra : 0));
        if (frames > cap) frames = cap;
    }
    _putenv("RSX_LIVE_DRAW=1");
    if (vm_reserve() != 0) { printf("[test_wb_batch] 4 GB reservation failed\n"); return 2; }
    if (rsx_null_backend_init(64, 64, "test_wb_batch") != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, live_guest_ptr, NULL) != 0) {
        printf("[test_wb_batch] live-draw init failed\n"); return 2;
    }
    rsx_null_backend_suppress_present(1);
    u32 ctx_out = host_alloc(4, 4);
    g_cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    static const float us[4] = { 0, 1, 1, 0 },   vs[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        const float v[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
        for (int k = 0; k < 12; k++) gf32(IO_ADDR + VTX_OFFSET + (u32)(i * 48 + k * 4), v[k]);
    }
    for (g_par = 0; g_par < 2; g_par++)
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                uint8_t* p = vm_base + IO_ADDR + (g_par ? TEXM2 : TEXM1) + y * 16u + x * 4u;
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
        emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attr[a] * 4, 0x80000000u | (VTX_OFFSET + a * 16u));
        emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr[a] * 4, 2u | (4u << 4) | (48u << 8));
    }
    kick();

    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    double rel_us = 0, rel_max = 0, copy_us = 0;
    double us0 = 0; u32 b0 = 0, s0 = 0, w0 = 0;
    for (int f = 0; f < frames; f++) {
        g_par = (u32)f & 1u; g_tex = g_par ? TEXM2 : TEXM1;
        /* Scene into local memory, then the NV0039 copy into main (write-back of one surface). */
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER, 0x8, SCENE, 5120, 1280, 720);
        for (int k = 0; k < extra; k++) {   /* patch 2029: enough draws for chunked submits */
            emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
            emit(NV4097_DRAW_ARRAYS,   (3u << 24) | 0u);
            emit(NV4097_SET_BEGIN_END, 0u);
        }
        emit_copy(0xFEED0000u, 0xFEED0001u, SCENE, CPY, 5120, 5120, 5120, 720);
        LARGE_INTEGER t0, t1, t2, t3;
        QueryPerformanceCounter(&t0); kick(); QueryPerformanceCounter(&t1);
        copy_us += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)qf.QuadPart;
        /* Four main-memory render targets (KZ3 draws 733-736). */
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, 0x8, MS0, 2560, 640, 360);
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, 0x8, MS1, 1280, 320, 180);
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, 0x8, MS2, 2560, 640, 360);
        emit_draw(CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER, 0xD, MS3, 1280, 320, 180);
        kick();
        memset(vm_base + IO_ADDR + MS0, 0, 2560u * 360u); memset(vm_base + IO_ADDR + MS1, 0, 1280u * 180u);
        memset(vm_base + IO_ADDR + MS2, 0, 2560u * 360u); memset(vm_base + IO_ADDR + MS3, 0, 1280u * 180u);
        /* The release: every dirty main-memory surface reaches guest memory before the label. */
        emit(0x1D6C, 0x100u);
        emit(0x1D70, (u32)f + 1u);
        QueryPerformanceCounter(&t2); kick(); QueryPerformanceCounter(&t3);
        const double us = (double)(t3.QuadPart - t2.QuadPart) * 1e6 / (double)qf.QuadPart;
        if (f >= 2) { rel_us += us; if (us > rel_max) rel_max = us; }   /* frames 0-1: first-use allocations */
        if (f == 1) rsx_live_draw_debug_writeback_stats(&us0, &b0, &s0, &w0);
        check_main("MS0 640x360", MS0, 2560, 640, 360, 0, f);
        check_main("MS1 320x180", MS1, 1280, 320, 180, 0, f);
        check_main("MS2 640x360", MS2, 2560, 640, 360, 0, f);
        check_main("MS3 320x180 F_X32", MS3, 1280, 320, 180, 1, f);
        check_main("NV0039 copy 1280x720", CPY, 5120, 1280, 720, 0, f);
        rsx_live_draw_present(0);
    }
    double us1 = 0; u32 b1 = 0, s1 = 0, w1 = 0;
    rsx_live_draw_debug_writeback_stats(&us1, &b1, &s1, &w1);
    const char* e = getenv("LD_WRITEBACK_BATCH");
    const int n = frames > 2 ? frames - 2 : 1;
    printf("[test_wb_batch] mode=%s frames=%d: release segment %.0f us avg, %.0f us max; copy segment %.0f us avg\n",
           e && atoi(e) == 0 ? "per-surface" : "batched", frames, rel_us / n, rel_max, copy_us / frames);
    printf("[test_wb_batch] write-backs after warm-up: %u calls, %u surfaces, %u GPU waits, %.0f us total, %.0f us per call\n",
           b1 - b0, s1 - s0, w1 - w0, us1 - us0, (b1 - b0) ? (us1 - us0) / (b1 - b0) : 0.0);
    printf("[test_wb_batch] extra draws %d/frame, LD_SUBMIT_CHUNK=%s: %llu no-wait submits\n", extra,
           getenv("LD_SUBMIT_CHUNK") ? getenv("LD_SUBMIT_CHUNK") : "(unset)", rsx_live_draw_debug_chunk_submits());
    printf("[test_wb_batch] %d checks match, %d mismatch\n", g_pass, g_fail);
    printf("[test_wb_batch] %s\n", g_fail ? "FAIL" : "PASS");
    cellGcmTerminate();
    return g_fail ? 1 : 0;
}
