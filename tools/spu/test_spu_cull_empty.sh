#!/usr/bin/env bash
# Host test (SPU pass 31): port/spu/kz3_spu_cull.c, the KZ3_CULL_AB=empty44 entry hook (runtime patch 1044).
# Builds the hook against the patched toolkit's spu_context.h with a fake job-manager local store: job context at
# LS 0x1330 whose +0x20 word points at the job's parameter block; the hook must zero the low 14 bits of halfword 7
# of parameter qword 0 (the occluder count) and keep the two flag bits, touch nothing else, and return 0 (run the
# module). Negative controls: a job the hook does not know (51), a context pointer outside the manager's area, and
# a null parameter pointer leave the store untouched. No game data, no work/ beyond the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_spu_cull_empty.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_context.h" ]] || { echo "SKIP: no patched toolkit at $P (run tools/ps3recomp-patched.sh)"; exit 0; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat > "$T/main.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "kz3_spu_skip.h"
static spu_context* ctx;
static int fails;
uint8_t* vm_base;                                     /* the runtime's guest base; here a 1 MB stand-in */
static uint8_t* guest;
#define CAP_EA 0x00012340u                            /* job 51 descriptor: w1, capacity at w1 + 0x20 */
static void setup51(uint32_t jc, uint32_t prm, uint32_t w1, uint32_t cap)
{
    spu_context_init(ctx, 0);
    memset(ctx->ls, 0xA5, 0x4000);
    if (jc < 0x4000) spu_ls_write32(ctx, jc + 0x20, prm);
    if (prm && prm < 0x4000) {
        spu_ls_write32(ctx, prm + 0, 0x40120000u);     /* w0 view data, w1 descriptor EA, w2/w3 whatever */
        spu_ls_write32(ctx, prm + 4, w1);
        spu_ls_write32(ctx, prm + 8, 0x11223344u);
        spu_ls_write32(ctx, prm + 12, 0x55667788u);
    }
    ctx->gpr[3]._u32[0] = jc;
    memset(guest, 0x5A, 0x100000);
    guest[CAP_EA + 0x20] = cap >> 24; guest[CAP_EA + 0x21] = cap >> 16; guest[CAP_EA + 0x22] = cap >> 8; guest[CAP_EA + 0x23] = cap;
}
static uint32_t cap_word(void) { return ((uint32_t)guest[CAP_EA + 0x20] << 24) | ((uint32_t)guest[CAP_EA + 0x21] << 16) | ((uint32_t)guest[CAP_EA + 0x22] << 8) | guest[CAP_EA + 0x23]; }
static int guest_untouched_except_cap(void)
{
    for (uint32_t a = 0; a < 0x100000; a++) { if (a >= CAP_EA + 0x20 && a < CAP_EA + 0x24) continue; if (guest[a] != 0x5A) return 0; }
    return 1;
}
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void setup(uint32_t jc, uint32_t prm, unsigned hw7)
{
    spu_context_init(ctx, 0);
    memset(ctx->ls, 0xA5, 0x4000);                      /* manager area: a pattern so stray writes show */
    if (jc < 0x4000) spu_ls_write32(ctx, jc + 0x20, prm);
    if (prm && prm < 0x4000) {
        spu_ls_write32(ctx, prm + 0, 0x00301234u);     /* hw0 size, w1 output EA, w2 ?, w3 ?:hw7 */
        spu_ls_write32(ctx, prm + 4, 0x40123480u);
        spu_ls_write32(ctx, prm + 8, 0x11223344u);
        spu_ls_write32(ctx, prm + 12, 0x55660000u | hw7);
    }
    ctx->gpr[3]._u32[0] = jc;
}
static unsigned hw7(uint32_t prm) { return spu_ls_read32(ctx, prm + 12) & 0xFFFFu; }
static int untouched_except(uint32_t prm, int allow_hw7)
{
    for (uint32_t a = 0; a < 0x4000; a++) {
        if (a == 0x1350 || a == 0x1351 || a == 0x1352 || a == 0x1353) continue;   /* our own setup words */
        if (prm && a >= prm && a < prm + 16) {
            if (allow_hw7 && (a == prm + 14 || a == prm + 15)) continue;
            continue;   /* parameter words are checked by value */
        }
        if (ctx->ls[a] != 0xA5) return 0;
    }
    return 1;
}
int main(void)
{
    ctx = calloc(1, sizeof *ctx);
    int r;
    /* 1. the real shape: ctx 0x1330, params at 0xC20, count 10 with flag bits 0b10 */
    setup(0x1330, 0xC20, 0x800Au);
    r = kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(r == 0, "returns 0 (run the module), got %d", r);
    CHECK(hw7(0xC20) == 0x8000u, "count zeroed, flags kept: hw7=0x%04X", hw7(0xC20));
    CHECK(spu_ls_read32(ctx, 0xC20) == 0x00301234u && spu_ls_read32(ctx, 0xC24) == 0x40123480u &&
          spu_ls_read32(ctx, 0xC28) == 0x11223344u && (spu_ls_read32(ctx, 0xC2C) >> 16) == 0x5566u,
          "other parameter words untouched");
    CHECK(untouched_except(0xC20, 1), "nothing else in the manager area written");
    /* 2. count already 0 with flags 0b11: stays 0xC000 */
    setup(0x1330, 0xC20, 0xC000u);
    kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(hw7(0xC20) == 0xC000u, "zero count stays, flags kept: 0x%04X", hw7(0xC20));
    /* 3. max count, no flags */
    setup(0x1330, 0xC20, 0x3FFFu);
    kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(hw7(0xC20) == 0u, "max count zeroed: 0x%04X", hw7(0xC20));
    /* negative controls */
    setup(0x1330, 0xC20, 0x800Au);
    r = kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)51);          /* job 51 with no guest memory: left alone */
    CHECK(r == 0 && hw7(0xC20) == 0x800Au && untouched_except(0xC20, 0), "job 51 without vm_base: untouched (hw7=0x%04X)", hw7(0xC20));
    setup(0x1330, 0xC20, 0x800Au);
    r = kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)43);          /* a job the hook does not know */
    CHECK(r == 0 && hw7(0xC20) == 0x800Au && untouched_except(0xC20, 0), "job 43: untouched (hw7=0x%04X)", hw7(0xC20));
    /* pass 32: empty51 zeroes the capacity u32 at (param w1 + 0x20) in guest memory, nothing else anywhere */
    guest = calloc(1, 0x100000); vm_base = guest;
    setup51(0x1330, 0xC20, CAP_EA, 0x12345678u);
    r = kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)51);
    CHECK(r == 0 && cap_word() == 0 && guest_untouched_except_cap(), "empty51: capacity 0x12345678 -> 0x%08X, rest of guest memory kept", cap_word());
    CHECK(spu_ls_read32(ctx, 0xC24) == CAP_EA && untouched_except(0xC20, 0) && spu_ls_read32(ctx, 0xC20) == 0x40120000u,
          "empty51: LS parameters and manager area untouched");
    setup51(0x1330, 0xC20, CAP_EA, 0u);
    kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)51);
    CHECK(cap_word() == 0 && guest_untouched_except_cap(), "empty51: zero capacity stays zero");
    setup51(0x1330, 0xC20, 0x00008000u, 0x12345678u);                /* descriptor EA below 0x10000: refused */
    kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)51);
    CHECK(cap_word() == 0x12345678u && guest_untouched_except_cap(), "empty51: w1 below 0x10000 left alone");
    setup51(0x1330, 0xC20, CAP_EA + 2, 0x12345678u);                 /* unaligned descriptor EA: refused */
    kz3_cull_empty_entry(ctx, 250, (void*)(intptr_t)51);
    CHECK(cap_word() == 0x12345678u && guest_untouched_except_cap(), "empty51: unaligned w1 left alone");
    setup51(0x1330, 0xC20, CAP_EA, 0x12345678u);                     /* job 44's hook never touches guest memory */
    kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(cap_word() == 0x12345678u && guest_untouched_except_cap() && (spu_ls_read32(ctx, 0xC2C) & 0x3FFFu) == 0,
          "job 44 on the same setup: guest memory kept, its own count zeroed");
    vm_base = NULL;
    setup(0x5000, 0xC20, 0x800Au);           /* context pointer inside the module buffer: not a manager call */
    r = kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(r == 0 && hw7(0xC20) == 0x800Au, "ctx >= 0x4000: untouched (hw7=0x%04X)", hw7(0xC20));
    setup(0x1330, 0, 0);                     /* null parameter pointer */
    r = kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(r == 0 && untouched_except(0, 0), "null params: untouched");
    setup(0x1330, 0x4800, 0x800Au);          /* parameter pointer inside the module buffer */
    memset(ctx->ls + 0x4800, 0xA5, 16);
    r = kz3_cull_empty_entry(ctx, 220, (void*)(intptr_t)44);
    CHECK(r == 0 && ctx->ls[0x480E] == 0xA5 && ctx->ls[0x480F] == 0xA5, "params >= 0x4000: untouched");
    printf("%s: %d check group(s), %d failure(s)\n", fails ? "FAIL" : "PASS", 16, fails);
    return fails ? 1 : 0;
}
C
gcc -std=gnu17 -O1 -w -I "$ROOT/port/spu" -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    "$T/main.c" "$ROOT/port/spu/kz3_spu_cull.c" -o "$T/t"
"$T/t" 2>&1
