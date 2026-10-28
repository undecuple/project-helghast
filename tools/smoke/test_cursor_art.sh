#!/usr/bin/env bash
# Host test of patch 2111 (menu pointer pixels). Needs the patched toolkit (tools/ps3recomp-patched.sh); SKIP without it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if [[ ! -f "$I/pad_cursor_art.h" ]]; then
  echo "[test_cursor_art] SKIP: no patched toolkit with patch 2111 ($I/pad_cursor_art.h)"; exit 0; fi
O="$ROOT/build/smoke-host/cursor_art"; mkdir -p "$O"
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 -I"$I" "$ROOT/tools/smoke/test_cursor_art.c" -o "$O/test_cursor_art" -lm
"$O/test_cursor_art" | grep -v "^\[PASS\]"
