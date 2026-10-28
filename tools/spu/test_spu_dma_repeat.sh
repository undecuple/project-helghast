#!/usr/bin/env bash
# Patch 1065 host test (tools/spu/test_spu_dma_repeat.c): the SPU DMA repeat guard is per context and does not halt
# unless SPU_DMA_REPEAT_LIMIT is set. Runs the default case (600 identical GETs on one context, interleaved traffic on
# another: nobody halts, the counter is per context) and the limit case (SPU_DMA_REPEAT_LIMIT=256 halts the polling
# context at its 256th repeat and nothing else). Needs work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_dma_repeat.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "rep_n" "$T/runtime/spu/spu_dma.h" || { echo "FAIL: $T has no patch 1065"; exit 1; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_spu_dma_repeat" \
    "$ROOT/tools/spu/test_spu_dma_repeat.c" $B $extra -lm 2>&1 | grep -m8 error)
[[ -x "$O/t_spu_dma_repeat" ]] || { echo "FAIL: build"; exit 1; }
rc=0
out="$(env -u SPU_DMA_REPEAT_LIMIT -u SPU_DMA_REPEAT_LOG "$O/t_spu_dma_repeat" 2>&1)"; r=$?
echo "default: rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -8; rc=1; }
out="$(SPU_DMA_REPEAT_LIMIT=256 "$O/t_spu_dma_repeat" --limit 2>&1)"; r=$?
echo "SPU_DMA_REPEAT_LIMIT=256 --limit: rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -8; rc=1; }
exit $rc
