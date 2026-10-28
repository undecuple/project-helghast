#!/usr/bin/env bash
# Patch 1096 host test (tools/spu/test_spu_hop.c): direct hops between lifted SPU functions (SPU_HOP / SPU_IB_HOP).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_hop.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if ! grep -q "SPU_IB_HOP" "$P/runtime/spu/spu_context.h" 2>/dev/null; then echo "SKIP: $P has no SPU_IB_HOP (patch 1096 not applied)"; exit 0; fi
rm -f "$O/t_hop"
gcc -std=gnu17 -O2 -msse4.1 -w -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_hop" "$ROOT/tools/spu/test_spu_hop.c" -lm 2>&1 | grep -m5 -i "error\|undefined"
# 8 MB default stack: 20 M nested calls would overflow it, so a pass also shows the hops are tail calls
"$O/t_hop"
