#!/usr/bin/env bash
# Host test for KZ3 patch 2042 (PSO key from the sampled texture units only): builds tools/smoke/test_fp_units.c
# against the patched toolkit's rsx_fp_decompiler.c and runs it. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_units.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then
  echo "[test_fp_units] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q rsx_fp_texture_unit_mask "$V/rsx_fp_decompiler.h"; then
  echo "[test_fp_units] SKIP: toolkit without patch 2042 (no rsx_fp_texture_unit_mask)"; exit 0; fi
O="$ROOT/build/smoke-host/fp_units"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -O1 -I"$V" -I"$PT/include" "$ROOT/tools/smoke/test_fp_units.c" \
  "$V/rsx_fp_decompiler.c" -o "$O/test_fp_units" -lm
"$O/test_fp_units"
