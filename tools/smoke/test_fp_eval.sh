#!/usr/bin/env bash
# Host test for tools/smoke/fp_eval.py (RSX pass 43): a hand-assembled fragment program on a synthetic
# patch-2043 dump; checks decoding (abs/neg/scale/sat), nearest sampling and the gamma hypotheses.
# No game data, no GPU, no patched toolkit needed (numpy only: SKIP without it).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_eval.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
python3 -c 'import numpy' 2>/dev/null || { echo "[test_fp_eval] SKIP: python3 without numpy"; exit 0; }
python3 "$ROOT/tools/smoke/test_fp_eval.py"
