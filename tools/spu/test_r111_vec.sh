#!/usr/bin/env bash
# Patches 1132-1134 host test (tools/spu/test_r111_vec.c): the vector multiplies, clz, cntb, gather/form-select masks,
# quadword byte shifts with the slow loop out of line and fi's exponent-255 lanes against the loops they replaced, built
# three ways: the port's SPU flags (-O2 -msse4.1 -mfma, no FP contraction), + -mavx2, and without SSE4.1. No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r111_vec.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if ! grep -q "spu__byteperm_ref" "$P/runtime/spu/spu_helpers.h" 2>/dev/null; then echo "SKIP: $P has no patch 1133"; exit 0; fi
rc=0
for isa in "-msse4.1 -mfma" "-msse4.1 -mfma -mavx2" "-mno-sse4.1"; do
  rm -f "$O/t_r111_vec"
  gcc -std=gnu17 -O2 $isa -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
      -o "$O/t_r111_vec" "$ROOT/tools/spu/test_r111_vec.c" -lm 2>&1 | grep -m5 -i "error\|undefined"
  echo "== $isa"
  "$O/t_r111_vec" || rc=1
done
[[ $rc == 0 ]] && echo "test_r111_vec: PASS" || echo "test_r111_vec: FAIL"
exit $rc
