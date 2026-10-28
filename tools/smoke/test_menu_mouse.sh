#!/usr/bin/env bash
# Host test for KZ3 patch 2098 (menu mouse hover and click): tools/smoke/test_menu_mouse.c against the patched
# toolkit's pure headers libs/input/pad_menu_mouse.h and pad_mouse.h. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_menu_mouse.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if [[ ! -f "$I/pad_menu_mouse.h" ]]; then echo "[test_menu_mouse] SKIP: toolkit without patch 2098"; exit 0; fi
O="$ROOT/build/smoke-host/menu_mouse"; mkdir -p "$O"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -Wall -Wextra -Werror -I"$I" "$ROOT/tools/smoke/test_menu_mouse.c" -lm -o "$O/t"
"$O/t"
