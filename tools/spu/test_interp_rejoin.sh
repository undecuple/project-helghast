#!/usr/bin/env bash
# Patch 1104 host test (tools/spu/test_interp_rejoin.c): the SMC fallback's interpreter run resumes a lifted entry.
# Copies runtime/spu, runtime/platform and include of the patched toolkit (SPU_TREE=DIR, default work/ps3recomp-patched)
# to a scratch dir, builds the fixture once WITHOUT 1104 (negative control: top, nested-calls must fail;
# nested-return, pure-interpreter must pass; upstream's top-zero mode is dropped: KZ3 treats a branch to LS 0 as job complete) and once WITH it (all must pass).
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_interp_rejoin.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t1104"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"; PF="$ROOT/port/patches/1104-spu-smc-interp-lifted-rejoin.patch"
[[ -f "$T/runtime/spu/spu_channels.c" && -f "$PF" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
SRCS="spu_channels.c spu_drain.c spu_lockstep.c spu_coherency.c spu_workload.c spurs_policy.c spurs_job.c spu_tsp_weak.c spu_vm_pagemap.c spurs_policy_blob_weak.c spu_interp.c"
build() {  # build NAME WITH(0|1)
  local d="$O/$1"; rm -rf "$d"; mkdir -p "$d"; cp -r "$T/runtime" "$T/include" "$d/"; mkdir -p "$d/libs"; cp -r "$T/libs/spurs" "$d/libs/"
  if [[ $2 == 1 ]] && ! grep -q "KZ3 patch 1104" "$d/runtime/spu/spu_channels.c"; then
    (cd "$d" && git apply "$PF") || { echo "FAIL: patch 1104 does not apply"; return 1; }; fi
  local srcs=() s; for s in $SRCS; do srcs+=("runtime/spu/$s"); done
  local extra=""; [[ -f "$d/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
  (cd "$d" && gcc -std=gnu17 -O1 -w -pthread -I include -I runtime/spu -I runtime/platform -o t "$ROOT/tools/spu/test_interp_rejoin.c" \
     "${srcs[@]}" runtime/platform/win32_compat.c $extra -lm 2>&1 | grep -m8 -e error -e undefined)
  [[ -x "$d/t" ]] || { echo "FAIL: build $1"; return 1; }
}
if grep -q "KZ3 patch 1104" "$T/runtime/spu/spu_channels.c"; then echo "SKIP negative control: tree already has 1104"; NEG=0; else NEG=1; fi
rc=0
if [[ $NEG == 1 ]]; then
  build old 0 || exit 1
  for m in top nested-calls; do
    if (cd "$O/old" && timeout 10 ./t $m >/dev/null 2>&1); then echo "FAIL: control $m passed without 1104"; rc=1; else echo "control $m fails without 1104: OK"; fi; done
fi
build new 1 || exit 1
for m in top nested-calls nested-return pure-interpreter; do
  out="$(cd "$O/new" && timeout 10 ./t $m 2>&1)"; r=$?
  echo "with 1104: $m rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -5; rc=1; }; done
[[ $rc == 0 ]] && echo "PASS test_interp_rejoin"
exit $rc
