/*
 * KZ3 D3D12 smoke host: the ps3recomp runtime/host/host_main.c demo, brought up to date and run on a real
 * guest address space.
 *
 * What it draws: four quads sampling one 2x2 red/green/blue/yellow texture through a guest vertex program
 * and guest TEX fragment programs, each with different guest sampler state (patch 2001, per-unit samplers):
 *   top left     unit 0,  LINEAR/LINEAR, WRAP,  uv 0..2       -> smooth gradient, tiled 2x2, no seams
 *   top right    unit 0,  NEAREST,       CLAMP_TO_EDGE, uv -0.5..1.5 -> four flat quadrants
 *                (same unit re-programmed between the two draws: per-draw capture)
 *   bottom left  unit 9,  NEAREST,       WRAP,  uv -0.5..1.5  -> 4x4 flat cells, colours shifted by one texel
 *   bottom right unit 15, NEAREST,       BORDER (white), uv -0.5..1.5 -> the four quadrants inside a white frame
 * Units 9 and 15 check that all 16 fragment units are bound (the backend used to stop at 4).
 * It goes through the real path a recompiled title takes: big-endian NV4097 commands in the IO window ->
 * guest control register `put` -> cellGcm_rsx_process_fifo (FIFO walker) -> D3D12 backend -> present.
 *
 * Differences from upstream host_main.c (v0.13.0), each one a reason it did not present:
 *   1. Guest memory: 4 GB reserved on a 4 GB boundary (MEM_RESERVE only), with a vectored exception
 *      handler that commits a 64 KB page on first touch (as ps3recomp templates/project/main.cpp does).
 *      host_main.c callocs 64 MB, but cellGcmInit writes the control register / labels / offset tables at
 *      VM_HLE_INJECT_BASE (0x20000000), so it faulted inside cellGcmInit.
 *   2. cellGcmSetupContext instead of cellGcmInit: the FIFO walker drains nothing until a context exists.
 *   3. The control register is a GUEST address (big-endian, written through vm_base), not a host pointer.
 *   4. Commands follow the conventions of runtime/host/host_posix.c (the maintained harness): vertex and
 *      texture data in the IO window addressed as MAIN memory (vertex offset bit 31, texture location 2),
 *      fragment program location 2, textures enabled via CONTROL0, linear (LN) A8R8G8B8 texture, explicit
 *      CLEAR_SURFACE, drain + flip + present every frame, ring wrapped with a JUMP.
 *
 * --live: the same scene through ps3recomp's live-draw engine (rsx_live_draw.c, patch 2003 for Proton) instead
 * of rsx_d3d12_backend: the null backend opens the window, the engine presents into it (as port/kz3/main.cpp does
 * with RSX_LIVE_DRAW=1). That engine renders into guest surfaces, so this mode also emits a full surface setup
 * (colour A = display buffer 0 in local memory, Z24S8 zeta, viewport scale/offset).
 *
 * --glyph (implies --live, patch 2006): KZ3-style stencil-then-cover text. A 512x512 A8R8G8B8 glyph cache
 * (local GLYPH_OFFSET, zeta shared with the 1280x720 main buffer) gets three glyphs, each in its own
 * 128x128 cell: per cell a scissored colour+stencil clear, outline fans with colour writes off and
 * two-sided stencil, then a cover quad (stencil NOTEQUAL 0, ops ZERO, colour on, white):
 *   cell 0  "C" (concave), INVERT on both faces                      -> a white C
 *   cell 1  "C", INCR_WRAP front / DECR_WRAP back (nonzero winding)  -> a white C
 *   cell 2  "O" as two fans (outer CCW, inner CW), INCR/DECR_WRAP    -> a white ring with a hole
 *   cell 3  the same "O", FRONT_FACE CCW, front INCR_WRAP write mask 0xFF, back write mask 0x00 (per-face
 *           masks, D3D12_DEPTH_STENCIL_DESC2): only the front-facing outer fan counts -> a solid disc
 *           (a ring = per-face masks ignored; a small disc = front/back swapped)
 * The main pass then shows the whole cache texture (black square, glyphs in its top row) in the centre.
 * Wrong per-face ops, culled fans or unscissored clears (only the last glyph survives) show at a glance.
 *
 * Usage: host_kz3.exe [--live|--glyph] [frames]   (0 or none = run until the window is closed)
 * Build: tools/smoke/build-host.sh -> build/smoke/host_kz3.exe
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_d3d12_backend.h"
#include "rsx_test_programs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Runtime entry points without a public declaration (used the same way by runtime/host/host_posix.c). */
extern void cellGcm_rsx_process_fifo(void);
extern int  cellGcm_take_flip_pending_synced(void);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);

