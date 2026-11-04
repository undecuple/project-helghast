#!/usr/bin/env bash
# Patch 1154 host test + micro-benchmark: the task thread pool (KZ3_SPU_TASK_POOL=1, default off). Runs
# tools/spu/test_task_pool.c with the switch off and on over a fake Taskset2: the taskset bytes after a serialized run
# must be equal, 16 concurrent jobs must all run at once with their own ids, the pool must reuse threads (off creates one
# per job); prints the calls-per-second line for both. SKIP without the patched toolkit or without patch 1154 in it
# (R145_TREE=DIR to point at another tree).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_task_pool.sh [N]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${R145_TREE:-$ROOT/work/ps3recomp-patched}"; N="${1:-400}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q KZ3_SPU_TASK_POOL "$T/runtime/spu/spu_workload.c" || { echo "SKIP: patch 1154 not in $T (run tools/ps3recomp-patched.sh)"; exit 0; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_task_pool"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -I libs/spurs -o "$O/t_task_pool" \
    "$ROOT/tools/spu/test_task_pool.c" $B $extra -lm 2>&1 | grep -m8 "error\|undefined")
[[ -x "$O/t_task_pool" ]] || { echo "FAIL: build"; exit 1; }
rc=0
for m in 0 1; do
  env KZ3_SPU_TASK_POOL=$m timeout 120 "$O/t_task_pool" "$N" 2>/dev/null >"$O/task_pool_$m.out" || { echo "FAIL: run pool=$m"; grep -m5 "FAIL\|RESULT" "$O/task_pool_$m.out"; rc=1; }
  grep "^RESULT\|^BENCH" "$O/task_pool_$m.out" | sed "s/^/pool=$m /"
done
a="$(grep -m1 'RESULT tshash' "$O/task_pool_0.out")"; b="$(grep -m1 'RESULT tshash' "$O/task_pool_1.out")"
[[ -n "$a" && "$a" == "$b" ]] && echo "taskset bytes equal (off == on)" || { echo "FAIL: taskset bytes differ: [$a] [$b]"; rc=1; }
t0="$(grep -m1 -o 'threads=[0-9]*' "$O/task_pool_0.out")"; t1="$(grep -m1 -o 'threads=[0-9]*' "$O/task_pool_1.out")"
echo "distinct task threads after 50 serialized + 32 burst jobs: off ${t0#threads=}, on ${t1#threads=}"
[[ "${t1#threads=}" -lt "${t0#threads=}" ]] || { echo "FAIL: the pool did not reuse threads"; rc=1; }
[[ $rc == 0 ]] && echo "ALL OK" || echo "FAILED"; exit $rc
