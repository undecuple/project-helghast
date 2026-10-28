#!/usr/bin/env bash
# Host test for fragment inputs TC8/TC9 and FACING, now from upstream PR #328 (patch 9328; our 2067 and its
# LD_FP_TC89 switch were dropped in pass 86): a program that reads TC8 reads input.tc8, TC9 reads input.tc9, the PS
# input struct declares TEXCOORD8/9 only when the body reads them (other programs stay byte-identical), FACING reads
# SV_IsFrontFace as +1/-1, and the vertex program's VSOutput carries TEX8 = o[15], TEX9 = o[6].
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_tc89.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then echo "[test_fp_tc89] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q '"input.tc8"' "$V/rsx_fp_decompiler.c"; then echo "[test_fp_tc89] FAIL: toolkit without PR #328 (9328)"; exit 1; fi
O="$ROOT/build/smoke-host/fp_tc89"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
static char h[65536];
static int fp_mov_input(u32 in)  /* MOV r0, input `in` (END), 32-bit export */
{
    u8 a[16];
    rsx_test_fp_instr(a, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(in) |
                      RSX_TEST_FP_END, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    return rsx_fp_decompile_buffered_ex(a, 16, 0x40, 0, h, sizeof h, NULL) > 0;
}
int main(void)
{
    const char* decl = "float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;";
    for (u32 in = 0xC; in <= 0xD; in++) {
        if (!fp_mov_input(in)) { puts("FAIL decompile"); return 1; }
        char what[64]; snprintf(what, sizeof what, "TC%u read as the input", in - 4u);
        check(what, strstr(h, in == 0xC ? "input.tc8" : "input.tc9") != NULL);
        check("PSInput declares TEXCOORD8/9 when read", strstr(h, decl) != NULL);
    }
    if (!fp_mov_input(0x4)) { puts("FAIL decompile"); return 1; }   /* TC0 */
    check("PSInput without TEXCOORD8/9 when not read", strstr(h, decl) == NULL && strstr(h, "_rsx_front") == NULL);
    if (!fp_mov_input(0xE)) { puts("FAIL decompile"); return 1; }   /* FACING */
    check("FACING = SV_IsFrontFace as +1/-1", strstr(h, "bool _rsx_front : SV_IsFrontFace;") != NULL &&
          strstr(h, "input._rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1)") != NULL);
    /* Any vertex program: MOV o[7], v0 (END). The output struct and epilogue are fixed text. */
    u8 v[16];
    rsx_test_vp_vec_out_tex(v, RSX_TEST_VP_OP_MOV, 0, RSX_TEST_VP_SWZ_IDENT, 0, 7, 1);
    static char vs[65536];
    if (rsx_vp_decompile(v, 16, vs, sizeof vs) <= 0) { puts("FAIL vp decompile"); return 1; }
    check("VSOutput declares TEXCOORD8/9", strstr(vs, "float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;") != NULL);
    check("TEX8 = o[15], TEX9 = o[6]", strstr(vs, "Out.t8=o[15]; Out.t9=o[6];") != NULL);
    return fails ? 1 : 0;
}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/t.c" "$V/rsx_fp_decompiler.c" "$V/rsx_vp_decompiler.c" -o "$O/t" -lm 2>&1 | grep -i "error" || true
"$O/t"
echo "[test_fp_tc89] PASS"
