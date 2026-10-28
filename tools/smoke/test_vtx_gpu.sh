#!/usr/bin/env bash
# Host test for KZ3 patch 2076 (GPU-side vertex fetch, LD_VTX_GPU): builds tools/smoke/test_vtx_gpu.c with
# rsx_vtx_gpu.h and rsx_vertex_compact.c of the patched toolkit and runs it (-O2, then an ASan/UBSan pass). The
# shader's decode text is compiled as C and compared with the CPU fetch bit for bit. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vtx_gpu.sh
# KZ3_VTX_GPU_DIR=<dir> takes rsx_vtx_gpu.h from that directory instead of the toolkit's (scratch edits).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
G="${KZ3_VTX_GPU_DIR:-$V}"
if [[ ! -f "$V/rsx_vertex_compact.c" || ! -d "$PT/include" ]]; then
  echo "[test_vtx_gpu] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if [[ ! -f "$G/rsx_vtx_gpu.h" ]]; then
  echo "[test_vtx_gpu] SKIP: toolkit without patch 2076 (no rsx_vtx_gpu.h)"; exit 0; fi
O="$ROOT/build/smoke-host/vtx_gpu"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -I$G -I$V -I$PT/include"
$CC $W -O2 "$ROOT/tools/smoke/test_vtx_gpu.c" "$V/rsx_vertex_compact.c" -o "$O/test_vtx_gpu" -lm
"$O/test_vtx_gpu" | grep -v "^\[PASS\] part"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_vtx_gpu.c" \
    "$V/rsx_vertex_compact.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