uint8_t* vm_base = NULL;
static int g_live = 0;   /* --live: rsx_live_draw instead of rsx_d3d12_backend */
static int g_glyph = 0;  /* --glyph: stencil-then-cover glyph cache scene (live only) */

/* Live-draw engine and null backend (no public header; same declarations as port/kz3/main.cpp). */
extern int   rsx_live_draw_init(void* hwnd, u32 w, u32 h,
                                const uint8_t* (*guest_ptr)(void*, u32, u32, u32), void* user);
extern void  rsx_live_draw_present(u32 buffer_id);
extern int   rsx_null_backend_init(u32 w, u32 h, const char* title);
extern int   rsx_null_backend_pump_messages(void);
extern void* rsx_null_backend_get_hwnd(void);
extern void  rsx_null_backend_suppress_present(int on);
extern u32   cellGcmResolveLocated(int local, u32 offset);
extern u32   cellGcmResolveIO(u32 offset);

#define VM_TOTAL    0x100000000ull
#define IO_ADDR     0x00100000u   /* IO window base EA: FIFO at IO offset 0, data behind it */
#define IO_SIZE     0x00200000u
#define CMD_SIZE    0x00010000u
#define RING_LIMIT  0x0000C000u   /* wrap the command ring with a JUMP before this IO offset */
#define HEAP_BASE   0x00400000u   /* bump allocator for cellGcmSetupContext's guest structures */
/* Data, as IO offsets (EA = IO_ADDR + offset), all MAIN memory. */
#define VTX_OFFSET  0x00010000u   /* 4 quads x 4 vertices: float4 pos, float4 colour, float4 uv; stride 48 */
#define TEX_OFFSET  0x00020000u   /* 2x2 A8R8G8B8, linear */
#define FP_OFFSET   0x00030000u   /* per unit u: TEX r0, TC0, unit u ; END  at FP_OFFSET + u*0x100 */
#define CLEAR_ARGB  0xFF101830u
/* Live mode surfaces, RSX local memory offsets: display buffer 0 at 0, the zeta buffer behind it. */
#define DISPLAY_OFFSET 0x00000000u
#define ZETA_OFFSET    0x00400000u
#define SURF_PITCH     (1280u * 4u)
#define GLYPH_OFFSET   0x00800000u   /* 512x512 A8R8G8B8 glyph cache, pitch 2048 */
#define GLYPH_VTX_BASE 64u           /* glyph vertices: vertex index 64.. of the VTX_OFFSET array */

static volatile LONG g_commits;

/* Commit-on-first-touch for the guest reservation. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    uintptr_t base = (uintptr_t)vm_base;
    if (!vm_base || fault < base || fault >= base + VM_TOTAL) return EXCEPTION_CONTINUE_SEARCH;
    void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
    if (!VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_commits);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Reserve 4 GB on a 4 GB boundary: over-reserve, find the aligned base, release, re-reserve there. The HLE
 * recovers guest addresses by truncating host pointers to 32 bits, so the low 32 bits of vm_base must be 0. */
static int vm_reserve(void)
{
    for (int attempt = 0; attempt < 8 && !vm_base; attempt++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)(VM_TOTAL * 2), MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uint8_t* aligned = (uint8_t*)(((uintptr_t)probe + (uintptr_t)VM_TOTAL - 1u) & ~((uintptr_t)VM_TOTAL - 1u));
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc(aligned, (SIZE_T)VM_TOTAL, MEM_RESERVE, PAGE_NOACCESS);
    }
    if (!vm_base) return -1;
    AddVectoredExceptionHandler(1, vm_commit_veh);
    return 0;
}

