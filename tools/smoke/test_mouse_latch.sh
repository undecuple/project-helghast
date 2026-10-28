#!/usr/bin/env bash
# Host test for KZ3 patch 2112 (late-latch view rotation): tools/smoke/test_mouse_latch.c against the patched toolkit's
# libs/input/pad_mouse.{h,c}. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_mouse_latch.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if ! grep -q pad_look_acc_peek "$I/pad_mouse.h" 2>/dev/null; then echo "[test_mouse_latch] SKIP: toolkit without patch 2112"; exit 0; fi
O="$ROOT/build/smoke-host/mouse_latch"; mkdir -p "$O"
env -u KZ3_MOUSE_LOOK -u KZ3_MOUSE_SCRIPT -u KZ3_MOUSE_SENS -u KZ3_MOUSE_YAW -u KZ3_MOUSE_ZOOM_SENS cc -std=c11 -O1 -Wall -Wextra -I"$I" \
  "$ROOT/tools/smoke/test_mouse_latch.c" "$I/pad_mouse.c" -lm -lpthread -o "$O/t"
env -u KZ3_MOUSE_LOOK -u KZ3_MOUSE_SCRIPT -u KZ3_MOUSE_SENS -u KZ3_MOUSE_YAW -u KZ3_MOUSE_ZOOM_SENS "$O/t"
