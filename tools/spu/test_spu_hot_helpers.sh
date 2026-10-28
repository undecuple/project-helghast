#!/usr/bin/env bash
# Patch 1035 host test (tools/spu/test_spu_hot_helpers.c) in three builds: SSE4.1 (the port's SPU flags),
# plain x86-64 (byte-loop shufb) and -mfma (hardware fmaf). Needs work/ps3recomp-patched.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_hot_helpers.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="$ROOT/work/ps3recomp-patched"; rc=0
H="${SPU_HDR:-$P/runtime/spu}"   # SPU_HDR=DIR tests other spu_helpers.h/spu_context.h copies
for v in "sse41:-msse4.1" "base:" "fma:-msse4.1 -mfma"; do
  n="${v%%:*}"; f="${v#*:}"
  gcc -std=gnu17 -O2 -w -ffp-contract=off $f -I "$H" -I "$P/include" -I "$P/runtime/spu" -I "$P/runtime/platform" \
      -o "$O/t_hot_$n" "$ROOT/tools/spu/test_spu_hot_helpers.c" -lm 2>&1 | grep -m5 error
  echo "== $n"; "$O/t_hot_$n" 2>/dev/null || rc=1
done
exit $rc
