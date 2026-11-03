#!/usr/bin/env bash
# Host test for KZ3 patch 2139 (rsx_vertex_refs_from_indices): builds tools/smoke/test_vtx_refs.c
# with libs/video/rsx_vertex_compact.c of the patched toolkit and runs it (-O2, then an ASan/UBSan pass). No game
# data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vtx_refs.sh
# KZ3_VTX_DIR=<dir> tests that directory's rsx_vertex_compact.c/.h instead of the toolkit's (scratch edits).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_vertex_compact.c" || ! -d "$PT/include" ]]; then
  echo "[test_vtx_refs] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q rsx_vertex_refs_from_indices "$V/rsx_vertex_compact.h"; then
  echo "[test_vtx_refs] SKIP: toolkit without patch 2139"; exit 0; fi
O="$ROOT/build/smoke-host/vtx_refs"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=c11 -Wall -Wextra -Werror -Wno-unused-function -I$V -I$PT/libs/video -I$PT/include"
$CC $W -O2 "$ROOT/tools/smoke/test_vtx_refs.c" "$V/rsx_vertex_compact.c" -o "$O/test_vtx_refs" -lm
out="$("$O/test_vtx_refs")" || { echo "$out"; echo "[FAIL] test_vtx_refs"; exit 1; }
printf '%s\n' "$out" | grep -v "^\[PASS\]" || true
echo "[PASS] test_vtx_refs"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$ROOT/tools/smoke/test_vtx_refs.c" \
    "$V/rsx_vertex_compact.c" -o "$O/test_asan" -lm
  "$O/test_asan" >/dev/null && echo "[PASS] ASan/UBSan pass" || { echo "[FAIL] ASan/UBSan pass"; exit 1; }
fi
