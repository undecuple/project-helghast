#!/usr/bin/env bash
# Patch 1095 host test (tools/spu/test_spu_ftz.c): the SPU denormal rule (spu_fpmode.h) with the port's SPU flags.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_ftz.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$P/runtime/spu/spu_fpmode.h" ]]; then echo "SKIP: $P has no spu_fpmode.h (patch 1095 not applied)"; exit 0; fi
rm -f "$O/t_ftz"
gcc -std=gnu17 -O2 -msse4.1 -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_ftz" "$ROOT/tools/spu/test_spu_ftz.c" -lm 2>&1 | grep -m5 error
"$O/t_ftz"
