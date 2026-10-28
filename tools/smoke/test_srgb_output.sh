#!/usr/bin/env bash
# Host test for KZ3 patch 2055 (SET_SHADER_PACKER output sRGB, adapted from sp00nznet/ps3recomp PR #304 by tekulvw):
# enable rule, the output rewrite on both shapes (single target, struct with depth), atomic failure. Since ps3recomp
# v0.15.0 the helper is upstream's (PR #304 merged: the final return goes through _rsx_color_out, encoded per target;
# 2055 keeps only the D3D12 live-draw call site), so the shape checks follow upstream's form.
# No game data, no GPU.   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_srgb_output.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then echo "[test_srgb_output] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q rsx_fp_apply_srgb_output "$V/rsx_fp_decompiler.h"; then echo "[test_srgb_output] SKIP: toolkit without patch 2055"; exit 0; fi
O="$ROOT/build/smoke-host/srgb_output"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); return 1; } } while (0)
int main(void)
{
    /* eligibility: packer, 32-bit exports (0x40), float/X32 surfaces */
    CHECK(!rsx_fp_srgb_output_enabled(0, 0x00, 8), "packer 0 off");
    CHECK(rsx_fp_srgb_output_enabled(1, 0x00, 8), "packer 1, half exports, A8R8G8B8 on");
    CHECK(rsx_fp_srgb_output_enabled(0x10, 0x00, 5), "any non-zero packer on");
    CHECK(!rsx_fp_srgb_output_enabled(1, 0x40, 8), "32-bit exports bypass");
    CHECK(!rsx_fp_srgb_output_enabled(1, 0x00, 0x0B), "W16Z16Y16X16 bypass");
    CHECK(!rsx_fp_srgb_output_enabled(1, 0x00, 0x0C), "W32 bypass");
    CHECK(!rsx_fp_srgb_output_enabled(1, 0x00, 0x0D), "X32 bypass");
    u8 fp[16]; static char h[65536], keep[65536];
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC(2) | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    /* single target */
    CHECK(rsx_fp_decompile_buffered_ex(fp, 16, 0x00, 0, h, sizeof h, NULL) > 0, "decompile single");
    CHECK(strstr(h, "float4 main(PSInput input) : SV_TARGET"), "single: has main");
    CHECK(rsx_fp_apply_srgb_output(h, sizeof h) == 1, "apply single");
    CHECK(strstr(h, "float4 main(PSInput input) : SV_TARGET"), "single: entry kept");
    CHECK(strstr(h, "    float4 _rsx_color_out = "), "single: result captured");
    CHECK(strstr(h, "    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);"), "single: encoded before return");
    CHECK(strstr(h, "float4(saturate(rgb), v.a)"), "alpha untouched, rgb saturated");
    CHECK(rsx_fp_apply_srgb_output(h, sizeof h) == -1, "second apply refused");
    /* struct output with depth export (ctrl 0x0E) */
    CHECK(rsx_fp_decompile_buffered_ex(fp, 16, 0x0E, 0, h, sizeof h, NULL) > 0, "decompile struct");
    CHECK(strstr(h, "struct PSOutput") && strstr(h, "SV_Depth"), "struct with depth");
    CHECK(rsx_fp_apply_srgb_output(h, sizeof h) == 1, "apply struct");
    CHECK(strstr(h, "    PSOutput _rsx_color_out = "), "struct: result captured");
    CHECK(strstr(h, "_rsx_color_out.t0 = _rsx_srgb_output(_rsx_color_out.t0);"), "struct: t0 encoded");
    CHECK(!strstr(h, "_rsx_srgb_output(_rsx_color_out.depth"), "depth untouched");
    /* atomic failure: no room -> source unchanged; no entry point -> unchanged */
    CHECK(rsx_fp_decompile_buffered_ex(fp, 16, 0x00, 0, h, sizeof h, NULL) > 0, "decompile again");
    strcpy(keep, h);
    CHECK(rsx_fp_apply_srgb_output(h, (u32)strlen(h) + 8) == -1 && !strcmp(h, keep), "no room: refused, unchanged");
    strcpy(h, "float4 other() { return 0; }\n");
    strcpy(keep, h);
    CHECK(rsx_fp_apply_srgb_output(h, sizeof h) == -1 && !strcmp(h, keep), "no entry point: refused, unchanged");
    puts("srgb output: ok");
    return 0;
}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm
"$O/t"
