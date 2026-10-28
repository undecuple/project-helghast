#!/usr/bin/env bash
# Host test for KZ3 patch 2041 (ZSTENCIL_CLEAR_VALUE decode by zeta format, Z16 vs Z24S8): builds
# tools/smoke/test_zclear.c against the patched toolkit's libs/video/rsx_dispatch.h and runs it. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_zclear.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_ZCLEAR_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_dispatch.h" || ! -d "$PT/include" ]]; then
  echo "[test_zclear] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q rsx_dsp_zclear_decode "$V/rsx_dispatch.h"; then
  echo "[test_zclear] SKIP: toolkit without patch 2041 (no rsx_dsp_zclear_decode)"; exit 0; fi
O="$ROOT/build/smoke-host/zclear"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I"$V" -I"$PT/include" \
  "$ROOT/tools/smoke/test_zclear.c" -o "$O/test_zclear" -lm
"$O/test_zclear" | grep -v "^\[PASS\]"
