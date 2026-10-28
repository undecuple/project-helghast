/* Patch 1027 host test: GCM_SET_USER_COMMAND (method 0xEB00 = subchannel 7 / 0x0B00) in the FIFO walker reaches the
 * handler registered with the HLE cellGcmSetUserHandler.
 *
 * Same setup as test_gcm_reports.c: a real 4 GB guest reservation, cellGcmSetupContext, one ring through
 * cellGcm_rsx_process_fifo, no graphics backend. The handler is a host stand-in: g_ps3_guest_caller records every
 * (opd, arg) the HLE pump delivers; a host thread runs ppu_gcm_pump in a loop, like the game's HLE boundaries do.
 * Ring: 0x0004E000 0xCAFEBABE (subchannel 7 SET_OBJECT, must not reach the handler), 0x0004EB00 0x12345678,
 * 0x0004EB00 0x9ABCDEF0. Checks: the handler gets 0x12345678 then 0x9ABCDEF0, exactly once each, nothing else.
 * Without patch 1027 the walker only raises the lv2 USER_CMD event (no ISR queue here, as in KZ3) and the handler
 * gets nothing. Usage: test_gcm_usercmd.exe (exit 0 = PASS). Built by tools/smoke/build-host.sh (also as
 * test_gcm_usercmd_no1027.exe against the patch series minus 1027), run under Proton with tools/proton-test.sh.
 */
#include <windows.h>
#include "cellGcmSys.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void ppu_gcm_pump(void);

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

static uint32_t g_heap = HEAP_BASE;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap; g_heap += size; return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static uint32_t g_head;
static void emit_raw(uint32_t hdr, uint32_t d) { gw32(IO_ADDR + g_head, hdr); gw32(IO_ADDR + g_head + 4, d); g_head += 8; }

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

#define HANDLER_OPD 0x00010000u   /* never dereferenced: the stand-in caller below records instead of running guest code */
#define MAX_CALLS 16
static volatile LONG g_ncalls;
static uint32_t g_opd[MAX_CALLS], g_arg[MAX_CALLS];
static void stand_in_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    LONG n = InterlockedIncrement(&g_ncalls) - 1;
    if (n < MAX_CALLS) { g_opd[n] = opd; g_arg[n] = (uint32_t)a0; }
}
static volatile LONG g_stop;
static DWORD WINAPI pump_thread(LPVOID p) { (void)p; while (!g_stop) { ppu_gcm_pump(); Sleep(0); } return 0; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (vm_reserve() != 0) { printf("[FAIL] 4 GB guest reservation\n"); return 1; }
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    printf("[test_gcm_usercmd] context 0x%08X control 0x%08X\n", cdata, ctrl);

    g_ps3_guest_caller = stand_in_caller;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)HANDLER_OPD);
    HANDLE th = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);

    g_head = 0;
    emit_raw(0x0004E000u, 0xCAFEBABEu);                 /* subchannel 7 method 0: SET_OBJECT */
    emit_raw(0x0004EB00u, 0x12345678u);                 /* GCM_SET_USER_COMMAND */
    emit_raw(0x0004EB00u, 0x9ABCDEF0u);                 /* a second one, issued back to back */
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl + 0, g_head);                       /* put */
    cellGcm_rsx_process_fifo();
    for (int i = 0; i < 200 && g_ncalls < 2; i++) Sleep(1);   /* let the pump deliver the last one */
    Sleep(50);                                                /* and catch late duplicates */
    g_stop = 1; WaitForSingleObject(th, 2000);
    ppu_gcm_pump();

    printf("[test_gcm_usercmd] get=0x%X put=0x%X handler calls=%ld\n", vm_read32(ctrl + 4), vm_read32(ctrl + 0),
           (long)g_ncalls);
    for (LONG i = 0; i < g_ncalls && i < MAX_CALLS; i++)
        printf("[test_gcm_usercmd]   call %ld: opd 0x%08X arg 0x%08X\n", (long)i, g_opd[i], g_arg[i]);
    CHECK(vm_read32(ctrl + 4) == g_head, "walker drained the ring (get == put == 0x%X)", g_head);
    CHECK(g_ncalls == 2, "handler called exactly twice (got %ld)", (long)g_ncalls);
    CHECK(g_ncalls >= 1 && g_opd[0] == HANDLER_OPD && g_arg[0] == 0x12345678u, "first call: opd 0x%08X arg 0x12345678",
          HANDLER_OPD);
    CHECK(g_ncalls >= 2 && g_opd[1] == HANDLER_OPD && g_arg[1] == 0x9ABCDEF0u, "second call: opd 0x%08X arg 0x9ABCDEF0",
          HANDLER_OPD);
    int bad = 0;
    for (LONG i = 0; i < g_ncalls && i < MAX_CALLS; i++) bad += g_arg[i] == 0xCAFEBABEu;
    CHECK(!bad, "the subchannel 7 SET_OBJECT (0xE000 0xCAFEBABE) did not reach the handler");

    printf("[test_gcm_usercmd] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
