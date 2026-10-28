#!/usr/bin/env bash
# MFC engine host test (tools/spu/test_mfc_slots.c; was patch 1083's, which v0.15.0 absorbed: upstream PR #290's
# per-context slot list, s_mfc_slots in spu_channels.c, now grows without limit): every SPU context keeps its own
# MFC engine past 64 live contexts (300 contexts: distinct engines, a held tag not completed by peers' DMA, released
# slots reused). Upstream's own runtime/spu/tests/test_spu_mfc_slots.c runs in test_lockline.sh. Needs
# work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_mfc_slots.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "s_mfc_slots" "$T/runtime/spu/spu_channels.c" || { echo "FAIL: $T has no per-context MFC slot list (PR #290)"; exit 1; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_mfc_slots"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_mfc_slots" \
    "$ROOT/tools/spu/test_mfc_slots.c" $B $extra -lm 2>&1 | grep -m8 error)
[[ -x "$O/t_mfc_slots" ]] || { echo "FAIL: build"; exit 1; }
out="$("$O/t_mfc_slots" 2>&1)"; r=$?
echo "$out" | grep -v "^\[SPU\] [0-9]* MFC engine slots in use" | tail -6
if grep -q "contexts hold an MFC engine at once" <<<"$out"; then echo "FAIL: shared fallback engine used"; exit 1; fi
[[ $r == 0 ]] && echo PASS || echo FAIL
exit $r
