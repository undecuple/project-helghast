#!/usr/bin/env bash
# Patch 1156 host test: KZ3_SPU_JOBPROF's [spu-job] line (see test_r156_jobprof.c): synthetic job set, counters add up,
# off prints nothing and leaves the accumulators untouched, negative control. SKIP without the patched toolkit or patch 1156
# (R156_TREE=DIR to point at another tree).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r156_jobprof.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${R156_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q KZ3_SPU_JOBPROF "$T/runtime/diag/kz3_wait_acct.c" || { echo "SKIP: patch 1156 not in $T (run tools/ps3recomp-patched.sh)"; exit 0; }
rm -f "$O/t_r156_jobprof"
gcc -std=gnu17 -O1 -w -pthread -I "$T/runtime/diag" -I "$T/include" -o "$O/t_r156_jobprof" "$ROOT/tools/spu/test_r156_jobprof.c" 2>&1 | grep -m8 "error\|undefined"
[[ -x "$O/t_r156_jobprof" ]] || { echo "FAIL: build"; exit 1; }
timeout 60 "$O/t_r156_jobprof" "$O"; exit $?
