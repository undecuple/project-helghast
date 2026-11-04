#!/usr/bin/env bash
# Host test for KZ3 patch 2148 (LD_CB_REUSE): the bytes the shader reads from the constant-buffer slot are identical with
# reuse on and off over a synthetic draw sequence. Builds tools/smoke/test_ld_cb_reuse.c with the patched toolkit's
# rsx_dispatch.c and rsx_cb_reuse.h. No game data, no GPU; SKIP without the patched toolkit or without patch 2148.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_ld_cb_reuse.sh
# KZ3_PATCHED_DIR may point at another toolkit tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_dispatch.c" || ! -d "$PT/include" ]]; then
  echo "[test_ld_cb_reuse] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if [[ ! -f "$V/rsx_cb_reuse.h" ]]; then
  echo "[test_ld_cb_reuse] SKIP: toolkit without patch 2148"; exit 0; fi
O="$ROOT/build/smoke-host/ld_cb_reuse"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 -I"$V" -I"$PT/include" \
  "$ROOT/tools/smoke/test_ld_cb_reuse.c" "$V/rsx_dispatch.c" -o "$O/t" -lm
"$O/t" | tail -8
[[ ${PIPESTATUS[0]} -eq 0 ]] || { echo "[test_ld_cb_reuse] FAIL"; exit 1; }
echo "[test_ld_cb_reuse] PASS"
