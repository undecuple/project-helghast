#!/usr/bin/env bash
# Patch 1107 host test (tools/spu/test_spu_lscold.c), built with the out-of-line gated accessors (default) and with
# -DSPU_LS_COLD=0 (old, inlined); both must match the byte-wise reference. No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_lscold.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_context.h" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
grep -q 'KZ3 patch 1107' "$P/runtime/spu/spu_context.h" || { echo "SKIP: patch 1107 not in $P"; exit 0; }
rc=0
for cold in 1 0; do
  rm -f "$O/t_spu_lscold"
  gcc -std=gnu17 -O2 -msse4.1 -w -fno-strict-aliasing -DSPU_LS_COLD=$cold -I "$P/runtime/spu" -I "$P/include" \
      -I "$P/runtime" -o "$O/t_spu_lscold" "$ROOT/tools/spu/test_spu_lscold.c" 2>&1 | grep -m5 -E 'error|undefined'
  "$O/t_spu_lscold" || rc=1
done
exit $rc
