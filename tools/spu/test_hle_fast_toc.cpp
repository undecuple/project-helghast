/* Patch 1145 host test (J-Stars port patch 0004): ps3_hle_call's fast forms (ps3_hle_register_fast, the sys_lwmutex_lock/unlock
 * fast paths) return before _TocGuard, the only code that stored r2 to 0x28(r1). A guest caller does `ld r2,40(r1)` after an
 * import (the glink stub would have saved r2 there), so it reloaded whatever stale value the frame held.
 * Fake caller: stale value in the slot, call a fast NID, reload r2 from the slot as the caller would. Also checks the
 * full (non-fast) path and a fast form that declines (returns 0) still end with slot == r2. Exit 0 = pass.
 * Builds ppu_hle.cpp itself with stubs (tools/spu/test_hle_fast_toc.sh). */
#include "ppu_recomp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint8_t g_mem[1u << 16];
uint8_t* vm_base = g_mem;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

/* guest memory is big-endian */
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return __builtin_bswap64(v); }
void vm_write64(uint64_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_mem + a, &v, 8); }
extern "C" uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return __builtin_bswap32(v); }
extern "C" void vm_write32(uint64_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_mem + a, &v, 4); }

/* link stubs for what ppu_hle.cpp references but the paths under test never reach */
extern "C" {
int g_kz3_wacct = 0; const char* (*g_kz3_wacct_hle_name)(uint32_t);
int kz3_wacct_init(void) { return 0; } uint64_t kz3_wacct_begin_slow(void) { return 0; } void kz3_wacct_end(uint64_t, int, uint32_t, uint32_t) {}
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea;
void ppu_dump_bctrl_ring(uint32_t, const char*) {} void ppu_dump_guest_stack(ppu_context*, const char*) {}
uint32_t ppu_guest_call(...) { return 0; } void ppu_guest_callstack(...) {} uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ppu_prof_stamp(...) {} void* prx_resolve_export(...) { return 0; } void ps3_indirect_call(...) {} void ps3_msf(...) {}
void ps3_hle_register(uint32_t, const char*, void*);
void ps3_hle_register_fast(uint32_t, int (*)(ppu_context*));
void ps3_hle_call(uint32_t, ppu_context*);
}
static int fast_ok(ppu_context* c) { c->gpr[3] = 0; c->gpr[2] = 0x6580E15Eull; return 1; }   /* even a fast form may clobber r2 */
static int fast_decline(ppu_context*) { return 0; }
static uint64_t slow_h(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { return 7; }   /* generic 8-arg HLE form */

static void caller(uint32_t nid, const char* what)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    const uint64_t TOC = 0x0139FE40ull, SP = 0x8000;
    ctx.gpr[1] = SP; ctx.gpr[2] = TOC;
    vm_write64(SP + 0x28, 0x6580E15Eull);          /* stale frame content */
    ps3_hle_call(nid, &ctx);
    const uint64_t slot = vm_read64(SP + 0x28);
    CHECK(slot == TOC, "%s: 0x28(r1) = 0x%llx after the import, caller r2 was 0x%llx (ld r2,40(r1) would reload garbage)",
          what, (unsigned long long)slot, (unsigned long long)TOC);
}
int main()
{
    ps3_hle_register_fast(0x11111111u, fast_ok);
    ps3_hle_register_fast(0x22222222u, fast_decline);
    ps3_hle_register(0x22222222u, "decline_full", (void*)slow_h);
    ps3_hle_register(0x33333333u, "slow_only", (void*)slow_h);
    caller(0x11111111u, "fast form that handles the call");
    caller(0x22222222u, "fast form that declines, full handler runs");
    caller(0x33333333u, "full handler only");
    printf(g_fail ? "FAIL\n" : "ALL OK\n");
    return g_fail;
}
