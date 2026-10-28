#!/usr/bin/env bash
# Host test for KZ3 patch 2090 (prompt glyph sets): builds tools/smoke/test_prompt_glyphs.c against
# libs/video/rsx_prompt_glyphs.h (+ rsx_keycaps.h) of the patched toolkit and runs it, plus an ASan/UBSan pass. No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_prompt_glyphs.sh
# Preview sheet of the real atlases (game data, sheets stay under work/): tools/smoke/prompt_glyphs_preview.py
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_prompt_glyphs.h"
if [[ ! -f "$H" ]]; then echo "[test_prompt_glyphs] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/prompt_glyphs"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I$PT/libs/video"
$CC $W "$ROOT/tools/smoke/test_prompt_glyphs.c" -o "$O/test_prompt_glyphs" -lm
"$O/test_prompt_glyphs" | grep -v "^\[PASS\]"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_prompt_glyphs.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
