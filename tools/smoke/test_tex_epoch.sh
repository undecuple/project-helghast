#!/usr/bin/env bash
# Host test for KZ3 patch 2159 (texture-state epochs): scripted + random register streams against the patched
# toolkit's rsx_dispatch.c and rsx_tex_epoch.h, and a negative control (hidden epoch bumps must be caught by the verify).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_tex_epoch.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_dispatch.c" || ! -d "$PT/include" ]]; then
  echo "[test_tex_epoch] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if [[ ! -f "$V/rsx_tex_epoch.h" ]]; then echo "[test_tex_epoch] SKIP: toolkit without patch 2159"; exit 0; fi
O="$ROOT/build/smoke-host/tex_epoch"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I"$V" -I"$PT/include" \
  "$ROOT/tools/smoke/test_tex_epoch.c" "$V/rsx_dispatch.c" -o "$O/test_tex_epoch" -lm
"$O/test_tex_epoch"
"$O/test_tex_epoch" neg && echo "[test_tex_epoch] negative control: stale epoch caught by the verify (PASS)" \
  || { echo "[test_tex_epoch] FAIL: negative control not detected"; exit 1; }