/* --- guest writes (big-endian) ------------------------------------------- */
static void guest_w32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void guest_f32(uint32_t ea, float f) { uint32_t w; memcpy(&w, &f, 4); guest_w32(ea, w); }

static uint32_t g_heap = HEAP_BASE;
static u32 host_alloc(u32 size, u32 align)
{
    if (align == 0) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { guest_w32(ea, v); }

/* --- command ring --------------------------------------------------------- */
static uint32_t g_head;   /* IO offset of the next command word */
static void emit_word(uint32_t v) { guest_w32(IO_ADDR + g_head, v); g_head += 4; }
static void emit(uint32_t method, uint32_t data)
{
    emit_word((1u << 18) | (((method >> 2) & 0x7FFu) << 2));   /* one non-incrementing method write */
    emit_word(data);
}

#define VFMT(size, stride) (2u | ((u32)(size) << 4) | ((u32)(stride) << 8))   /* float32 */
#define VTX_MAIN(off)      (0x80000000u | (u32)(off))                          /* bit 31: MAIN memory */

/* The four quads: NDC rectangle and uv range (see the header comment). */
static const struct { float x0, y0, x1, y1, u0, u1; } k_quads[4] = {
    { -0.95f,  0.05f, -0.05f,  0.95f,   0.0f, 2.0f },   /* top left     */
    {  0.05f,  0.05f,  0.95f,  0.95f,  -0.5f, 1.5f },   /* top right    */
    { -0.95f, -0.95f, -0.05f, -0.05f,  -0.5f, 1.5f },   /* bottom left  */
    {  0.05f, -0.95f,  0.95f, -0.05f,  -0.5f, 1.5f },   /* bottom right */
};
static const uint32_t k_fp_units[3] = { 0, 9, 15 };

static void upload_guest_data(void)
{
    /* RSX_PRIMITIVE_QUADS order: top-left, top-right, bottom-right, bottom-left; uv v grows downwards. */
    for (int q = 0; q < 4; q++) {
        const float xs[4] = { k_quads[q].x0, k_quads[q].x1, k_quads[q].x1, k_quads[q].x0 };
        const float ys[4] = { k_quads[q].y1, k_quads[q].y1, k_quads[q].y0, k_quads[q].y0 };
        const float us[4] = { k_quads[q].u0, k_quads[q].u1, k_quads[q].u1, k_quads[q].u0 };
        const float vs[4] = { k_quads[q].u0, k_quads[q].u0, k_quads[q].u1, k_quads[q].u1 };
        for (int i = 0; i < 4; i++) {
            const float v[12] = { xs[i], ys[i], 0.0f, 1.0f,  1, 1, 1, 1,  us[i], vs[i], 0, 1 };
            for (int k = 0; k < 12; k++)
                guest_f32(IO_ADDR + VTX_OFFSET + (uint32_t)((q * 4 + i) * 48 + k * 4), v[k]);
        }
    }

    /* 2x2 A8R8G8B8 (big-endian ARGB words): red, green / blue, yellow. */
    static const uint32_t tex[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u };
    for (int i = 0; i < 4; i++) guest_w32(IO_ADDR + TEX_OFFSET + (uint32_t)i * 4u, tex[i]);

    for (int i = 0; i < 3; i++)
        rsx_test_fp_tex_r0_tc0(vm_base + IO_ADDR + FP_OFFSET + k_fp_units[i] * 0x100u, k_fp_units[i]);
}

/* NV4097 texture register values, libgcm encodings (see port/patches/2001-*.patch for the field layout). */
#define TEX_NEAREST 1u
#define TEX_LINEAR  2u
#define TEX_LINEAR_LINEAR 6u
#define TEX_CONV_QUINCUNX 1u
#define WRAP_WRAP   1u
#define WRAP_CLAMP_TO_EDGE 3u
#define WRAP_BORDER 4u
#define TEX_FILTER(minf, magf)  (((u32)(minf) << 16) | ((u32)(magf) << 24) | (TEX_CONV_QUINCUNX << 13))
#define TEX_ADDRESS(s, t, r)    ((u32)(s) | ((u32)(t) << 8) | ((u32)(r) << 16))
#define TEX_CONTROL0(maxlod_4_8, aniso) (0x80000000u | (((u32)(maxlod_4_8) & 0xFFFu) << 7) | ((u32)(aniso) << 4))

/* One fragment texture unit: the 2x2 texture plus the given sampler state. */
static void emit_texture_unit(u32 unit, u32 filter, u32 address, u32 border)
{
    const u32 b = unit * 0x20u;
    emit(NV4097_SET_TEXTURE_OFFSET + b,       TEX_OFFSET);
    emit(NV4097_SET_TEXTURE_FORMAT + b,       2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_ADDRESS + b,      address);
    emit(NV4097_SET_TEXTURE_CONTROL0 + b,     TEX_CONTROL0(12u << 8, 0u));
    emit(NV4097_SET_TEXTURE_CONTROL1 + b,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_FILTER + b,       filter);
    emit(NV4097_SET_TEXTURE_IMAGE_RECT + b,   (2u << 16) | 2u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR + b, border);
}

static void emit_quad(int q, u32 fp_unit)
{
    emit(NV4097_SET_SHADER_PROGRAM, (FP_OFFSET + fp_unit * 0x100u) | 2u);   /* location 2 = main */
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    emit(NV4097_DRAW_ARRAYS,   (3u << 24) | (u32)(q * 4));                 /* first q*4, count 4 */
    emit(NV4097_SET_BEGIN_END, 0u);
}

/* (location, offset) -> host memory for the live engine: 0 = RSX local memory, 1 = main memory (IO table). */
static const uint8_t* live_guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    u32 ea;
    if (location == 0) ea = cellGcmResolveLocated(1, offset);
    else { ea = cellGcmResolveIO(offset); if (!ea) ea = cellGcmResolveLocated(0, offset); }
    if (!vm_base || !ea || ea == 0xFFFFFFFFu || (uint64_t)ea + min_bytes > 0x100000000ull) return NULL;
    return vm_base + ea;
}

