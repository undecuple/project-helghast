#!/usr/bin/env bash
# Host test for KZ3 patch 2097 (cursor hiding and mouse lock decision, KZ3_MOUSE_LOOK=0): tools/smoke/test_cursor_gate.c
# against the patched toolkit's libs/input/pad_mouse.{h,c}. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_cursor_gate.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if ! grep -q pad_cursor_decide "$I/pad_mouse.h" 2>/dev/null; then echo "[test_cursor_gate] SKIP: toolkit without patch 2097"; exit 0; fi
O="$ROOT/build/smoke-host/cursor_gate"; mkdir -p "$O"
env -u KZ3_MOUSE_LOOK cc -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -Wall -Wextra -I"$I" \
  "$ROOT/tools/smoke/test_cursor_gate.c" "$I/pad_mouse.c" -lm -lpthread -o "$O/t"
env -u KZ3_MOUSE_LOOK "$O/t"
