#!/usr/bin/env bash
# Host test for KZ3 patch 2086 (per-pass GPU classes, rsx_gpupass.h): builds tools/smoke/test_gpupass.c against the patched
# toolkit and runs it (class rule on K4 pass shapes, top-N pick). No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_gpupass.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_gpupass.h"
if [[ ! -f "$H" ]]; then echo "[test_gpupass] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/gpupass"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$PT/libs/video" "$ROOT/tools/smoke/test_gpupass.c" -o "$O/test_gpupass"
(cd "$O" && ./test_gpupass) | grep -v "^\[PASS\]"