static void emit_f32(u32 method, float f) { u32 w; memcpy(&w, &f, 4); emit(method, w); }

/* What cellGcmSetSurface + cellGcmSetViewport emit for a 1280x720 A8R8G8B8 / Z24S8 pitch surface. */
static void emit_surface_setup(void)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_FORMAT,      0x08u | (0x2u << 5) | (0x1u << 8));   /* A8R8G8B8, Z24S8, pitch */
    emit(NV4097_SET_SURFACE_PITCH_A,     SURF_PITCH);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, DISPLAY_OFFSET);
    emit(NV4097_SET_SURFACE_PITCH_Z,     SURF_PITCH);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, ZETA_OFFSET);
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 1280u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   720u << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, 1280u << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   720u << 16);
    emit(NV4097_SET_SCISSOR_HORIZONTAL,  1280u << 16);
    emit(NV4097_SET_SCISSOR_VERTICAL,    720u << 16);
    emit_f32(0x0A20, 640.0f); emit_f32(0x0A24, 360.0f); emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);  /* offset */
    emit_f32(0x0A30, 640.0f); emit_f32(0x0A34, -360.0f); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f); /* scale */
}

static void emit_vp_and_arrays(void)
{
    /* Vertex program: MOV o0,v0 (HPOS) ; MOV o1,v3 (COL0) ; MOV o7,v8 (TC0) ; END */
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

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(VTX_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(VTX_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8 * 4, VTX_MAIN(VTX_OFFSET + 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8 * 4, VFMT(4, 48));
}

static void emit_frame(void)
{
    if (g_live) emit_surface_setup();
    emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
    emit(NV4097_CLEAR_SURFACE, 0xF0u);

    emit_vp_and_arrays();

    /* Units 9 and 15 for the bottom quads; unit 0 is re-programmed between the top two. */
    emit_texture_unit(9,  TEX_FILTER(TEX_NEAREST, TEX_NEAREST), TEX_ADDRESS(WRAP_WRAP, WRAP_WRAP, WRAP_WRAP), 0);
    emit_texture_unit(15, TEX_FILTER(TEX_NEAREST, TEX_NEAREST),
                      TEX_ADDRESS(WRAP_BORDER, WRAP_BORDER, WRAP_BORDER), 0xFFFFFFFFu);

    emit_texture_unit(0, TEX_FILTER(TEX_LINEAR_LINEAR, TEX_LINEAR), TEX_ADDRESS(WRAP_WRAP, WRAP_WRAP, WRAP_WRAP), 0);
    emit_quad(0, 0);
    emit_texture_unit(0, TEX_FILTER(TEX_NEAREST, TEX_NEAREST),
                      TEX_ADDRESS(WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE), 0);
    emit_quad(1, 0);
    emit_quad(2, 9);
    emit_quad(3, 15);
}

/* --- --glyph scene -------------------------------------------------------- */
#define GL_KEEP 0x1E00u
#define GL_ZERO_OP 0x0000u
#define GL_INVERT 0x150Au
#define GL_INCR_WRAP 0x8507u
#define GL_DECR_WRAP 0x8508u
#define GL_NOTEQUAL 0x0205u
#define GL_ALWAYS 0x0207u
static u32 g_gv;   /* next free glyph vertex index */
/* One vertex in glyph-cache pixels (512x512, y down), stored as NDC for the 512x512 viewport. */
static u32 glyph_vtx(float px, float py, float u, float v)
{
    const float x = px / 256.0f - 1.0f, y = 1.0f - py / 256.0f;
    const float vv[12] = { x, y, 0.0f, 1.0f,  1, 1, 1, 1,  u, v, 0, 1 };
    for (int k = 0; k < 12; k++) guest_f32(IO_ADDR + VTX_OFFSET + g_gv * 48u + (u32)k * 4u, vv[k]);
    return g_gv++;
}
static struct { u32 first, count; } g_fan_c0, g_fan_c1, g_fan_o_out, g_fan_o_in, g_fan_p_out, g_fan_p_in, g_cover[4], g_show;
/* "C": outer arc r=50 from 45 to 315 degrees, inner arc r=30 back; a concave simple polygon. */
static void glyph_c(float cx, float cy, u32* first, u32* count)
{
    const int n = 20;
    *first = g_gv;
    for (int i = 0; i <= n; i++) { float a = (45.0f + 270.0f * i / n) * 3.14159265f / 180.0f;
        glyph_vtx(cx + 50.0f * cosf(a), cy - 50.0f * sinf(a), 0, 0); }
    for (int i = n; i >= 0; i--) { float a = (45.0f + 270.0f * i / n) * 3.14159265f / 180.0f;
        glyph_vtx(cx + 30.0f * cosf(a), cy - 30.0f * sinf(a), 0, 0); }
    *count = g_gv - *first;
}
static void glyph_circle(float cx, float cy, float r, int ccw, u32* first, u32* count)
{
    const int n = 40;
    *first = g_gv;
    for (int i = 0; i < n; i++) { float a = (ccw ? 1.0f : -1.0f) * 6.2831853f * i / n;
        glyph_vtx(cx + r * cosf(a), cy - r * sinf(a), 0, 0); }
    *count = g_gv - *first;
}
static void upload_glyph_data(void)
{
    g_gv = GLYPH_VTX_BASE;
    glyph_c(64.0f, 64.0f, &g_fan_c0.first, &g_fan_c0.count);
    glyph_c(192.0f, 64.0f, &g_fan_c1.first, &g_fan_c1.count);
    glyph_circle(320.0f, 64.0f, 50.0f, 1, &g_fan_o_out.first, &g_fan_o_out.count);
    glyph_circle(320.0f, 64.0f, 25.0f, 0, &g_fan_o_in.first, &g_fan_o_in.count);
    glyph_circle(448.0f, 64.0f, 50.0f, 1, &g_fan_p_out.first, &g_fan_p_out.count);
    glyph_circle(448.0f, 64.0f, 25.0f, 0, &g_fan_p_in.first, &g_fan_p_in.count);
    for (int c = 0; c < 4; c++) {   /* cover quads (QUADS order), uv 5,5 = white border of unit 15 */
        const float x0 = 128.0f * c + 4.0f, x1 = x0 + 120.0f;
        g_cover[c].first = g_gv;
        glyph_vtx(x0, 4.0f, 5, 5); glyph_vtx(x1, 4.0f, 5, 5); glyph_vtx(x1, 124.0f, 5, 5); glyph_vtx(x0, 124.0f, 5, 5);
        g_cover[c].count = 4;
    }
    /* Main-pass quad showing the 512x512 cache at 1:1 in the centre of 1280x720 (NDC, not glyph px). */
    g_show.first = g_gv;
    const float sx = 512.0f / 1280.0f, sy = 512.0f / 720.0f;
    const float q[4][4] = { { -sx, sy, 0, 0 }, { sx, sy, 1, 0 }, { sx, -sy, 1, 1 }, { -sx, -sy, 0, 1 } };
    for (int i = 0; i < 4; i++) {
        const float vv[12] = { q[i][0], q[i][1], 0.0f, 1.0f,  1, 1, 1, 1,  q[i][2], q[i][3], 0, 1 };
        for (int k = 0; k < 12; k++) guest_f32(IO_ADDR + VTX_OFFSET + g_gv * 48u + (u32)k * 4u, vv[k]);
        g_gv++;
    }
    g_show.count = 4;
    rsx_test_fp_tex_r0_tc0(vm_base + IO_ADDR + FP_OFFSET + 1u * 0x100u, 1u);
}
static void emit_draw(u32 prim, u32 first, u32 count, u32 fp_unit)
{
    emit(NV4097_SET_SHADER_PROGRAM, (FP_OFFSET + fp_unit * 0x100u) | 2u);
    emit(NV4097_SET_BEGIN_END, prim);
    emit(NV4097_DRAW_ARRAYS, ((count - 1u) << 24) | first);
    emit(NV4097_SET_BEGIN_END, 0u);
}
static void emit_scissor(u32 x, u32 y, u32 w, u32 h)
{
    emit(NV4097_SET_SCISSOR_HORIZONTAL, (w << 16) | x);
    emit(NV4097_SET_SCISSOR_VERTICAL,   (h << 16) | y);
}
/* Outline fans: colour off, two-sided stencil ALWAYS with the given front/back pass ops, no cull. */
static void emit_fan_state(u32 front_op, u32 back_op)
{
    emit(0x0324, 0u);                       /* COLOR_MASK: nothing */
    emit(0x0328, 1u);                       /* STENCIL_TEST_ENABLE */
    emit(0x0348, 1u);                       /* TWO_SIDED_STENCIL_TEST_ENABLE */
    emit(0x032C, 0xFFu); emit(0x0330, GL_ALWAYS); emit(0x0334, 0u); emit(0x0338, 0xFFu);
    emit(0x033C, GL_KEEP); emit(0x0340, GL_KEEP); emit(0x0344, front_op);
    emit(0x034C, 0xFFu); emit(0x0350, GL_ALWAYS); emit(0x0354, 0u); emit(0x0358, 0xFFu);
    emit(0x035C, GL_KEEP); emit(0x0360, GL_KEEP); emit(0x0364, back_op);
    emit(0x183C, 0u);                       /* CULL_FACE_ENABLE off */
}
/* Cover: colour on, one-sided stencil NOTEQUAL 0, every op ZERO (resets the cell for the next glyph). */
static void emit_cover_state(void)
{
    emit(0x0324, 0x01010101u);
    emit(0x0348, 0u);
    emit(0x0330, GL_NOTEQUAL); emit(0x0334, 0u); emit(0x0338, 0xFFu); emit(0x032C, 0xFFu);
    emit(0x033C, GL_ZERO_OP); emit(0x0340, GL_ZERO_OP); emit(0x0344, GL_ZERO_OP);
}
static void emit_glyph_surface(void)
{
    emit(NV4097_SET_SURFACE_FORMAT,      0x08u | (0x2u << 5) | (0x1u << 8));   /* A8R8G8B8, Z24S8, pitch */
    emit(NV4097_SET_SURFACE_PITCH_A,     2048u);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET, GLYPH_OFFSET);
    emit(NV4097_SET_SURFACE_PITCH_Z,     2048u);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, ZETA_OFFSET);   /* shared with the larger main zeta, on purpose */
    emit(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 512u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   512u << 16);
    emit(NV4097_SET_VIEWPORT_HORIZONTAL, 512u << 16);
    emit(NV4097_SET_VIEWPORT_VERTICAL,   512u << 16);
    emit_f32(0x0A20, 256.0f); emit_f32(0x0A24, 256.0f); emit_f32(0x0A28, 0.5f); emit_f32(0x0A2C, 0.0f);
    emit_f32(0x0A30, 256.0f); emit_f32(0x0A34, -256.0f); emit_f32(0x0A38, 0.5f); emit_f32(0x0A3C, 0.0f);
}
static void emit_glyph_frame(void)
{
    /* Main surface clear (colour, depth, stencil). */
    emit_surface_setup();
    emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
    emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
    emit(NV4097_CLEAR_SURFACE, 0xF3u);
    emit_vp_and_arrays();
    emit(0x0A74, 0u);                         /* depth test off for the whole scene */
    emit(0x0310, 0u);                         /* blend off */
    emit_texture_unit(15, TEX_FILTER(TEX_NEAREST, TEX_NEAREST),
                      TEX_ADDRESS(WRAP_BORDER, WRAP_BORDER, WRAP_BORDER), 0xFFFFFFFFu);

    /* Glyph cache: whole-cache clear, then per glyph a scissored cell clear (colour + stencil, the last
     * one leaves stencil 0xFF outside the cell's coverage to catch a missing per-cell clear). */
    emit_glyph_surface();
    emit_scissor(0, 0, 512, 512);
    emit(NV4097_SET_COLOR_CLEAR_VALUE, 0x00000000u);
    emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF55u);   /* stencil garbage everywhere */
    emit(NV4097_CLEAR_SURFACE, 0xF3u);
    emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
    static const u32 ops[4][2] = { { GL_INVERT, GL_INVERT }, { GL_INCR_WRAP, GL_DECR_WRAP },
                                   { GL_INCR_WRAP, GL_DECR_WRAP }, { GL_INCR_WRAP, GL_DECR_WRAP } };
    emit(0x1834, 0x0901u);                    /* FRONT_FACE CCW */
    for (u32 c = 0; c < 4; c++) {
        emit_scissor(128u * c, 0, 128u, 128u);
        emit(NV4097_SET_COLOR_CLEAR_VALUE, 0x00000000u);
        emit(NV4097_CLEAR_SURFACE, 0xF2u);              /* colour + stencil, this cell only */
        emit_fan_state(ops[c][0], ops[c][1]);
        if (c == 0) emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_c0.first, g_fan_c0.count, 15);
        if (c == 1) emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_c1.first, g_fan_c1.count, 15);
        if (c == 2) {
            emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_o_out.first, g_fan_o_out.count, 15);
            emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_o_in.first, g_fan_o_in.count, 15);
        }
        if (c == 3) {
            emit(0x034C, 0x00u);                /* back write mask 0: back faces leave stencil alone */
            emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_p_out.first, g_fan_p_out.count, 15);
            emit_draw(RSX_PRIMITIVE_TRIANGLE_FAN, g_fan_p_in.first, g_fan_p_in.count, 15);
        }
        emit_cover_state();
        emit_draw(RSX_PRIMITIVE_QUADS, g_cover[c].first, 4, 15);
    }
    emit(0x0328, 0u);                         /* stencil off */

    /* Main pass: show the cache texture (unit 1, local memory, linear A8R8G8B8 512x512, point/clamp). */
    emit_surface_setup();
    const u32 b = 1u * 0x20u;
    emit(NV4097_SET_TEXTURE_OFFSET + b,       GLYPH_OFFSET);
    emit(NV4097_SET_TEXTURE_FORMAT + b,       1u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_ADDRESS + b,      TEX_ADDRESS(WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE, WRAP_CLAMP_TO_EDGE));
    emit(NV4097_SET_TEXTURE_CONTROL0 + b,     TEX_CONTROL0(12u << 8, 0u));
    emit(NV4097_SET_TEXTURE_CONTROL1 + b,     0xAAE4u);
    emit(NV4097_SET_TEXTURE_FILTER + b,       TEX_FILTER(TEX_NEAREST, TEX_NEAREST));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT + b,   (512u << 16) | 512u);
    emit(NV4097_SET_TEXTURE_BORDER_COLOR + b, 0u);
    emit_draw(RSX_PRIMITIVE_QUADS, g_show.first, 4, 1);
}

