#!/usr/bin/env bash
# Patch 1061 host test (tools/spu/test_spu_cbea.c): rdch RdEventStat masked, PUTLLUC no self-Lr (default on),
# RdMachStat = IE latch (opt-in KZ3_SPU_MACHSTAT_IE=1; CBEA v1.01 s.9.11.1, s.9.8). Runs: clean (must pass), the latch
# on with --ie-latch (must pass), then one negative control per piece (KZ3_SPU_EVSTAT_UNMASKED=1,
# KZ3_SPU_PUTLLUC_SELF_LR=1, and --ie-latch with the latch switch unset): each must FAIL in its own case only. Needs work/ps3recomp-patched (SPU_TREE=DIR).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_cbea.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q kz3_evstat_unmasked "$T/runtime/spu/spu_channels.c" || { echo "FAIL: $T has no patch 1061"; exit 1; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_spu_cbea" \
    "$ROOT/tools/spu/test_spu_cbea.c" $B $extra -lm 2>&1 | grep -m8 error)
[[ -x "$O/t_spu_cbea" ]] || { echo "FAIL: build"; exit 1; }
rc=0
out="$(env -u KZ3_SPU_EVSTAT_UNMASKED -u KZ3_SPU_MACHSTAT_OLD -u KZ3_SPU_PUTLLUC_SELF_LR "$O/t_spu_cbea" 2>&1)"; r=$?
echo "clean: rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -8; rc=1; }
out="$(KZ3_SPU_MACHSTAT_IE=1 "$O/t_spu_cbea" --ie-latch 2>&1)"; r=$?
echo "KZ3_SPU_MACHSTAT_IE=1 --ie-latch: rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -8; rc=1; }
# Negative controls: each old behaviour fails its own case only (machstat: the latch asserted with the switch off).
for sw in KZ3_SPU_EVSTAT_UNMASKED:evstat KZ3_SPU_MACHSTAT_IE:machstat KZ3_SPU_PUTLLUC_SELF_LR:putlluc; do
  v="${sw%%:*}"; c="${sw#*:}"
  if [[ $c == machstat ]]; then out="$(env -u KZ3_SPU_MACHSTAT_IE "$O/t_spu_cbea" --ie-latch 2>&1)"; r=$?; v="(KZ3_SPU_MACHSTAT_IE unset, --ie-latch)"
  else out="$(env "$v=1" "$O/t_spu_cbea" 2>&1)"; r=$?; v="$v=1"; fi
  fails="$(grep -c '^FAIL:' <<<"$out")"; other="$(grep '^FAIL:' <<<"$out" | grep -vc "FAIL: $c ")"
  if [[ $r != 0 && $fails -gt 0 && $other == 0 ]]; then echo "$v: fails $c only ($fails checks): negative control OK"
  else echo "$v: expected only case $c to fail (rc=$r fails=$fails other=$other)"; echo "$out" | head -6; rc=1; fi
done
exit $rc
