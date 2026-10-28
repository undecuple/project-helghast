#!/usr/bin/env bash
# Host test for KZ3 patch 2037 (keybinds.ini parser, defaults, writer, key evaluation, portable flag):
# builds tools/smoke/test_pad_keybinds.c against libs/input/pad_keybinds.h of the patched toolkit and runs it, plus
# an ASan/UBSan pass. When x86_64-w64-mingw32-gcc is on PATH (.#win shell) it also compile-checks cellPad.c.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pad_keybinds.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/input/pad_keybinds.h"
if [[ ! -f "$H" ]]; then echo "[test_pad_keybinds] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/pad_keybinds"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I$PT/libs/input"
$CC $W "$ROOT/tools/smoke/test_pad_keybinds.c" -o "$O/test_pad_keybinds"
"$O/test_pad_keybinds" | grep -v "^\[PASS\]"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_pad_keybinds.c" -o "$O/test_asan"
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  x86_64-w64-mingw32-gcc -std=c11 -Wall -Werror -Wno-unused-function -Wno-unknown-pragmas -c "$PT/libs/input/cellPad.c" -I"$PT/include" -I"$PT" \
    -o "$O/cellPad.obj" && echo "[PASS] cellPad.c compiles for Windows" || { echo "[FAIL] cellPad.c Windows compile"; exit 1; }
fi
