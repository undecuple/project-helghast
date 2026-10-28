#!/usr/bin/env bash
# Taskset SIGNALLED bit host test, now for upstream PR #340 (patch 9340, which replaced our 1075 in pass 86): the
# host's set (spu_taskset_signal_task) and clear (WAIT_SIGNAL) of a task's bit are one read-modify-write under the
# lock-line lock, so an SPU PUTLLC to the same word between the read and the write is not lost. Builds and runs
# PR #340's own deterministic test (runtime/spu/tests/test_spurs_signalled_atomic.c: the interleaving is forced with
# the lock, not raced for) against the patched tree with our stack's extra sources (1056 striped lock: the
# whole-lock spu_lockline_lock takes every stripe, so it still excludes a striped PUTLLC). Self-contained: no game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_ts_sig_race.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_workload.c" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
T=runtime/spu/tests/test_spurs_signalled_atomic.c
[[ -f "$P/$T" ]] || { echo "FAIL: $T not in $P (patch 9340 missing)"; exit 1; }
grep -q 'spu_signalled_update' "$P/runtime/spu/spu_workload.c" \
  || { echo "FAIL: spu_signalled_update not in spu_workload.c (patch 9340 missing)"; exit 1; }
src="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c
     runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c
     runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$P/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_ts_sig"
(cd "$P" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_ts_sig" \
   "$T" $src $extra -lm 2>&1 | grep -m5 -E "error|undefined reference")
[[ -x "$O/t_ts_sig" ]] || { echo "FAIL: build"; exit 1; }
out="$("$O/t_ts_sig" 2>&1)"; rc=$?
tail -n3 <<<"$out"
(( rc == 0 )) || { grep -m8 FAIL <<<"$out"; echo "FAIL: test_spurs_signalled_atomic exit $rc"; exit 1; }
echo "PASS: test_spurs_signalled_atomic (PR #340) on the patched tree"
