#!/usr/bin/env bash
# Pass 143 candidate patches (port/patches 1151-1153, default off; or staged copies via R143_PATCHES=DIR applied on a scratch
# copy of work/ps3recomp-patched), then run the lock-line host tests with every candidate switch off and on, and the
# PUTLLC fast-path semantics test (tools/spu/test_r143_putllc_fast.c) in both modes. SKIP without the patched toolkit.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r143_candidates.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/r143-tests"; T="$O/tree"
SRC="$ROOT/work/ps3recomp-patched"
[[ -f "$SRC/runtime/spu/spu_coherency.c" ]] || { echo "SKIP: no patched toolkit at $SRC"; exit 0; }
mkdir -p "$O"; [[ -d "$T" ]] && "$ROOT/tools/safe_rm.sh" "$T"; mkdir -p "$T"
tar -C "$SRC" --exclude=.git -cf - . | tar -C "$T" -xf -
rc=0
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
# Since the merge the candidates are port/patches 1151-1153 (already in the patched tree); R143_PATCHES=DIR applies staged copies instead.
PD="${R143_PATCHES:-}"
if [[ -z "$PD" ]]; then
  grep -q KZ3_COH_PREFETCH "$T/runtime/spu/spu_coherency.c" || { echo "SKIP: patches 1151-1153 not in $SRC (run tools/ps3recomp-patched.sh)"; exit 0; }
  echo "== patches 1151-1153 in the patched tree"
fi
for p in ${PD:+"$PD"/r143?-*.patch}; do
  (cd "$T" && patch -p1 -s < "$p") || { echo "== $(basename "$p"): DOES NOT APPLY"; exit 1; }
  echo "== $(basename "$p"): applied"
done
echo "-- switches off"
LL_TREE="$T" "$ROOT/tools/spu/test_lockline.sh" "$@" || rc=1
echo "-- all candidate switches on"
KZ3_COH_PREFETCH=1 KZ3_PUTLLC_FAST=1 KZ3_LL_TASKSET_STRIPE=1 LL_TREE="$T" "$ROOT/tools/spu/test_lockline.sh" "$@" || rc=1
for f in 0 1; do
  (cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform \
     -o "$O/t_putllc_fast" "$ROOT/tools/spu/test_r143_putllc_fast.c" \
     runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
     runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
     runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c \
     $extra -lm 2>&1 | grep -m5 "undefined reference\|error:" | sort -u)
  [[ -x "$O/t_putllc_fast" ]] || { echo "== t_putllc_fast: BUILD FAIL"; rc=1; break; }
  out="$(KZ3_PUTLLC_FAST=$f "$O/t_putllc_fast" 2>&1)"; r=$?
  echo "== t_putllc_fast FAST=$f (exit $r): $(tail -n1 <<<"$out")"
  [[ $r == 0 ]] || { grep -m8 FAIL <<<"$out"; rc=1; }
done
exit $rc
