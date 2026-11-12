#!/usr/bin/env bash
# Host test for KZ3 patch 2153 (LD_RING_FENCE_WAIT): the mark arithmetic in rsx_ring_marks.h (wrap at the ring end,
# marks spanning the wrap point, out-of-order fences rejected, empty ring, randomized ring vs a per-slot reference).
# Builds tools/smoke/test_ld_ring_marks.c against the patched toolkit's header. No game data, no GPU; SKIP without the
# patched toolkit or without patch 2153.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_ld_ring_marks.sh
# KZ3_PATCHED_DIR may point at another toolkit tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -d "$V" ]]; then
  echo "[test_ld_ring_marks] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if [[ ! -f "$V/rsx_ring_marks.h" ]]; then
  echo "[test_ld_ring_marks] SKIP: toolkit without patch 2153"; exit 0; fi
O="$ROOT/build/smoke-host/ld_ring_marks"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_ld_ring_marks.c" -o "$O/t"
"$O/t" | tail -8
[[ ${PIPESTATUS[0]} -eq 0 ]] || { echo "[test_ld_ring_marks] FAIL"; exit 1; }
echo "[test_ld_ring_marks] PASS"
