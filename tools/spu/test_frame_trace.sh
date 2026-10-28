#!/usr/bin/env bash
# port/spu/kz3_frame_trace.c (KZ3_FRAME_TRACE, patch 1085) + tools/spu/frame_trace.py host test (SPU pass 66): off
# without KZ3_FRAME_TRACE or KZ3_WAIT_ACCT; events outside the window dropped; the file is written once when the
# window ends; the reader finds 4 frames of 30 ms, the release job 244 for 4/4 parks, its end -> release pass 100 us.
# Self-contained: no game data, no patched toolkit.   nix develop --command tools/sandbox.sh -- tools/spu/test_frame_trace.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
${CC:-cc} -std=gnu11 -O1 -Wall -o "$O/t_frame_trace" "$ROOT/tools/spu/test_frame_trace.c" "$ROOT/port/spu/kz3_frame_trace.c" \
  2> "$O/t_frame_trace.log" || { echo "FAIL: build"; head -20 "$O/t_frame_trace.log"; exit 1; }
"$O/t_frame_trace" "$O/t_frame_trace.bin" 2>/dev/null || exit 1
out="$(python3 "$ROOT/tools/spu/frame_trace.py" "$O/t_frame_trace.bin")" || { echo "FAIL: reader"; exit 1; }
for want in "frames 4" "30.00 ms per frame" "release job img 244 found for 4/4 parks" "end -> park release p50 100 us" \
            "img 250     1.0/frame"; do
  grep -qF -- "$want" <<< "$out" || { echo "FAIL: reader output lacks '$want'"; echo "$out" | head -30; exit 1; }
done
echo "PASS: test_frame_trace (C side + reader)"
