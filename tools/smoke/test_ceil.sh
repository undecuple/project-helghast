#!/usr/bin/env bash
# Host test for KZ3 patch 2183 (LD_CEIL ceiling probes): pure parser, and rsx_vertex_fetch_many with LD_CEIL unset /
# other tokens (must convert, the negative control) and LD_CEIL=vtx (must skip). No GPU, no game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_ceil.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_vertex_compact.c" || ! -d "$PT/include" ]]; then
  echo "[test_ceil] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
if [[ ! -f "$V/rsx_ceil.h" ]]; then echo "[test_ceil] SKIP: toolkit without patch 2183"; exit 77; fi
O="$ROOT/build/smoke-host/ceil"; mkdir -p "$O"
CC="${CC:-cc}"
W=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -I"$V" -I"$PT/libs/video" -I"$PT/include")
$CC "${W[@]}" -O2 "$ROOT/tools/smoke/test_ceil.c" "$V/rsx_vertex_compact.c" -o "$O/test_ceil" -lm
"$O/test_ceil"
env -u LD_CEIL "$O/test_ceil" convert
LD_CEIL=texhash,wb,rb "$O/test_ceil" convert      # other tokens do not skip the conversion
LD_CEIL=bogus "$O/test_ceil" convert              # an unknown token enables nothing
LD_CEIL=vtx "$O/test_ceil" skip
LD_CEIL="rb, vtx" "$O/test_ceil" skip
# negative control: a build whose gate is removed must be caught by the "skip" run
sed 's/if (rsx_ceil_mask() & RSX_CEIL_VTX) return 1;//' "$V/rsx_vertex_compact.c" > "$O/vc_nogate.c"
cp "$V"/*.h "$O/" 2>/dev/null || true
$CC "${W[@]}" -O2 -I"$O" "$ROOT/tools/smoke/test_ceil.c" "$O/vc_nogate.c" -o "$O/test_ceil_nogate" -lm
if LD_CEIL=vtx "$O/test_ceil_nogate" skip >/dev/null; then echo "[test_ceil] FAIL: negative control (gate removed) not detected"; exit 1; fi
echo "[test_ceil] negative control: removed gate detected (PASS)"
echo "[test_ceil] PASS"
