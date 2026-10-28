/* Host test, first for patch 2060 (dropped in v0.15.0: upstream PR #324 has the same function):
 * cellFontInitializeWithRevision initialises the font library (OpenFontset refused before, works after, a second
 * init is ALREADY_INITIALIZED). The registration is upstream's NID generator now (checked by the .sh with upstream's
 * tools/test_font_hle_nids.py); the ctx-handler table below stays as a stub for anything cellFont.c still registers.
 * No game data, no fonts. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ps3emu/ps3types.h"
#include "ps3emu/nid.h"
#include "../../runtime/ppu/ppu_context.h"
#include "cellFont.h"

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t a)
{
    (void)a;
}
void ps3_ww_report_inline(uint32_t a, uint64_t v, int n)
{
    (void)a;
    (void)v;
    (void)n;
}
int spu_coh_is_reserved(uint32_t a)
{
    (void)a;
    return 0;
}
void spu_coh_notify_write(uint32_t a)
{
    (void)a;
}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
typedef void (*ctxfn)(ppu_context*);
static struct {
    uint32_t nid;
    ctxfn fn;
} s_reg[16];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, ctxfn fn)
{
    (void)name;
    if (s_nreg < 16) {
        s_reg[s_nreg].nid = nid;
        s_reg[s_nreg++].fn = fn;
    }
}

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)
static void wr32(uint32_t a, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + a, &v, 4);
}

int main(void)
{
    static uint8_t mem[1 << 20];
    vm_base = mem;
    ppu_vm_size = sizeof mem;
    const uint32_t CFG = 0x1000, TYPE = 0x1100, FONT = 0x1200;
    wr32(TYPE, 0x109);
    wr32(TYPE + 4, 1);

    /* Before init: the library refuses, as a title stub that only returned CELL_OK left it (the in-game bug). */
    CHECK(cellFontOpenFontset(0, (CellFontType*)(uintptr_t)TYPE, (CellFont*)(uintptr_t)FONT) ==
              CELL_FONT_ERROR_UNINITIALIZED,
          "OpenFontset before init should be UNINITIALIZED");

    /* Since ps3recomp v0.15.0 (PR #324) the export is a plain function that upstream's NID generator
     * (tools/gen_hle_nids.py) registers; test_font_init2060.sh runs upstream's check of that registration.
     * Here: the NID, and the call as the generic binding makes it (r3 = revision flags, r4 = config EA). */
    CHECK(ps3_compute_nid("cellFontInitializeWithRevision") == 0xF03DCC29u, "NID");
    {
        s32 r = cellFontInitializeWithRevision(0x0000000F00000000ull, (const CellFontConfig*)(uintptr_t)CFG);
        CHECK(r == 0, "InitializeWithRevision -> 0x%x", (unsigned)r);
        CHECK(cellFontOpenFontset(0, (CellFontType*)(uintptr_t)TYPE, (CellFont*)(uintptr_t)FONT) == CELL_OK,
              "OpenFontset after init");
        CHECK(cellFontSetScalePixel((CellFont*)(uintptr_t)FONT, 2048.0f, 2048.0f) == CELL_OK,
              "SetScalePixel after init");
        r = cellFontInitializeWithRevision(0, (const CellFontConfig*)(uintptr_t)CFG);
        CHECK(r == CELL_FONT_ERROR_ALREADY_INITIALIZED, "second init -> 0x%x", (unsigned)r);
    }
    printf("RESULT: %s (%d failure(s), %d ctx handler(s) registered)\n", fails ? "FAIL" : "PASS", fails, s_nreg);
    return fails ? 1 : 0;
}
