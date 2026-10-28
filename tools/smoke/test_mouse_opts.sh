#!/usr/bin/env bash
# Host test for KZ3 patch 2101 (Options > Mouse rows): tools/smoke/test_mouse_opts.c against the patched toolkit's
# libs/input/pad_mouse.{h,c}. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_mouse_opts.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if [[ ! -f "$I/pad_mouse.c" ]]; then echo "[test_mouse_opts] SKIP: toolkit without patch 2069"; exit 0; fi
O="$ROOT/build/smoke-host/mouse_opts"; mkdir -p "$O"
env -u KZ3_MOUSE_LOOK -u KZ3_MOUSE_SCRIPT -u KZ3_MOUSE_SENS -u KZ3_MOUSE_YAW -u KZ3_MOUSE_ZOOM_SENS cc -std=c11 -O1 -Wall -Wextra -I"$I" \
  "$ROOT/tools/smoke/test_mouse_opts.c" "$I/pad_mouse.c" -lm -lpthread -o "$O/t"
env -u KZ3_MOUSE_LOOK -u KZ3_MOUSE_SCRIPT -u KZ3_MOUSE_SENS -u KZ3_MOUSE_YAW -u KZ3_MOUSE_ZOOM_SENS "$O/t"
