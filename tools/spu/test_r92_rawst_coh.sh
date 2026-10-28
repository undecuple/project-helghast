#!/usr/bin/env bash
# Host test for KZ3 patch 1111 (lifted PPU raw stores commit through ppu_st_coh on an SPU-reserved line; from
# JonathanDC64/ps3recomp PR #38, adapted). No game data.
#   nix develop .#re --command tools/sandbox.sh -- bash tools/spu/test_r92_rawst_coh.sh
# KZ3_PATCHED_DIR=<tree> for another tree (work/spu/r92/pre = the stack before 1111: the reserved cases fail).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/tools/ppu_lifter.py" ]]; then
  echo "[test_r92_rawst_coh] SKIP: no patched toolkit (run tools/ps3recomp-patched.sh)"; exit 0; fi
python3 "$ROOT/tools/spu/test_r92_rawst_coh.py" "$PT" "$ROOT/build/smoke-host/r92_rawst_coh"
