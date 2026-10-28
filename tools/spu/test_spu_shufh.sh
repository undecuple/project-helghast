#!/usr/bin/env bash
# Patch 1108 host test: tools/spu/test_spu_shufh.c (the hoisted shufb1 decode against the byte-loop reference; built
# with -msse4.1 and with -DSPU_SHUFB_HOIST=0) and tools/spu/test_spu_shufh.py (the lifter pass). No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_shufh.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_helpers.h" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
grep -q 'KZ3 patch 1108' "$P/runtime/spu/spu_helpers.h" || { echo "SKIP: patch 1108 not in $P"; exit 0; }
rc=0
for f in "" "-DSPU_SHUFB_HOIST=0"; do
  rm -f "$O/t_spu_shufh"
  gcc -std=gnu17 -O2 -msse4.1 -w -ffp-contract=off -fno-strict-aliasing $f -I "$P/runtime/spu" -I "$P/include" \
      -I "$P/runtime" -o "$O/t_spu_shufh" "$ROOT/tools/spu/test_spu_shufh.c" -lm 2>&1 | grep -m5 -E 'error|undefined'
  "$O/t_spu_shufh" || rc=1
done
python3 "$ROOT/tools/spu/test_spu_shufh.py" "$P" || rc=1
exit $rc
