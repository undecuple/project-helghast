#!/usr/bin/env bash
# Host test for KZ3 patch 2099 (D3D12 WARP fallback rule): tools/smoke/test_d3d12_warp.c against the patched
# toolkit's libs/video/rsx_d3d12_device.h. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_d3d12_warp.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_d3d12_device.h" ]]; then echo "[test_d3d12_warp] SKIP: toolkit without patch 2099"; exit 0; fi
O="$ROOT/build/smoke-host/d3d12_warp"; mkdir -p "$O"
cc -std=c11 -O1 -Wall -Wextra -I"$V" "$ROOT/tools/smoke/test_d3d12_warp.c" -o "$O/t"
"$O/t"
