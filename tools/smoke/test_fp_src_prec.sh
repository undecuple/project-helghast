#!/usr/bin/env bash
# Host test for patch 2124 (fragment-program source precision modifiers, DWORD2 bits 19-27): a source with
# modifier 4 is wrapped in clamp(.., 0.0, 1.0), 3 in clamp(.., -1.0, 1.0) with the negate outside the clamp and abs
# inside, 2 in [-2, 2], 1 in +-65504; 0 and 5 (RPCS3: no effect) leave the source as is; LD_NO_SRC_PREC=1 gives the
# old text; a branch instruction's DWORD2 is not read as modifiers. Needs only the patched toolkit (no game data).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_src_prec.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then echo "[test_fp_src_prec] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q 'LD_NO_SRC_PREC' "$V/rsx_fp_decompiler.c"; then echo "[test_fp_src_prec] FAIL: toolkit without patch 2124"; exit 1; fi
O="$ROOT/build/smoke-host/fp_src_prec"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
static char h[65536];
/* MUL r0, TC0, TC0 (END) with SRC0 modifier p0 (abs a0) and SRC1 modifier p1 (negate n1). */
static int fp_mul(u32 p0, u32 p1, int a0, int n1)
{
    u8 a[16];
    rsx_test_fp_instr(a, RSX_TEST_FP_OPC(0x02u) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | (a0 ? (1u << 29) : 0u));
    rsx_test_fp_put_word(a + 8, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | (n1 ? (1u << 17) : 0u) |
                                (p0 << 19) | (p1 << 22));
    return rsx_fp_decompile_buffered_ex(a, 16, 0x40, 0, h, sizeof h, NULL) > 0;
}
int main(int argc, char** argv)
{
    const int off = argc > 1 && !strcmp(argv[1], "off");
    if (!fp_mul(0, 0, 0, 0)) { puts("FAIL decompile"); return 1; }
    check("modifier 0: no clamp", strstr(h, "clamp(") == NULL);
    if (!fp_mul(5, 5, 0, 0)) { puts("FAIL decompile"); return 1; }
    check("modifier 5: no clamp", strstr(h, "clamp(") == NULL);
    if (!fp_mul(4, 3, 1, 1)) { puts("FAIL decompile"); return 1; }
    if (off) {
        check("LD_NO_SRC_PREC=1: no clamp", strstr(h, "clamp(") == NULL);
        return fails ? 1 : 0;
    }
    check("modifier 4 on SRC0: clamp(abs(..), 0.0, 1.0)", strstr(h, "clamp(abs(") != NULL && strstr(h, "), 0.0, 1.0)") != NULL);
    check("modifier 3 on SRC1: -(clamp(.., -1.0, 1.0))", strstr(h, "-(clamp(") != NULL && strstr(h, ", -1.0, 1.0))") != NULL);
    if (!fp_mul(2, 1, 0, 0)) { puts("FAIL decompile"); return 1; }
    check("modifier 2: [-2, 2]", strstr(h, ", -2.0, 2.0)") != NULL);
    check("modifier 1: half range", strstr(h, ", -65504.0, 65504.0)") != NULL);
    return fails ? 1 : 0;
}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm 2>&1 | grep -i "error" || true
"$O/t"
LD_NO_SRC_PREC=1 "$O/t" off
echo "[test_fp_src_prec] PASS"
