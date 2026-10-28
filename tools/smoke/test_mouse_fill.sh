#!/usr/bin/env bash
# Host test for KZ3 patch 2116 (render-rate fill of the mouse look): tools/smoke/test_mouse_fill.c against the patched
# toolkit's libs/input/pad_mouse.h (header only). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_mouse_fill.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if ! grep -q pad_look_fill_delta "$I/pad_mouse.h" 2>/dev/null; then echo "[test_mouse_fill] SKIP: toolkit without patch 2116"; exit 0; fi
O="$ROOT/build/smoke-host/mouse_fill"; mkdir -p "$O"
cc -std=c11 -O1 -Wall -Wextra -I"$I" "$ROOT/tools/smoke/test_mouse_fill.c" -lm -o "$O/t"
"$O/t"
