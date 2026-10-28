#!/usr/bin/env bash
# Host test for KZ3 patch 2088 (fragment WPOS in pixels on antialiased targets, from our Killzone 2 port): takes
# ld_wpos_aa_apply out of the patched rsx_live_draw.c, decompiles small fragment programs with the patched
# rsx_fp_decompiler.c and checks the substitution: 2x2 and 2x1 grids give floor(p.xy / aa) + 0.5 with z/w kept,
# 1x1 / 0 leave the text alone, a perspective-corrected TC read keeps its 1/w, a too-small buffer fails cleanly.
# No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_wpos_aa.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -f "$V/rsx_live_draw.c" || ! -d "$PT/include" ]]; then
  echo "[test_wpos_aa] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q '^static int ld_wpos_aa_apply(' "$V/rsx_live_draw.c"; then
  echo "[test_wpos_aa] FAIL: toolkit without patch 2088 (no ld_wpos_aa_apply)"; exit 1; fi
O="$ROOT/build/smoke-host/wpos_aa"; mkdir -p "$O"
awk '/^static int ld_wpos_aa_apply\(/{on=1} on{print} on&&/^}/{exit}' "$V/rsx_live_draw.c" > "$O/apply.inc"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apply.inc"
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
static char h[65536];
static int fp_mov_input(u32 in)  /* MOV r0, input `in` (END) */
{
    u8 a[16];
    rsx_test_fp_instr(a, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(in) |
                      RSX_TEST_FP_END, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    return rsx_fp_decompile_buffered_ex(a, 16, 0x40, 0, h, sizeof h, NULL) > 0;
}
static size_t count(const char* s, const char* k) { size_t n = 0; for (const char* p = strstr(s, k); p; p = strstr(p + 1, k)) n++; return n; }
int main(void)
{
    if (!fp_mov_input(0)) { puts("FAIL decompile"); return 1; }   /* WPOS */
    const size_t reads = count(h, "input.position");
    check("WPOS read decompiles to input.position", reads >= 1);
    static char save[65536]; strcpy(save, h);
    check("1x1 grid leaves the program alone", ld_wpos_aa_apply(h, sizeof h, 0x0101u) == 0 && !strcmp(h, save));
    check("grid 0 leaves the program alone", ld_wpos_aa_apply(h, sizeof h, 0u) == 0 && !strcmp(h, save));
    check("2x2: every read replaced", ld_wpos_aa_apply(h, sizeof h, 2u | 2u << 8) == (int)reads);
    check("2x2: pixel centre of the sample", strstr(h, "float4(floor(input.position.xy / float2(2.0, 2.0)) + 0.5, input.position.zw)") != NULL);
    check("2x2: no raw read left", count(h, "input.position") == 2u * reads);
    strcpy(h, save);
    check("2x1 (DIAGONAL_CENTERED_2)", ld_wpos_aa_apply(h, sizeof h, 2u | 1u << 8) == (int)reads &&
          strstr(h, "float2(2.0, 1.0)") != NULL);
    /* A perspective-corrected texcoord read divides by input.position.w: the substitution keeps w. */
    char p[256] = "r0 = (input.tc0 * _rsx_inv_w(input.position));";
    check("1/w read keeps w", ld_wpos_aa_apply(p, sizeof p, 0x0202u) == 1 &&
          !strcmp(p, "r0 = (input.tc0 * _rsx_inv_w(float4(floor(input.position.xy / float2(2.0, 2.0)) + 0.5, input.position.zw)));"));
    char small[64] = "x = input.position;";
    check("too-small buffer fails, text unchanged", ld_wpos_aa_apply(small, sizeof small, 0x0202u) == -1 &&
          !strcmp(small, "x = input.position;"));
    if (!fp_mov_input(4)) { puts("FAIL decompile"); return 1; }   /* TC0 only */
    strcpy(save, h);
    check("program without WPOS unchanged", ld_wpos_aa_apply(h, sizeof h, 0x0202u) == 0 && !strcmp(h, save));
    return fails ? 1 : 0;
}
EOC
CC="${CC:-cc}"
W="-std=c11 -Wall -Wno-unused-function -Wno-format-truncation -I$O -I$V -I$PT/include"
$CC $W -O2 "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm
"$O/t" 2>/dev/null
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t_asan" -lm
  "$O/t_asan" >/dev/null 2>&1 && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
echo "[test_wpos_aa] PASS"
