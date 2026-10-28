/* Patch 2013 host test: NV4097_SET_CONTEXT_DMA_REPORT (0x1A8) + NV4097_GET_REPORT (0x1800) in the FIFO walker.
 *
 * Builds a small command ring on a real 4 GB guest reservation (as host_kz3.c), runs it through
 * cellGcm_rsx_process_fifo without any graphics backend, and checks where each report lands:
 *   - default context (0xFEED0000, local): host report slot (cellGcmGetReportDataAddress), guest memory untouched;
 *   - 0xFEED0001 (main): 16 bytes big-endian at the IO offset's guest EA: u64 timestamp, u32 value
 *     (ZPASS type 1 = 0xFFFF "visible", other types 0), u32 0; timestamps strictly rising;
 *   - an unmapped main offset is skipped (no crash), and switching back to 0xFEED0000 restores the local path.
 * Usage: test_gcm_reports.exe  (exit code 0 = all checks pass). Built by tools/smoke/build-host.sh, run under
 * Proton with tools/proton-test.sh.
 */
#include <windows.h>
#include "cellGcmSys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void cellGcm_rsx_process_fifo(void);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);

uint8_t* vm_base = NULL;

#define VM_TOTAL   0x100000000ull
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u

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
static uint64_t gr64(uint32_t ea) { return ((uint64_t)gr32(ea) << 32) | gr32(ea + 4); }

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
static void emit(uint32_t m, uint32_t d) { emit_word((1u << 18) | (((m >> 2) & 0x7FFu) << 2)); emit_word(d); }

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

#define SENTINEL 0xA5A5A5A5u
#define LOCAL_OFF  0x07F0u      /* local report: host slot 0x7F, guest IO 0x7F0 must stay as is */
#define MAIN_OFF1  0x50390u     /* KZ3's timestamp pair offset (pass 13 captures) */
#define MAIN_OFF2  0x4D2A0u     /* an occlusion slot */
#define MAIN_OFF3  0x4D2B0u     /* type 2 (ZCULL stats): value 0 */
#define BAD_OFF    0xF00000u    /* IO page 15: not mapped (IO_SIZE is 2 MB) */
#define LOCAL_OFF2 0x0800u

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (vm_reserve() != 0) { printf("[FAIL] 4 GB guest reservation\n"); return 1; }
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    printf("[test_gcm_reports] context 0x%08X control 0x%08X\n", cdata, ctrl);

    const uint32_t offs[] = { LOCAL_OFF, MAIN_OFF1, MAIN_OFF2, MAIN_OFF3, LOCAL_OFF2, MAIN_OFF1 + 0x10 };
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
        for (unsigned k = 0; k < 16; k += 4) gw32(IO_ADDR + offs[i] + k, SENTINEL);

    g_head = 0;
    emit(0x1800, (1u << 24) | LOCAL_OFF);              /* default context: local */
    emit(0x01A8, 0xFEED0001u);                          /* SET_CONTEXT_DMA_REPORT: main memory */
    emit(0x1800, (1u << 24) | MAIN_OFF1);
    emit(0x1800, (1u << 24) | MAIN_OFF2);
    emit(0x1800, (2u << 24) | MAIN_OFF3);
    emit(0x1800, (1u << 24) | BAD_OFF);
    emit(0x1800, (1u << 24) | (MAIN_OFF1 + 0x10));
    emit(0x01A8, 0xFEED0000u);                          /* back to local */
    emit(0x1800, (1u << 24) | LOCAL_OFF2);
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl + 0, g_head);                       /* put */
    cellGcm_rsx_process_fifo();
    printf("[test_gcm_reports] get=0x%X put=0x%X\n", vm_read32(ctrl + 4), vm_read32(ctrl + 0));
    CHECK(vm_read32(ctrl + 4) == g_head, "walker drained the ring (get == put == 0x%X)", g_head);

    CellGcmReportData* r = cellGcmGetReportDataAddress(LOCAL_OFF / 16u);
    CHECK(r && r->value == 0xFFFFu && r->timestamp != 0, "local report: host slot 0x%X value 0x%X ts %llu",
          LOCAL_OFF / 16u, r ? r->value : 0, r ? (unsigned long long)r->timestamp : 0ull);
    CHECK(gr32(IO_ADDR + LOCAL_OFF) == SENTINEL && gr32(IO_ADDR + LOCAL_OFF + 8) == SENTINEL,
          "local report: guest IO 0x%X untouched", LOCAL_OFF);

    uint64_t t1 = gr64(IO_ADDR + MAIN_OFF1), t2 = gr64(IO_ADDR + MAIN_OFF2), t3 = gr64(IO_ADDR + MAIN_OFF3);
    uint64_t t4 = gr64(IO_ADDR + MAIN_OFF1 + 0x10);
    CHECK(gr32(IO_ADDR + MAIN_OFF1 + 8) == 0xFFFFu && gr32(IO_ADDR + MAIN_OFF1 + 12) == 0u && t1 != 0 &&
          t1 != ((uint64_t)SENTINEL << 32 | SENTINEL),
          "main report @IO 0x%X: ts %llu value 0x%X pad 0x%X", MAIN_OFF1, (unsigned long long)t1,
          gr32(IO_ADDR + MAIN_OFF1 + 8), gr32(IO_ADDR + MAIN_OFF1 + 12));
    CHECK(gr32(IO_ADDR + MAIN_OFF2 + 8) == 0xFFFFu && gr32(IO_ADDR + MAIN_OFF2 + 12) == 0u,
          "main report @IO 0x%X: value 0x%X (ZPASS visible)", MAIN_OFF2, gr32(IO_ADDR + MAIN_OFF2 + 8));
    CHECK(gr32(IO_ADDR + MAIN_OFF3 + 8) == 0u && gr32(IO_ADDR + MAIN_OFF3 + 12) == 0u,
          "main report @IO 0x%X: type 2 value 0x%X", MAIN_OFF3, gr32(IO_ADDR + MAIN_OFF3 + 8));
    CHECK(t1 < t2 && t2 < t3 && t3 < t4, "timestamps strictly rising: %llu < %llu < %llu < %llu",
          (unsigned long long)t1, (unsigned long long)t2, (unsigned long long)t3, (unsigned long long)t4);
    CHECK(cellGcmGetReportDataAddress(MAIN_OFF1 / 16u)->timestamp == 0, "main report did not touch host slot 0x%X",
          MAIN_OFF1 / 16u);
    r = cellGcmGetReportDataAddress(LOCAL_OFF2 / 16u);
    CHECK(r->value == 0xFFFFu && r->timestamp > t4 && gr32(IO_ADDR + LOCAL_OFF2) == SENTINEL,
          "back on 0xFEED0000: host slot 0x%X ts %llu > %llu, guest untouched", LOCAL_OFF2 / 16u,
          (unsigned long long)r->timestamp, (unsigned long long)t4);

    printf("[test_gcm_reports] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
