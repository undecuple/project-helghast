#!/usr/bin/env bash
# Host test for KZ3 patch 2160 (RSX_PERF_VTXREG classifier, libs/video/rsx_vtx_reg.h): scripted + random draw sequences, and a
# negative control (a mutated header that stops comparing bytes must be rejected). SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vtx_reg.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_vtx_reg.h" ]]; then echo "[test_vtx_reg] SKIP: toolkit without patch 2160 ($V)"; exit 0; fi
O="$ROOT/build/smoke-host/vtx_reg"; mkdir -p "$O/mut"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_vtx_reg.c" -o "$O/test_vtx_reg"
"$O/test_vtx_reg"
sed 's/if (memcmp(b, p->copy + o, spans\[i\].len)) same = 0;/(void)b;/' "$V/rsx_vtx_reg.h" > "$O/mut/rsx_vtx_reg.h"
cmp -s "$V/rsx_vtx_reg.h" "$O/mut/rsx_vtx_reg.h" && { echo "[test_vtx_reg] FAIL: mutation did not apply"; exit 1; }
${CC:-cc} -std=c11 -O2 -I"$O/mut" "$ROOT/tools/smoke/test_vtx_reg.c" -o "$O/test_vtx_reg_mut" 2>/dev/null
if "$O/test_vtx_reg_mut" >/dev/null; then echo "[test_vtx_reg] FAIL: negative control not detected"; exit 1; fi
echo "[test_vtx_reg] negative control: a classifier that ignores byte changes is rejected (PASS)"
