#!/usr/bin/env bash
# cellSpursCreateTask's taskset bitset updates are atomic with the task exit's clear (was patch 1073's own race test).
# Since ps3recomp v0.17.0 the fix is upstream (#388, from KZ3's 1073 + the exit part of 1030: spurs_taskset_take_slot,
# spurs_taskset_bitset_update and the exit's bitsets under the lock-line lock), and 1073 is dropped (SPU pass 95).
# This runs upstream's test of it, runtime/spu/tests/test_spurs_taskset_bits_atomic.c, on the PATCHED tree with the
# CI job's source list plus runtime/diag/kz3_wait_acct.c (patch 1032), with gcc and clang where present: a KZ3 patch
# that undoes #388 fails here. Before the bump the old test compiled 1073's kz3-ts-create block instead.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_ts_create_race.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$P/runtime" ]] || { echo "SKIP: no patched toolkit at $P (run tools/ps3recomp-patched.sh)"; exit 0; }
T="$P/runtime/spu/tests/test_spurs_taskset_bits_atomic.c"
[[ -f "$T" ]] || { echo "FAIL: $T missing (ps3recomp older than v0.17.0?)"; exit 1; }
rc=0; ran=0
for cc in gcc clang; do
  command -v "$cc" >/dev/null || continue
  ran=$((ran + 1))
  exe="$O/test_spurs_taskset_bits_atomic.$cc"
  if ! (cd "$P" && "$cc" -std=gnu17 -I include -I runtime/spu -I runtime/platform -o "$exe" \
        runtime/spu/tests/test_spurs_taskset_bits_atomic.c libs/spurs/spurs_taskset.c \
        runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
        runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
        runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c \
        runtime/diag/kz3_wait_acct.c -lpthread -lm) > "$O/test_ts_create_race.$cc.log" 2>&1; then
    echo "FAIL ($cc): does not build"; tail -5 "$O/test_ts_create_race.$cc.log"; rc=1; continue
  fi
  out="$("$exe" 2>&1)"; r=$?
  echo "$cc: $(tail -1 <<<"$out")"
  [[ $r == 0 ]] || { echo "FAIL ($cc): exit $r"; rc=1; }
done
[[ $ran -gt 0 ]] || { echo "FAIL: no C compiler"; exit 1; }
[[ $rc == 0 ]] && echo "PASS: upstream #388 taskset bitset atomicity on the patched tree"
exit $rc
