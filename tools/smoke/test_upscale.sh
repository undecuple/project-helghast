#!/usr/bin/env bash
# Host test for KZ3 patch 2145 (RSX pass 134): the upscaler interface (libs/video/rsx_upscale.*) with a fake backend,
# the FSR 1 constants, the embedded shader text vs the vendored AMD headers (fsr1_embed.py --check), and, when slangc is on
# PATH, a syntax/semantic compile of the assembled HLSL for both passes (ASSUMPTION: fxc/D3DCompile accepts what slangc
# accepts; the D3D12 compile itself only runs on Windows). No game data, no GPU. SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_upscale.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/pyresolve.sh"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_upscale.c" || ! -f "$V/fsr1/ffx_fsr1.h" ]]; then
  echo "[test_upscale] SKIP: toolkit without patch 2145 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/upscale"; mkdir -p "$O"
$PY "$ROOT/tools/smoke/fsr1_embed.py" --check "$V/fsr1" || { echo "[test_upscale] FAIL: fsr1_embed.h is not what the headers generate"; exit 1; }
echo "[test_upscale] fsr1_embed.h matches the vendored headers"
SANS=("")
if echo "int main(void){return 0;}" | gcc -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  SANS+=("-fsanitize=address,undefined -fno-sanitize-recover=undefined")
else
  echo "[test_upscale] note: compiler sanitizer runtime unavailable"
fi
for san in "${SANS[@]}"; do
  # shellcheck disable=SC2086
  gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-ignored-qualifiers $san -I "$V" \
      "$ROOT/tools/smoke/test_upscale.c" "$V/rsx_upscale.c" "$V/rsx_upscale_fsr1.c" -o "$O/t" -lm
  "$O/t" "$V/fsr1" | tail -4
  [[ ${PIPESTATUS[0]} -eq 0 ]] || { echo "[test_upscale] FAIL ${san:-plain}"; exit 1; }
  echo "[test_upscale] PASS ${san:-plain}"
done
if command -v slangc >/dev/null 2>&1; then
  cat > "$O/dump.c" <<'EOC'
#include "rsx_upscale.h"
#include <stdio.h>
const rsx_upscale_ops rsx_upscale_fsr1_d3d12_ops = { 0 };
int main(void) { size_t n; char* s = rsx_upscale_fsr1_hlsl_source(&n); fwrite(s, 1, n, stdout); return 0; }
EOC
  gcc -std=c99 -I "$V" "$O/dump.c" "$V/rsx_upscale.c" "$V/rsx_upscale_fsr1.c" -o "$O/dump" -lm
  "$O/dump" > "$O/fsr1.hlsl"
  for d in SAMPLE_EASU SAMPLE_RCAS; do
    : > "$O/$d.spv"
    slangc -lang hlsl -profile cs_5_0 -target spirv -entry mainCS -D$d=1 -o "$O/$d.spv" "$O/fsr1.hlsl" 2>&1 | grep -A6 'error' | head -20 || true
    [[ -s "$O/$d.spv" ]] || { echo "[test_upscale] FAIL: slangc did not compile $d"; exit 1; }
    echo "[test_upscale] HLSL $d compiles (slangc, cs_5_0 -> SPIR-V)"
  done
else echo "[test_upscale] note: no slangc, HLSL compile skipped"; fi
echo "[test_upscale] ALL PASS"
