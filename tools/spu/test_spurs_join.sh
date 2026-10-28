#!/usr/bin/env bash
# Patch 1066/1067 host test (tools/spu/test_spurs_join.c; since SPU pass 96 also patch 1023 + #381: spu_taskset_dispatch
# must not self-deadlock on the taskset lock): JoinTaskset waits for running (not parked) tasks and cuts
# off the old generation, so a taskset re-created at the same EA cannot lose its task's signal to the old task.
# Needs work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spurs_join.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "spu_taskset_join_wait" "$T/runtime/spu/spu_workload.c" || { echo "FAIL: $T has no patch 1066"; exit 1; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_spurs_join"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_spurs_join" \
    "$ROOT/tools/spu/test_spurs_join.c" $B $extra -lm 2>&1 | grep -m8 error)
[[ -x "$O/t_spurs_join" ]] || { echo "FAIL: build"; exit 1; }
out="$(env -u KZ3_JOIN_TASKSET_OLD -u KZ3_JOIN_CUTOFF_EXIT timeout 20 "$O/t_spurs_join" 2>&1)"; r=$?
echo "rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | grep -m8 "FAIL"; exit 1; }
# Patch 1067 on (KZ3_JOIN_CUTOFF_EXIT=1): the cut-off task leaves the wait without the new task's signal.
out="$(env -u KZ3_JOIN_TASKSET_OLD KZ3_JOIN_CUTOFF_EXIT=1 timeout 20 "$O/t_spurs_join" 2>&1)"; r=$?
echo "exit: rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | grep -m8 "FAIL"; exit 1; }
