#!/usr/bin/env bash
# Host test for KZ3 patch 2039 (keycap prompts): builds tools/smoke/test_keycaps.c against
# libs/video/rsx_keycaps.h of the patched toolkit and runs it, plus an ASan/UBSan pass. No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_keycaps.sh
# Preview of a real atlas dump (scratch, not in CI): build/smoke-host/keycaps/test_keycaps ATLAS.bin W H OUT.ppm
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_keycaps.h"
if [[ ! -f "$H" ]]; then echo "[test_keycaps] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/keycaps"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I$PT/libs/video"
$CC $W "$ROOT/tools/smoke/test_keycaps.c" -o "$O/test_keycaps" -lm
"$O/test_keycaps" | grep -v "^\[PASS\]"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_keycaps.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
