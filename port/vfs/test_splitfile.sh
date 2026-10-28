#!/usr/bin/env bash
# kz3_splitfile.h host test (public issue 4). Self-contained: no game data, no toolkit. Also compiles kz3_overlay.c
# with -Werror, which uses the header for its disc archive reader.
#   nix develop .#re --command tools/sandbox.sh -- bash port/vfs/test_splitfile.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$(mktemp -d)"; trap 'rm -rf "$O"' EXIT
mkdir -p "$O/run"
gcc -std=gnu17 -O2 -Wall -Wextra -Werror -I"$ROOT/port/vfs" -o "$O/t" "$ROOT/port/vfs/test_splitfile.c" 2>"$O/err" || { echo "FAIL build"; head -20 "$O/err"; exit 1; }
gcc -std=gnu17 -O2 -Wall -Wextra -Werror -I"$ROOT/port/vfs" -c -o "$O/ovl.o" "$ROOT/port/vfs/kz3_overlay.c" 2>"$O/err" || { echo "FAIL build overlay"; head -20 "$O/err"; exit 1; }
"$O/t" "$O/run"
