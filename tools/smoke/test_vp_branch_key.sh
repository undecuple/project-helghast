#!/usr/bin/env bash
# Host test for KZ3 patch 2077 (the VP start slot in the pipeline key): builds tools/smoke/test_vp_branch_key.c with
# rsx_vp_decompiler.c of the patched toolkit and runs it (-O2, then an ASan/UBSan pass when available).
# Two pipeline records that differ only in vp_start: a branching program decompiles to different HLSL and gets two
# keys, a branch-free one decompiles to the same HLSL and keeps one key. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vp_branch_key.sh
# KZ3_VP_DIR=<dir> takes rsx_vp_decompiler.c/.h from that directory instead of the toolkit's (scratch edits).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
D="${KZ3_VP_DIR:-$V}"
if [[ ! -f "$D/rsx_vp_decompiler.c" || ! -d "$PT/include" ]]; then
  echo "[test_vp_branch_key] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q rsx_vp_key_branch_base "$D/rsx_vp_decompiler.h"; then
  echo "[test_vp_branch_key] SKIP: toolkit without patch 2077 (no rsx_vp_key_branch_base)"; exit 0; fi
O="$ROOT/build/smoke-host/vp_branch_key"; mkdir -p "$O"
CC="${CC:-cc}"
# -Wno-format-truncation: the toolkit decompiler's own snprintf into a 256-byte buffer warns under -O2 fortify
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-format-truncation -I$D -I$V -I$PT/include"
$CC $W -O2 "$ROOT/tools/smoke/test_vp_branch_key.c" "$D/rsx_vp_decompiler.c" -o "$O/test_vp_branch_key" -lm
"$O/test_vp_branch_key"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_vp_branch_key.c" \
    "$D/rsx_vp_decompiler.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
