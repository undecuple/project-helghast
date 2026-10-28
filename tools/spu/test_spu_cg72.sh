#!/usr/bin/env bash
# Patch 1097 host test (tools/spu/test_spu_cg72.c): fsmbi/generate-control tables, SSE conversions and the fma
# exact-midpoint lanes against the old helpers, built with the port's SPU flags (-O2 -msse4.1, no FP contraction).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_cg72.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if ! grep -q "spu__fsmb_nib" "$P/runtime/spu/spu_helpers.h" 2>/dev/null; then echo "SKIP: $P has no patch 1097"; exit 0; fi
rm -f "$O/t_cg72"
gcc -std=gnu17 -O2 -msse4.1 ${CG72_ISA:-} -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_cg72" "$ROOT/tools/spu/test_spu_cg72.c" -lm 2>&1 | grep -m5 -i "error\|undefined"
"$O/t_cg72"
