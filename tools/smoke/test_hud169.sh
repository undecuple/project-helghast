#!/usr/bin/env bash
# Host test for KZ3 patch 2113 (2D layer kept 16:9): builds tools/smoke/test_hud169.c
# against the patched toolkit's libs/video/rsx_display_mode.h and runs it (plus an ASan/UBSan pass). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_hud169.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_hud169.h" ]]; then
  echo "[test_hud169] SKIP: no patched toolkit with patch 2113 ($V/rsx_hud169.h; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/hud169"; mkdir -p "$O"
CC="${CC:-cc}"
W=(-std=c11 -Wall -Wextra -Werror -I"$V")
"$CC" "${W[@]}" -O2 "$ROOT/tools/smoke/test_hud169.c" -o "$O/test_hud169" -lm
"$O/test_hud169" | grep -v "^\[PASS\]"
if echo "int main(void){return 0;}" | "$CC" -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  "$CC" "${W[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_hud169.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
