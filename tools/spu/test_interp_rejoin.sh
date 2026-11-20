#!/usr/bin/env bash
# Patch 1104 host test (tools/spu/test_interp_rejoin.c): the SMC fallback's interpreter run resumes a lifted entry.
# Copies runtime/spu, runtime/platform and include of the patched toolkit (SPU_TREE=DIR, default work/ps3recomp-patched)
# to a scratch dir and builds the fixture once; all four modes must pass (upstream's top-zero mode is dropped: KZ3 treats a
# branch to LS 0 as job complete). Patch 1104 was absorbed into ps3recomp v0.15 (port/patches/1104-* deleted in 51905cd1,
# the RE notes), so there is no patch to apply and no negative control any more; a missing
# toolkit is a FAIL, never a skip (audit 2026-11-19: the old PF guard made this test skip silently for ever).
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_interp_rejoin.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t1104"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "FAIL: no patched toolkit at $T (run tools/ps3recomp-patched.sh)"; exit 1; }
SRCS="spu_channels.c spu_drain.c spu_lockstep.c spu_coherency.c spu_workload.c spurs_policy.c spurs_job.c spu_tsp_weak.c spu_vm_pagemap.c spurs_policy_blob_weak.c spu_interp.c"
build() {  # build NAME WITH(0|1)
  local d="$O/$1"; rm -rf "$d"; mkdir -p "$d"; cp -r "$T/runtime" "$T/include" "$d/"; mkdir -p "$d/libs"; cp -r "$T/libs/spurs" "$d/libs/"
  local srcs=() s; for s in $SRCS; do srcs+=("runtime/spu/$s"); done
  local extra=""; [[ -f "$d/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
  (cd "$d" && gcc -std=gnu17 -O1 -w -pthread -I include -I runtime/spu -I runtime/platform -o t "$ROOT/tools/spu/test_interp_rejoin.c" \
     "${srcs[@]}" runtime/platform/win32_compat.c $extra -lm 2>&1 | grep -m8 -e error -e undefined)
  [[ -x "$d/t" ]] || { echo "FAIL: build $1"; return 1; }
}
rc=0
build new 1 || exit 1
for m in top nested-calls nested-return pure-interpreter; do
  out="$(cd "$O/new" && timeout 10 ./t $m 2>&1)"; r=$?
  echo "$m rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -5; rc=1; }; done
[[ $rc == 0 ]] && echo "PASS test_interp_rejoin"
exit $rc
