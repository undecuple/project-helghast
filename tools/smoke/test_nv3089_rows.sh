#!/usr/bin/env bash
# Host test for KZ3 patch 2080 (NV3089 unscaled blits as row copies, libs/video/rsx_nv3089_rows.h): byte-identical
# to cellGcmSys.c's per-pixel loop over random blits. Linux, no game data; SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_nv3089_rows.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/libs/video/rsx_nv3089_rows.h" ]]; then echo "[test_nv3089_rows] SKIP: no patched toolkit with 2080 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/nv3089_rows"; mkdir -p "$O"
gcc -std=gnu11 -O2 -Wall -Werror -I "$PT/libs/video" -o "$O/test_nv3089_rows" "$ROOT/tools/smoke/test_nv3089_rows.c"
"$O/test_nv3089_rows"
