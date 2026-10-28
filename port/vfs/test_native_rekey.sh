#!/usr/bin/env bash
# kz3_overlay_native_rekey host test (RE pass 168). Self-contained: no game data, no toolkit.
#   nix develop .#re --command tools/sandbox.sh -- bash port/vfs/test_native_rekey.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$(mktemp -d)"; trap 'rm -rf "$O"' EXIT
mkdir -p "$O/run/src"
gcc -std=gnu17 -O2 -Wall -Wextra -Werror -I"$ROOT/port/vfs" -o "$O/t" "$ROOT/port/vfs/test_native_rekey.c" \
    "$ROOT/port/vfs/kz3_overlay.c" -lpthread 2>"$O/err" || { echo "FAIL build"; head -20 "$O/err"; exit 1; }
"$O/t" "$O/run"
