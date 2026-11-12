#!/usr/bin/env bash
# Host test for KZ3 patch 4030 (motion-section assist: mouse valve stick, L3+R3 autocomplete): tools/smoke/test_motion_assist.c
# against the patched toolkit's libs/input/pad_assist.h (header only). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_motion_assist.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/input/pad_assist.h"
if [[ ! -f "$H" ]]; then echo "[test_motion_assist] SKIP: toolkit without patch 4030"; exit 0; fi
O="$ROOT/build/smoke-host/motion_assist"; mkdir -p "$O"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$PT/libs/input" "$ROOT/tools/smoke/test_motion_assist.c" -o "$O/t" -lm
"$O/t"
