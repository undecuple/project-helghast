#!/usr/bin/env bash
# Patch 1071 host test (tools/spu/test_spu_xr.c), built with the port's SPU flags (-O2 -msse4.1). No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_xr.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
rm -f "$O/t_spu_xr"
gcc -std=gnu17 -O2 -msse4.1 ${XR_ISA:-} -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_spu_xr" "$ROOT/tools/spu/test_spu_xr.c" -lm 2>&1 | grep -m5 error
"$O/t_spu_xr"
