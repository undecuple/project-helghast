#!/usr/bin/env bash
# Host test for KZ3 patch 2085 (SET_COLOR_MASK reserved bits: logged and applied by default, ignored with
# RSX_COLOR_MASK_RESERVED=ignore): builds tools/smoke/test_color_mask.c with the patched toolkit's rsx_dispatch.c and
# runs it in both modes. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_color_mask.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_dispatch.c" || ! -d "$PT/include" ]]; then
  echo "[test_color_mask] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q RSX_COLOR_MASK_RESERVED "$V/rsx_dispatch.c"; then
  echo "[test_color_mask] SKIP: toolkit without patch 2085"; exit 0; fi
O="$ROOT/build/smoke-host/color_mask"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I"$V" -I"$PT/include" \
  "$ROOT/tools/smoke/test_color_mask.c" "$V/rsx_dispatch.c" -o "$O/test_color_mask" -lm
env -u RSX_COLOR_MASK_RESERVED "$O/test_color_mask" default 2>&1 | grep -v "^\[PASS\]"
RSX_COLOR_MASK_RESERVED=ignore "$O/test_color_mask" ignore 2>&1 | grep -v "^\[PASS\]"
