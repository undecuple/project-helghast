#!/usr/bin/env bash
# Pass 123: the LightProbes native kernels (port/spu/kz3_native_lp.c, slots 8-15) against the lifted functions on
# synthetic states (tools/spu/native_diff.py). Needs the game lift (SKIP without it, as native_diff.py does).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
out=$(python3 "$ROOT/tools/spu/native_diff.py" --slot 8,9,12,13 --trials 200 2>&1); rc=$?
echo "$out" | tail -4
case "$out" in *SKIP*) exit 0;; esac
[[ $rc -eq 0 ]] && echo "PASS" || { echo "FAIL"; exit 1; }
