#!/usr/bin/env bash
# Host test for KZ3 patch 2083's LD_TEX_WRITER page stamp (libs/video/rsx_tex_writer.h). No game data.
#   nix develop .#re --command tools/sandbox.sh -- bash tools/smoke/test_tex_writer.sh [--old]
# --old evaluates patch 2081's frame rule on the same cases (expected FAIL: the defect 2083 fixes).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/libs/video/rsx_tex_writer.h" ]]; then
  echo "[test_tex_writer] SKIP: no patched toolkit with 2083 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/tex_writer"; mkdir -p "$O"
def=(); [[ ${1:-} == --old ]] && def=(-DOLD_RULE)
gcc -std=gnu11 -O2 -Wall -Werror "${def[@]}" -I "$PT/libs/video" -o "$O/test_tex_writer" "$ROOT/tools/smoke/test_tex_writer.c"
"$O/test_tex_writer"
