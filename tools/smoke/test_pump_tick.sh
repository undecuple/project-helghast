#!/usr/bin/env bash
# Host test for RSX pass 117 (frame_clock pumps the window's messages on every vblank tick, KZ3_PUMP_TICK):
# tools/smoke/test_pump_tick.c, a model of the loop with the patched toolkit's libs/input/pad_mouse.h (header only).
# No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pump_tick.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if ! grep -q pad_look_acc_take_until "$I/pad_mouse.h" 2>/dev/null; then echo "[test_pump_tick] SKIP: toolkit without patch 2108"; exit 0; fi
O="$ROOT/build/smoke-host/pump_tick"; mkdir -p "$O"
cc -std=c11 -O1 -Wall -Wextra -fsanitize=address,undefined -I"$I" "$ROOT/tools/smoke/test_pump_tick.c" -lm -o "$O/t"
"$O/t"
