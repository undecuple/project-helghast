#!/usr/bin/env bash
# Host test for KZ3 patch 2137's page write sequence (libs/video/rsx_tex_samef.h). No game data.
#   nix develop .#re --command tools/sandbox.sh -- bash tools/smoke/test_tex_samef.sh [--old]
# --old runs the pre-2137 rule on the same cases (expected FAIL: the late write the patch fixes).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/libs/video/rsx_tex_samef.h" ]]; then
  echo "[test_tex_samef] SKIP: no patched toolkit with 2137 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/tex_samef"; mkdir -p "$O"
def=(); [[ ${1:-} == --old ]] && def=(-DOLD_RULE)
gcc -std=gnu11 -O2 -Wall -Werror "${def[@]}" -I "$PT/libs/video" -o "$O/test_tex_samef" "$ROOT/tools/smoke/test_tex_samef.c"
"$O/test_tex_samef"
