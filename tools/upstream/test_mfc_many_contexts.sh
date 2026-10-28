#!/usr/bin/env bash
# Focused test, first for patch 1013 (per-context MFC engines). ps3recomp v0.15.0 absorbed 1013 (PR #290's
# per-context slot list; upstream's own runtime/spu/tests/test_spu_mfc_slots.c runs in tools/spu/test_lockline.sh),
# so it now runs test_mfc_many_contexts.c against upstream (third_party/ps3recomp) and the full patched tree.
#   nix develop --command tools/sandbox.sh -- tools/upstream/test_mfc_many_contexts.sh
# Expected: upstream PASS, full stack PASS (v0.13.0 FAILed: one shared engine past the fixed table).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$ROOT/work/upstream-tests"; mkdir -p "$O"; verdict=0
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"

for t in "upstream:$ROOT/third_party/ps3recomp" "full-stack:$ROOT/work/ps3recomp-patched"; do
  name="${t%%:*}" tree="${t#*:}"
  [[ -d "$tree/runtime/spu" ]] || { echo "== $name: no tree at $tree"; verdict=1; continue; }
  # Patch 1032 (wait accounting) adds runtime/diag/kz3_wait_acct.c, which the patched SPU sources call.
  extra=""; [[ -f "$tree/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
  bs=""; for f in $B; do [[ -f "$tree/$f" ]] && bs="$bs $f"; done   # upstream lacks some KZ3-added sources
  rm -f "$O/t_$name"
  (cd "$tree" && gcc -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$O/t_$name" \
     "$ROOT/tools/upstream/test_mfc_many_contexts.c" $bs $extra -lpthread -lm 2>&1 | grep -m5 "undefined reference\|error:" | sort -u)
  echo "== $name"; out="$("$O/t_$name" 2>&1)"; tail -2 <<<"$out"
  last="$(tail -n1 <<<"$out")"
  [[ "$last" == *PASS* ]] || { echo "want PASS on $name"; verdict=1; }
done
exit $verdict
