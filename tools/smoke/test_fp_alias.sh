#!/usr/bin/env bash
# Host test for KZ3 patch 2064 (NV40 half/full register aliasing in the fragment decompiler): h[2k] is the low 64 bits
# of r[k], h[2k+1] the high 64 bits. A program that writes h0.xy and reads r0.x gets r0.x = the two halves packed; one
# that writes r3.x and reads h6 gets h6.xy = the halves of r3.x; LD_FP_ALIAS=0 emits neither; a program that never
# reads across the views is byte-identical either way. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_alias.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then echo "[test_fp_alias] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q fp_alias_scan "$V/rsx_fp_decompiler.c"; then echo "[test_fp_alias] SKIP: toolkit without patch 2064"; exit 0; fi
O="$ROOT/build/smoke-host/fp_alias"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define HALF_OUT (1u << 7)
#define MASK(x, y, z, w) (((x) << 9) | ((y) << 10) | ((z) << 11) | ((w) << 12))
#define SRC_TEMP(r) ((u32)(r) << 2)
#define SRC_HALF (1u << 8)
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
int main(void)
{
    const int on = !(getenv("LD_FP_ALIAS") && getenv("LD_FP_ALIAS")[0] == '0');
    static char h[65536];
    /* A: MOV h0.xy, COL0 ; MOV r2, r0.xxxx (END), 32-bit exports. */
    u8 a[32];
    rsx_test_fp_instr(a, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | HALF_OUT | MASK(1, 1, 0, 0) | RSX_TEST_FP_INSRC_COL0,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(a + 16, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_OUTREG(2) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_END, SRC_TEMP(0) | RSX_TEST_FP_SWZ(0, 0, 0, 0));
    if (rsx_fp_decompile_buffered_ex(a, 32, 0x40, 0, h, sizeof h, NULL) <= 0) { puts("FAIL decompile A"); return 1; }
    const char* g = strstr(h, "r[0].x = asfloat(f32tof16(h[0].x) | (f32tof16(h[0].y) << 16));");
    check(on ? "A: r0.x gathers h0.xy" : "A: no gather when off", on ? g != NULL : g == NULL);
    check("A: r0.y untouched (h0.zw not written)", strstr(h, "r[0].y = asfloat") == NULL);
    /* B: MOV r3.x, COL0 ; MOV r0, h6.xyzw (END), 32-bit exports. */
    u8 b[32];
    rsx_test_fp_instr(b, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_OUTREG(3) | MASK(1, 0, 0, 0) |
                      RSX_TEST_FP_INSRC_COL0, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(b + 16, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      SRC_TEMP(6) | SRC_HALF | RSX_TEST_FP_SWZ_IDENT);
    if (rsx_fp_decompile_buffered_ex(b, 32, 0x40, 0, h, sizeof h, NULL) <= 0) { puts("FAIL decompile B"); return 1; }
    const char* s = strstr(h, "h[6].xy = float2(f16tof32(asuint(r[3].x)), f16tof32(asuint(r[3].x) >> 16));");
    check(on ? "B: h6.xy splits r3.x" : "B: no split when off", on ? s != NULL : s == NULL);
    /* C: MOV r0, COL0 (END): no cross-view read, so the text must not change. */
    u8 c[16]; static char h2[65536];
    rsx_test_fp_mov_r0_col0(c, RSX_TEST_FP_SWZ_IDENT);
    if (rsx_fp_decompile_buffered_ex(c, 16, 0x40, 0, h2, sizeof h2, NULL) <= 0) { puts("FAIL decompile C"); return 1; }
    check("C: no alias line in a plain program", strstr(h2, "/* alias */") == NULL);
    return fails ? 1 : 0;
}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm 2>&1 | grep -i "error" || true
"$O/t"
LD_FP_ALIAS=0 "$O/t"
echo "[test_fp_alias] PASS"
