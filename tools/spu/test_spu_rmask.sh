#!/usr/bin/env bash
# Patch 1105 host test (tools/spu/test_spu_rmask.c), built with the port's SPU flags (-O2 -msse4.1, and again with
# -mavx2 -mfma as the Windows build's KZ3_SPU_ISA). No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_rmask.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_helpers.h" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
rc=0
for isa in "-msse4.1" "-msse4.1 -mavx2 -mfma"; do
  rm -f "$O/t_spu_rmask"
  gcc -std=gnu17 -O2 $isa -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
      -o "$O/t_spu_rmask" "$ROOT/tools/spu/test_spu_rmask.c" -lm 2>&1 | grep -m5 error
  echo "[$isa]"; "$O/t_spu_rmask" || rc=1
done
exit $rc
