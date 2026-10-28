#!/usr/bin/env bash
# Host test for KZ3 patch 2117 (controls picture without the L1 line under Modern): tools/smoke/test_ctl_no_l1.c
# against the patched toolkit's libs/video/rsx_prompt_glyphs.h (header only). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_ctl_no_l1.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_prompt_glyphs.h"
if ! grep -q rsx_pg_ctl_erase_l1 "$H" 2>/dev/null; then echo "[test_ctl_no_l1] SKIP: toolkit without patch 2117"; exit 0; fi
O="$ROOT/build/smoke-host/ctl_no_l1"; mkdir -p "$O"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$PT/libs/video" "$ROOT/tools/smoke/test_ctl_no_l1.c" -o "$O/t" -lm
"$O/t"
