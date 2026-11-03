#!/usr/bin/env bash
# Host test for KZ3 patch 2143 (the Killzone 2 port, patch 2096): fragment DDX / DDY are decompiled (they wrote nothing,
# so the destination kept 0), the program declares _rsx_deriv_scale, and ld_deriv_scale_apply (taken out of the patched
# rsx_live_draw.c) rewrites it for the target's sample grid. No game data, no GPU. SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_deriv.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -f "$V/rsx_live_draw.c" || ! -d "$PT/include" ]]; then
  echo "[test_fp_deriv] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q '^static int ld_deriv_scale_apply(' "$V/rsx_live_draw.c"; then
  echo "[test_fp_deriv] FAIL: toolkit without patch 2143 (no ld_deriv_scale_apply)"; exit 1; fi
O="$ROOT/build/smoke-host/fp_deriv"; mkdir -p "$O"
awk '/^static int ld_deriv_scale_apply\(/{on=1} on{print} on&&/^}/{exit}' "$V/rsx_live_draw.c" > "$O/apply.inc"
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
static int fp_op_input(u32 op, u32 in)  /* OP r0, input `in` (END) */
{
    u8 a[16];
    rsx_test_fp_instr(a, RSX_TEST_FP_OPC(op) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(in) | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    return rsx_fp_decompile_buffered_ex(a, 16, 0x40, 0, h, sizeof h, NULL) > 0;
}
int main(void)
{
    if (!fp_op_input(0x15u, 4)) { puts("FAIL decompile DDX"); return 1; }
    check("DDX emits ddx() scaled", strstr(h, "ddx(") != NULL && strstr(h, "* _rsx_deriv_scale.x") != NULL);
    check("DDX program declares the scale once", strstr(h, RSX_FP_DERIV_SCALE_DECL) != NULL);
    static char save[65536]; strcpy(save, h);
    check("1x1 grid: (1, 1), text same size", ld_deriv_scale_apply(h, sizeof h, 0u) == 1 && strstr(h, "float2(1.0, 1.0)") != NULL);
    strcpy(h, save);
    check("2x2 grid scales both", ld_deriv_scale_apply(h, sizeof h, 2u | 2u << 8) == 1 && strstr(h, "float2(2.0, 2.0)") != NULL);
    strcpy(h, save);
    check("2x1 grid", ld_deriv_scale_apply(h, sizeof h, 2u | 1u << 8) == 1 && strstr(h, "float2(2.0, 1.0)") != NULL);
    char small[80]; strcpy(small, RSX_FP_DERIV_SCALE_DECL);
    check("fits exactly", ld_deriv_scale_apply(small, sizeof small, 4u | 4u << 8) == 1 && strstr(small, "float2(4.0, 4.0)") != NULL);
    strcpy(small, RSX_FP_DERIV_SCALE_DECL);
    check("too-small buffer fails cleanly", ld_deriv_scale_apply(small, sizeof RSX_FP_DERIV_SCALE_DECL, 16u | 16u << 8) == -1 && !strcmp(small, RSX_FP_DERIV_SCALE_DECL));
    if (!fp_op_input(0x16u, 4)) { puts("FAIL decompile DDY"); return 1; }
    check("DDY emits ddy() scaled by y", strstr(h, "ddy(") != NULL && strstr(h, "* _rsx_deriv_scale.y") != NULL);
    if (!fp_op_input(0x01u, 4)) { puts("FAIL decompile MOV"); return 1; }
    strcpy(save, h);
    check("program without a derivative unchanged", strstr(h, "_rsx_deriv_scale") == NULL &&
          ld_deriv_scale_apply(h, sizeof h, 0x0202u) == 0 && !strcmp(h, save));
    return fails ? 1 : 0;
}
EOC
CC="${CC:-cc}"
W="-std=c11 -Wall -Wno-unused-function -Wno-format-truncation -I$O -I$V -I$PT/include"
$CC $W -O2 "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm
"$O/t" 2>/dev/null
echo "[test_fp_deriv] PASS"
