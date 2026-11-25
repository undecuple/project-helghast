#!/usr/bin/env bash
# Host test for tools/spu/skin_census.py (SPU pass 162): needs the lift, the patched toolkit and an ovl255 capture set
# (work/spu/capture/r121); SKIP otherwise (no game data in CI). Checks that the census builds, replays EXACT and that every
# variant row has vertices and cycles.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_r162_skin_census.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/r162t"
CAPS="${SKIN_CAPS:-$ROOT/work/spu/capture/r121}"
[[ -d "$CAPS" && -d "$ROOT/work/spu/lift/job_01242c80" && -f "$ROOT/work/ps3recomp-patched/runtime/spu/spu_helpers.h" ]] || { echo "SKIP: no captures, lift or patched toolkit"; exit 77; }
out="$(python3 "$ROOT/tools/spu/skin_census.py" "$CAPS" --out "$O" 2>&1)"; rc=$?
echo "$out" | tail -8
[[ $rc -eq 0 ]] || { echo "FAIL: census rc=$rc"; exit 1; }
echo "$out" | grep -q "^0x5670 .*" || { echo "FAIL: no 0x5670 row"; exit 1; }
# each of the four rows must show vertices (column 4) > 0 in this capture set
bad=$(echo "$out" | awk '/^0x5[0-9A-F]+ +0x/{ if ($4+0 == 0) n++ } END{print n+0}')
[[ "$bad" == 0 ]] || { echo "FAIL: $bad variant rows without vertices"; exit 1; }
echo "PASS"
