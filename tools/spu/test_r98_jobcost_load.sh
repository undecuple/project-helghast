#!/usr/bin/env bash
# Patch 1114 host test under load: runs the already built t_jobcost (tools/spu/test_r98_jobcost.sh builds it) N times
# in parallel next to B CPU burners (the merge gate runs host tests at -j8), and reports how many runs failed.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r98_jobcost_load.sh [N=10] [B=2 x nproc]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/jc-tests"
[[ -x "$O/t_jobcost" ]] || { echo "SKIP: build first with tools/spu/test_r98_jobcost.sh"; exit 0; }
N="${1:-10}"; B="${2:-$(( $(nproc) * 2 ))}"
rm -f "$O"/load_*.log
burners=()
for ((i = 0; i < B; i++)); do timeout 60 sh -c 'while :; do :; done' & burners+=($!); done
bad=0
for ((r = 0; r < N; r += 5)); do   # batches of 5 parallel runs
    pids=()
    for ((k = r; k < r + 5 && k < N; k++)); do "$O/t_jobcost" >"$O/load_$k.log" 2>&1 & pids+=($!); done
    wait "${pids[@]}"
done
kill "${burners[@]}" 2>/dev/null; wait 2>/dev/null
for ((k = 0; k < N; k++)); do grep -q '^PASS (0 failed)' "$O/load_$k.log" || { bad=$((bad + 1)); grep -m3 FAIL "$O/load_$k.log"; }; done
echo "== test_r98_jobcost under load ($N runs, $B burners): $((N - bad)) clean, $bad failed"
exit $((bad != 0))
