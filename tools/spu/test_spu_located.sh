#!/usr/bin/env bash
# Patch 1078 host test: a job module lifted a second time at the LS address another job loads it to (KZ3: the Amadeus
# mixer job_0127a180 loads the DSP plugin job_0129bd80 at LS 0x8000) is found by spu_lookup_located() only while
# LS[lsa..+16] holds the module's header and LS[pc..+4] the module's word at pc, only inside [lsa, lsa + span), only
# from its own image (exact, no wildcard), and not at all with KZ3_SPU_LOC=0. Compiles the kz3-loc block of the patched spu_channels.c with stubs and checks
# that spu_indirect_branch, the drain resume and the interpreter's rejoin probe call it. Self-contained: no game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_located.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
C="$P/runtime/spu/spu_channels.c"
[[ -f "$C" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
SNIP="$(sed -n '/kz3-loc begin/,/kz3-loc end/p' "$C" | sed '1d;$d')"
[[ -n "$SNIP" ]] || { echo "FAIL: kz3-loc block not found in $C (patch 1078 missing)"; exit 1; }
fail=0
grep -q 'fn = spu_lookup_located(ctx, ctx->pc);' "$C" ||   # patch 1123: after the shared resolver, outside resident code spans
  { echo "FAIL: spu_indirect_branch does not call spu_lookup_located"; fail=1; }
grep -q 'fn = spu_lookup_located(ctx, pc);' "$P/runtime/spu/spu_drain.c" ||
  { echo "FAIL: the drain resume does not call spu_lookup_located"; fail=1; }
grep -q 'spu_lifted_fn f = spu_lookup_located(ctx, pc);' "$P/runtime/spu/spu_interp.c" ||
  { echo "FAIL: the interpreter rejoin does not call spu_lookup_located"; fail=1; }
cat > "$O/t_spu_located.c" <<C
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SPU_LS_SIZE 0x40000u
#define SPU_LS_MASK 0x3FFFFu
typedef struct { uint8_t ls[SPU_LS_SIZE]; } spu_context;
typedef void (*spu_fn)(spu_context*);
static void f_plugin(spu_context* c) { (void)c; }
static void f_other(spu_context* c) { (void)c; }
/* registry stub: image 275 has a function at 0x8030 and 0x13FD0, image 274 (the 0x4000 lift) one at 0x8030 too */
static spu_fn spu_lookup_ovl_exact(uint32_t a, int img)
{
    if (img == 275 && (a == 0x8030 || a == 0x13FD0)) return f_plugin;
    if (img == 274 && a == 0x8030) return f_other;
    return NULL;
}
$SNIP
static const uint8_t k_sig[16] = {0x42,0x2D,0xAA,0x02,0x42,0x94,0xB4,0x82,0x42,0xFD,0x33,0x02,0x42,0x52,0x0B,0x82};
static uint8_t k_mod[78528];   /* the module's bytes: header + a pseudo-random body */
static spu_context ctx;
int main(int argc, char** argv)
{
    int off = argc > 1 && !strcmp(argv[1], "off"), fail = 0;
    uint32_t x = 12345;
    for (unsigned i = 0; i < sizeof k_mod; i++) { x = x * 1103515245u + 12345u; k_mod[i] = (uint8_t)(x >> 16); }
    memcpy(k_mod, k_sig, 16);
    spu_overlay_register_located(k_mod, 0x8000, sizeof k_mod, 275);
    spu_overlay_register_located(k_mod, 0x3F000, 0x2000, 276);   /* span past the end of LS: refused */
    if (off) {
        memcpy(ctx.ls + 0x8000, k_mod, sizeof k_mod);
        if (spu_lookup_located(&ctx, 0x8030)) { printf("FAIL: KZ3_SPU_LOC=0 still dispatches\n"); return 1; }
        printf("KZ3_SPU_LOC=0: no located dispatch\nPASS\n");
        return 0;
    }
    if (spu_lookup_located(&ctx, 0x8030)) { printf("FAIL: dispatch without the header in LS\n"); fail = 1; }
    memcpy(ctx.ls + 0x8000, k_sig, 16);   /* header only: the body at pc is other code */
    if (spu_lookup_located(&ctx, 0x8030)) { printf("FAIL: dispatch with the header but other code at pc\n"); fail = 1; }
    memcpy(ctx.ls + 0x8000, k_mod, sizeof k_mod);
    if (spu_lookup_located(&ctx, 0x8030) != f_plugin) { printf("FAIL: 0x8030 not served by image 275\n"); fail = 1; }
    if (spu_lookup_located(&ctx, 0x13FD0 | 0x40000) != f_plugin) { printf("FAIL: pc not masked\n"); fail = 1; }
    if (spu_lookup_located(&ctx, 0x4D70)) { printf("FAIL: pc below the load address served\n"); fail = 1; }
    if (spu_lookup_located(&ctx, 0x8000 + 78528)) { printf("FAIL: pc past the span served\n"); fail = 1; }
    if (spu_lookup_located(&ctx, 0x8034)) { printf("FAIL: an address with no function served\n"); fail = 1; }
    ctx.ls[0x8005] ^= 1;   /* another module now at 0x8000 */
    if (spu_lookup_located(&ctx, 0x8030)) { printf("FAIL: dispatch with a different header\n"); fail = 1; }
    printf("patch 1078: header + range + exact image checks%s\n", fail ? " -- NO" : "");
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
C
CC="$(command -v cc || command -v gcc || command -v clang)"
[[ -n "$CC" ]] || { echo "SKIP: no C compiler"; exit 0; }
"$CC" -std=c11 -O1 -Wall -Wno-unused-function -o "$O/t_spu_located" "$O/t_spu_located.c" 2>&1 | head -20
[[ -x "$O/t_spu_located" ]] || { echo "FAIL: build"; exit 1; }
"$O/t_spu_located" | grep -v '^\[spu-loc\]' || fail=1
KZ3_SPU_LOC=0 "$O/t_spu_located" off || fail=1
[[ $fail == 0 ]] && echo "test_spu_located: PASS" || echo "test_spu_located: FAIL"
exit $fail
