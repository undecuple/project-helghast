#!/usr/bin/env bash
# Patch 1110 host test (tools/spu/test_lr_lastcheck.c): the last SPU_EVENT_LR check before GETLLAR/PUTLLC
# replaces or drops a reservation, and the KZ3_LR_SNAP snapshot. Passes on the default path and must FAIL
# with KZ3_LR_LASTCHECK=0 (the old path: the missed-wake cases). Builds against work/ps3recomp-patched
# (or LL_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_lr_lastcheck.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/ll-tests"; mkdir -p "$O"
T="${LL_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_coherency.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q spu_resv_last_check "$T/runtime/spu/spu_coherency.c" || { echo "SKIP: $T has no patch 1110"; exit 0; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_lr_lastcheck"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_lr_lastcheck" \
   "$ROOT/tools/spu/test_lr_lastcheck.c" $B $extra -lm 2>&1 | grep -m5 "undefined reference\|error:" | sort -u)
[[ -x "$O/t_lr_lastcheck" ]] || { echo "== t_lr_lastcheck: BUILD FAIL"; exit 1; }
rc=0
out="$("$O/t_lr_lastcheck" 2>&1)"; r=$?
echo "== t_lr_lastcheck (exit $r): $(tail -n1 <<<"$out")"
[[ $r == 0 ]] || { grep -m8 FAIL <<<"$out"; rc=1; }
out="$(KZ3_LR_LASTCHECK=0 "$O/t_lr_lastcheck" 2>&1)"; r=$?
n=$(grep -c "^FAIL: [1-4]:" <<<"$out")
echo "== t_lr_lastcheck KZ3_LR_LASTCHECK=0 (exit $r, want 1): $n missed-wake cases fail (want 5)"
[[ $r == 1 && $n == 5 ]] || { grep -m8 FAIL <<<"$out"; rc=1; }
exit $rc
