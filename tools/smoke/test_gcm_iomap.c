/* Patch 2019 host test: many cellGcmMapMainMemory chunks (Killzone 3 maps 38 x 1 MB at level load).
 *
 * On a real 4 GB guest reservation (as test_gcm_reports.c), with no graphics backend:
 *   - 40 MapMainMemory calls of 1 MB at EA 0x20000000.. all return CELL_OK with distinct, 1 MB aligned IO offsets;
 *   - cellGcmAddressToOffset / cellGcmIoOffsetToAddress round-trip an address inside every chunk;
 *   - cellGcmResolveLocated(main, offset), the renderer's vertex/texture/surface translation, hits every chunk's EA;
 *   - UnmapIoAddress frees a slot that a further MapMainMemory reuses.
 * Built twice by tools/smoke/build-host.sh: test_gcm_iomap.exe (patched, expect PASS) and test_gcm_iomap_16.exe
 * (cellGcmSys.c with -DCELL_GCM_MAX_IO_MAPPINGS=16, the old limit, expect FAIL). Run under Proton with
 * tools/proton-test.sh; exit 0 = all checks pass.
 */
#include <windows.h>
#include "cellGcmSys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern u32 cellGcmResolveLocated(int location, u32 offset);   /* rsx_vertex_fetch.h; 0 = main */

uint8_t* vm_base = NULL;

#define VM_TOTAL   0x100000000ull
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u
#define CHUNK_EA   0x20000000u
#define N_CHUNKS   40

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
static uint32_t gr32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t g_heap = HEAP_BASE;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap; g_heap += size; return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }


static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (vm_reserve() != 0) { printf("[FAIL] 4 GB guest reservation\n"); return 1; }
    printf("[test_gcm_iomap] CELL_GCM_MAX_IO_MAPPINGS=%d\n", CELL_GCM_MAX_IO_MAPPINGS);
    u32 ctx_out = host_alloc(4, 4);
    cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    u32 out = host_alloc(4, 4);

    u32 io[N_CHUNKS]; int ok = 0, distinct = 1, aligned = 1;
    for (int i = 0; i < N_CHUNKS; i++) {
        gw32(out, 0xFFFFFFFFu);
        s32 r = cellGcmMapMainMemory(CHUNK_EA + (u32)i * 0x100000u, 0x100000u, (u32*)(uintptr_t)out);
        io[i] = r == CELL_OK ? gr32(out) : 0xFFFFFFFFu;
        if (r == CELL_OK) ok++;
        if (io[i] != 0xFFFFFFFFu && (io[i] & 0xFFFFFu)) aligned = 0;
        for (int k = 0; k < i; k++) if (io[i] != 0xFFFFFFFFu && io[k] == io[i]) distinct = 0;
    }
    CHECK(ok == N_CHUNKS, "MapMainMemory: %d/%d chunks of 1 MB mapped", ok, N_CHUNKS);
    CHECK(distinct && aligned, "IO offsets distinct and 1 MB aligned (first 0x%X, last 0x%X)", io[0], io[N_CHUNKS - 1]);

    int rt = 0;
    for (int i = 0; i < N_CHUNKS; i++) {
        u32 ea = CHUNK_EA + (u32)i * 0x100000u + 0x47700u, o = 0, back = 0;   /* KZ3's vertex offset low bits */
        gw32(out, 0); if (cellGcmAddressToOffset(ea, (u32*)(uintptr_t)out) == CELL_OK) o = gr32(out);
        gw32(out, 0); if (cellGcmIoOffsetToAddress(o, (u32*)(uintptr_t)out) == CELL_OK) back = gr32(out);
        if (io[i] != 0xFFFFFFFFu && o == io[i] + 0x47700u && back == ea) rt++;
    }
    CHECK(rt == N_CHUNKS, "AddressToOffset/IoOffsetToAddress round trip in %d/%d chunks", rt, N_CHUNKS);

    /* The renderer's main-memory translation (vertex fetch, textures and surfaces: rsx_draw_engine.c
     * eng_guest_ptr -> cellGcmResolveLocated), for an offset in every chunk. GET_REPORT offsets are 24-bit,
     * so the walker's report path cannot reach chunks past 16 MB of IO space and is not the probe here. */
    int rl = 0;
    for (int i = 0; i < N_CHUNKS; i++)
        if (io[i] != 0xFFFFFFFFu && cellGcmResolveLocated(0, io[i] + 0x47700u) == CHUNK_EA + (u32)i * 0x100000u + 0x47700u) rl++;
    CHECK(rl == N_CHUNKS, "cellGcmResolveLocated(main, IO offset) hits the chunk EA in %d/%d chunks", rl, N_CHUNKS);

    /* Slot reuse after an unmap. */
    s32 ur = cellGcmUnmapIoAddress(io[5]);
    gw32(out, 0xFFFFFFFFu);
    s32 mr = cellGcmMapMainMemory(CHUNK_EA + N_CHUNKS * 0x100000u, 0x100000u, (u32*)(uintptr_t)out);
    CHECK(ur == CELL_OK && mr == CELL_OK, "unmap chunk 5 (0x%X) then map one more: 0x%X / 0x%X -> IO 0x%X", io[5],
          (u32)ur, (u32)mr, gr32(out));

    printf("[test_gcm_iomap] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
