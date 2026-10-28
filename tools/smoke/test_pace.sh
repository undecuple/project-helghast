#!/usr/bin/env bash
# Host test for KZ3 patch 2072 (frame-pacing metric, rsx_pace.h): builds tools/smoke/test_pace.c against the patched
# toolkit and runs it (99x33.33 ms + 1x100 ms, steady 60 fps over two 10 s windows, overflow bucket). No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pace.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_pace.h"
if [[ ! -f "$H" ]]; then echo "[test_pace] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/pace"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$PT/libs/video" "$ROOT/tools/smoke/test_pace.c" -o "$O/test_pace" -lm
(cd "$O" && ./test_pace) | grep -v "^\[PASS\]"
