#!/usr/bin/env bash
# Pass 90 host test (tools/spu/test_r90_pr369_370.c): upstream PRs #369/#370 against KZ3's own SPU float helpers.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r90_pr369_370.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if ! grep -q "spu__fma4" "$P/runtime/spu/spu_helpers.h" 2>/dev/null; then echo "SKIP: $P has no KZ3 SPU helpers"; exit 0; fi
rm -f "$O/t_r90"
gcc -std=gnu17 -O2 -msse4.1 ${R90_ISA:-} -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_r90" "$ROOT/tools/spu/test_r90_pr369_370.c" -lm 2>&1 | grep -m5 -i "error\|undefined"
"$O/t_r90"