int main(int argc, char** argv)
{
    long max_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--live")) g_live = 1;
        else if (!strcmp(argv[i], "--glyph")) g_live = g_glyph = 1;
        else max_frames = strtol(argv[i], NULL, 10);
    }
    if (g_live) _putenv("RSX_LIVE_DRAW=1");   /* before the first rsx_live_draw_enabled() call */

    if (vm_reserve() != 0) { printf("[host_kz3] 4 GB guest reservation failed\n"); return 1; }
    printf("[host_kz3] guest reserved: vm_base=%p (4 GB, aligned, commit on touch)\n", (void*)vm_base);

    if (g_live) {
        if (rsx_null_backend_init(1280, 720, "ps3recomp host (live-draw)") != 0 ||
            rsx_live_draw_init(rsx_null_backend_get_hwnd(), 1280, 720, live_guest_ptr, NULL) != 0) {
            printf("[host_kz3] live-draw init failed\n"); return 1;
        }
        rsx_null_backend_suppress_present(1);
        printf("[host_kz3] live-draw engine up\n");
    } else if (rsx_d3d12_backend_init(1280, 720, "ps3recomp host (cellGcm bridge)") != 0) {
        printf("backend init failed\n"); return 1;
    }

    /* Bring up the GCM context as a guest's _cellGcmInitBody would (runs cellGcmInit inside). */
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    printf("[host_kz3] gcm context data @ 0x%08X (%ld guest pages committed so far)\n", cdata, (long)g_commits);
    cellGcmSetDisplayBuffer(0, DISPLAY_OFFSET, SURF_PITCH, 1280, 720);

    uint32_t ctrl_ea = (uint32_t)(uintptr_t)cellGcmGetControlRegister();   /* guest EA, big-endian fields */
    upload_guest_data();
    if (g_glyph) upload_glyph_data();
    printf("[host_kz3] control register at guest 0x%08X\n", ctrl_ea);
    printf("[host_kz3] Expect four quads: gradient tiles (linear/wrap), quadrants (point/clamp), "
           "4x4 cells (point/wrap, unit 9), quadrants in a white frame (point/border, unit 15).\n");

    long frame = 0, flips = 0;
    g_head = 0;
    for (;;) {
        if ((g_live ? rsx_null_backend_pump_messages() : rsx_d3d12_backend_pump_messages()) != 0) break;
        if (g_head + 0x2000u > RING_LIMIT) {          /* wrap: JUMP back to IO 0 (a frame is < 0x2000 bytes) */
            emit_word(0x20000000u | 0u);
            g_head = 0;
        }
        if (g_glyph) emit_glyph_frame(); else emit_frame();
        guest_w32(cdata + 0x8, IO_ADDR + g_head);     /* context->current, as the title's inline gcm would */
        vm_write32(ctrl_ea + 0, g_head);              /* put */
        cellGcm_rsx_process_fifo();
        cellGcmSetFlipCommand(0);
        cellGcmTickVBlank();
        cellGcmTickFlip();
        if (cellGcm_take_flip_pending_synced()) flips++;
        if (g_live) rsx_live_draw_present(0); else rsx_d3d12_backend_present();
        frame++;
        if (frame == 1 || frame == 60 || (frame % 600) == 0)
            printf("[host_kz3] frame %ld: %ld flips synced, get=0x%X put=0x%X, %ld guest pages committed\n",
                   frame, flips, vm_read32(ctrl_ea + 4), vm_read32(ctrl_ea + 0), (long)g_commits);
        if (max_frames > 0 && frame >= max_frames) break;
    }

    printf("[host_kz3] exiting after %ld frames\n", frame);
    cellGcmTerminate();
    if (!g_live) rsx_d3d12_backend_shutdown();
    VirtualFree(vm_base, 0, MEM_RELEASE);
    return 0;
}
