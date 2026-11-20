#!/usr/bin/env bash
# Host test for KZ3 patch 2195 ([o0-vtx], RSX_O0_VTX_RANGE): the pure core in libs/video/rsx_o0.h, plus static checks that
# the patched sources read the switch and hook the three stages. SKIP without a patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_o0_vtx.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if ! grep -qs rsx_o0v_fmt "$V/rsx_o0.h"; then echo "[test_o0_vtx] SKIP: no patched toolkit with patch 2195 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/o0-vtx"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_o0_vtx.c" -o "$O/t" -lm
"$O/t" 2>&1 | grep -v '^\[o0-vtx\] RSX_O0_VTX_RANGE='
test "${PIPESTATUS[0]}" -eq 0
for k in 'getenv("RSX_O0_VTX_RANGE")' 'rsx_o0v_fmt('; do grep -qF "$k" "$V/cellGcmSys.c" || { echo "[FAIL] not in cellGcmSys.c: $k"; exit 1; }; done
for k in 'ld_o0v_spans(' 'rsx_o0v_draw(' 'o0v_hash' 'o0v_conv' 'o0v_up'; do grep -qF "$k" "$V/rsx_live_draw.c" || { echo "[FAIL] not in rsx_live_draw.c: $k"; exit 1; }; done
echo "[test_o0_vtx] static checks ok"
