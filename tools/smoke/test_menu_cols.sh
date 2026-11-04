#!/usr/bin/env bash
# Host test for patch 2151 (Display page value column): builds tools/smoke/test_menu_cols.c against the patched toolkit's
# libs/video/rsx_menu_cols.h. No game data, no GPU.  nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_menu_cols.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_menu_cols.h" ]]; then
  echo "[test_menu_cols] SKIP: no patched toolkit with patch 2151 ($V/rsx_menu_cols.h; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/menu_cols"; mkdir -p "$O"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I"$V" \
  "$ROOT/tools/smoke/test_menu_cols.c" -o "$O/test_menu_cols"
"$O/test_menu_cols"
