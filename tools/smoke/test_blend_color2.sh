#!/usr/bin/env bash
# Host test for KZ3 patch 2006: on FP16 surfaces the blend constant's B/A halves come from SET_BLEND_COLOR2 (0x037C),
# never from 0x0368 (SET_SHADE_MODE). It cuts the M_BLEND_COLOR* defines, ld_half and ld_set_blend_factor out of the
# patched libs/video/rsx_live_draw.c and builds them with a stub register file (tools/smoke/test_blend_color2.c).
# Also runs a negative control: the same code with the define rewritten to 0x0368 must FAIL the test.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_blend_color2.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
SRC="$PT/libs/video/rsx_live_draw.c"
if [[ ! -f "$SRC" ]]; then echo "[test_blend_color2] SKIP: no patched toolkit ($SRC; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q 'define M_BLEND_COLOR2' "$SRC"; then echo "[test_blend_color2] SKIP: toolkit without patch 2006"; exit 0; fi
O="$ROOT/build/smoke-host/blend_color2"; mkdir -p "$O"

# static assertions on the source
def="$(grep -E '^#define M_BLEND_COLOR2[[:space:]]' "$SRC" | awk '{print $3}')"
if [[ "${def^^}" != "0X037C" ]]; then echo "[FAIL] M_BLEND_COLOR2 is $def, want 0x037C"; exit 1; fi
fn="$(awk '/^static void ld_set_blend_factor\(/{p=1} p{print} p&&/^}/{exit}' "$SRC")"
if [[ -z "$fn" ]]; then echo "[FAIL] ld_set_blend_factor not found"; exit 1; fi
if grep -qiE '0x0*368' <<<"$fn"; then echo "[FAIL] ld_set_blend_factor reads 0x0368 (SET_SHADE_MODE)"; exit 1; fi
if ! grep -q 'rsx_dsp_reg(&g.rsx, M_BLEND_COLOR2)' <<<"$fn"; then echo "[FAIL] ld_set_blend_factor does not read M_BLEND_COLOR2"; exit 1; fi
echo "[PASS] static: M_BLEND_COLOR2 = 0x037C, read by ld_set_blend_factor, no 0x0368"

extract() {  # $1 = define value override or empty
  { grep -E '^#define M_BLEND_COLOR(2)?[[:space:]]' "$SRC"
    awk '/^static float ld_half\(/{p=1} p{print} p&&/^}/{exit}' "$SRC"
    printf '%s\n' "$fn"; } | { if [[ -n "$1" ]]; then sed -E "s/^(#define M_BLEND_COLOR2[[:space:]]+)0[xX][0-9A-Fa-f]+/\1$1/"; else cat; fi; }
}
build() {  # $1 = name, $2 = override
  mkdir -p "$O/$1"; extract "$2" > "$O/$1/blend_extract.inc"
  ${CC:-cc} -std=c11 -O1 -Wall -Wextra -Wno-unused-function -I"$O/$1" "$ROOT/tools/smoke/test_blend_color2.c" -o "$O/$1/t" -lm
}
build good ""
"$O/good/t"
echo "[test_blend_color2] negative control: define rewritten to 0x0368 must fail"
build bad "0x0368"
if "$O/bad/t" > "$O/bad/out.txt" 2>&1; then echo "[FAIL] negative control passed on a 0x0368 define"; exit 1; fi
grep FAIL "$O/bad/out.txt" | sed 's/^/  (expected) /'
echo "[PASS] negative control failed as expected"
