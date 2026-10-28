#!/usr/bin/env bash
# Host test for KZ3 patch 2114 (render-time camera interpolation math): tools/smoke/test_cam_interp.c against the patched
# toolkit's libs/input/cam_interp.h. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_cam_interp.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if [[ ! -f "$I/cam_interp.h" ]]; then echo "[test_cam_interp] SKIP: toolkit without patch 2114"; exit 0; fi
O="$ROOT/build/smoke-host/cam_interp"; mkdir -p "$O"
cc -std=c11 -O1 -Wall -Wextra -I"$I" "$ROOT/tools/smoke/test_cam_interp.c" -lm -o "$O/t"
"$O/t"
