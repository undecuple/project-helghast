#!/usr/bin/env bash
# Patch 1090 host test (tools/spu/test_spu_ovl_index.c): the overlay source index returns the same entry as the old
# linear scan over every registered source (EA and content-signature sources, colliding buckets, duplicates).
# Needs work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_ovl_index.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "spu_ovl_find" "$T/runtime/spu/spu_channels.c" || { echo "FAIL: $T has no patch 1090"; exit 1; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_spu_ovl_index"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_spu_ovl_index" \
    "$ROOT/tools/spu/test_spu_ovl_index.c" $B $extra -lm 2>&1 | grep -m8 error)
[[ -x "$O/t_spu_ovl_index" ]] || { echo "FAIL: build"; exit 1; }
"$O/t_spu_ovl_index"
