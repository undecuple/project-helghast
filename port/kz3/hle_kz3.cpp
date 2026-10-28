/* Killzone 3 port: HLE for the imports ps3recomp leaves unresolved for KZ3.
 *
 * Names, call sites and expected behaviour: the RE notes, "Unresolved imports under
 * Proton" (RE notes). The four names were checked against ps3recomp's NID algorithm (SHA-1 of name + suffix).
 * Each handler does what KZ3's call site needs today; the full libraries are later work (noted per handler). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppu_recomp.h"     /* ppu_context */

extern "C" uint8_t* vm_base;
typedef void (*hle_ctx_fn)(ppu_context*);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
#ifdef KZ3_WITH_NET
extern "C" void kz3_net_register(void);   /* port/net/kz3_net.c: online setup, inert unless KZ3_ONLINE is set */
#endif

namespace {

/* ASSUMPTION: guest scratch for port-made objects, just past the runtime's 0x8000-byte HLE window at 0x20000000
 * (KZ3's own heap starts at 0x40000000; its GCM IO map at 0x40600000). Demand-committed, reads as zero. */
constexpr uint32_t KZ3_HLE_SCRATCH = 0x20010000u;
constexpr uint32_t FONTFT_LIBRARY  = KZ3_HLE_SCRATCH;          /* fake CellFontLibrary, 0x100 bytes of zeros */

void w32(uint32_t ea, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + ea, &be, 4); }

/* 0xFF0A21B7 cellKbRead(port, CellKbData*): older name of cellKbGetData. KZ3 polls it once a frame after
 * cellKbGetInfo. No keys yet: led = mkey = len = 0. Real keyboard input belongs to the mouse + keyboard feature. */
void cellKbRead(ppu_context* ctx)
{
    uint32_t data = (uint32_t)ctx->gpr[4];
    if (data) { w32(data + 0, 0); w32(data + 4, 0); w32(data + 8, 0); }
    ctx->gpr[3] = 0;
}

/* 0xF03DCC29 cellFontInitializeWithRevision is the runtime's own since patch 2060 (it initialises the library; this
 * file's old stub returned CELL_OK without it, so every cellFont call failed). */

/* 0x7A0A83C4 cellFontInitLibraryFreeTypeWithRevision(u64, CellFontLibraryConfigFT*, const CellFontLibrary**):
 * KZ3 keeps the handle at 0x1331960 and passes it on. A zeroed library object; real glyphs are later work. */
void cellFontInitLibraryFreeTypeWithRevision(ppu_context* ctx)
{
    uint32_t out = (uint32_t)ctx->gpr[5];
    if (out) w32(out, FONTFT_LIBRARY);
    ctx->gpr[3] = 0;
}

/* 0xA7FD2F5B cellCameraSetNotifyEventQueue2(u64 key, u64 source, u64 flag): KZ3 registers for camera events.
 * No camera in this port (PS Eye/Move declined; main.cpp sets PS3_CAMERA=0), so no event will ever be sent. */
void cellCameraSetNotifyEventQueue2(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* 0x94034C95 cellSpurs, name unknown: called once with the taskset right after cellSpursCreateTasksetWithAttribute;
 * KZ3 returns the result to its caller at 0x8A178. ASSUMPTION: a setup/query call that succeeds; CELL_OK. */
void cellSpurs_94034C95(ppu_context* ctx)
{
    static int n = 0;
    if (n++ < 4) fprintf(stderr, "[kz3-hle] cellSpurs NID 0x94034C95(taskset=0x%08X) -> CELL_OK (unnamed, ASSUMPTION)\n",
                         (uint32_t)ctx->gpr[3]);
    ctx->gpr[3] = 0;
}

}  // namespace

/* Called by main.cpp after the runtime's own HLE tables are registered. */
extern "C" void kz3_hle_register(void)
{
    ps3_hle_register_ctx(0xFF0A21B7u, "cellKbRead", cellKbRead);
    ps3_hle_register_ctx(0x7A0A83C4u, "cellFontInitLibraryFreeTypeWithRevision", cellFontInitLibraryFreeTypeWithRevision);
    ps3_hle_register_ctx(0xA7FD2F5Bu, "cellCameraSetNotifyEventQueue2", cellCameraSetNotifyEventQueue2);
    ps3_hle_register_ctx(0x94034C95u, "cellSpurs_94034C95", cellSpurs_94034C95);
#ifdef KZ3_WITH_NET
    kz3_net_register();
#endif
}
