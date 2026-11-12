#!/usr/bin/env bash
# Host test for KZ3 patch 2162 (SSE2 CMP32/HALF vertex conversion): bit-exact against the scalar loops and fetch_one,
# a negative control (a build with a deliberate kernel bug must FAIL), an ASan/UBSan pass and a micro-benchmark.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vtx_simd.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_vertex_compact.c" || ! -d "$PT/include" ]]; then
  echo "[test_vtx_simd] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q RSX_VTX_PATCH_2162 "$V/rsx_vertex_compact.h"; then echo "[test_vtx_simd] SKIP: toolkit without patch 2162"; exit 0; fi
O="$ROOT/build/smoke-host/vtx_simd"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -I$V -I$PT/libs/video -I$PT/include"
SRCS="$ROOT/tools/smoke/test_vtx_simd.c $V/rsx_vertex_compact.c"
$CC $W -O2 $SRCS -o "$O/test_vtx_simd" -lm
"$O/test_vtx_simd" | grep -v "^\[PASS\]\|^\[time\]" ; "$O/test_vtx_simd" | grep "^\[time\]"
$CC $W -O2 -DRSX_VTX_SIMD_TEST_FLAW $SRCS -o "$O/test_vtx_simd_flaw" -lm
if "$O/test_vtx_simd_flaw" >/dev/null; then echo "[test_vtx_simd] FAIL: negative control (flawed kernels) not detected"; exit 1; fi
echo "[test_vtx_simd] negative control: flawed kernels detected (PASS)"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $SRCS -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
