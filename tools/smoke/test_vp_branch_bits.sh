#!/usr/bin/env bash
# Host test for KZ3 patch 2100 (based on sp00nznet/ps3recomp PR #485 by n0madic): BRB/CLB branch on the transform
# branch bits (0x1FF8) and VP block leaders reach slot 543. Builds tools/smoke/test_vp_branch_bits.c with the patched
# toolkit's rsx_vp_decompiler.c (-O2, then ASan/UBSan when available). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vp_branch_bits.sh
# KZ3_VP_DIR=<dir> takes rsx_vp_decompiler.c/.h from that directory; KZ3_VP_OLD=1 builds only the long-program case
# (for a decompiler without the patch, where it must FAIL).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
D="${KZ3_VP_DIR:-$V}"
if [[ ! -f "$D/rsx_vp_decompiler.c" || ! -f "$V/rsx_test_programs.h" || ! -d "$PT/include" ]]; then
  echo "[test_vp_branch_bits] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
OLD=""
if [[ ${KZ3_VP_OLD:-} == 1 ]]; then OLD=-DKZ3_TEST_OLD
elif ! grep -q rsx_vp_set_branch_bits "$D/rsx_vp_decompiler.h"; then
  echo "[test_vp_branch_bits] SKIP: toolkit without patch 2100 (no rsx_vp_set_branch_bits)"; exit 0; fi
O="$ROOT/build/smoke-host/vp_branch_bits"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-format-truncation -I$D -I$V -I$PT/include $OLD"
$CC $W -O2 "$ROOT/tools/smoke/test_vp_branch_bits.c" "$D/rsx_vp_decompiler.c" -o "$O/test_vp_branch_bits" -lm
"$O/test_vp_branch_bits"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_vp_branch_bits.c" \
    "$D/rsx_vp_decompiler.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
