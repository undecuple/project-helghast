#!/usr/bin/env bash
# Patch 1052 host tests: tools/spu/test_spu_event_race.c (SN/LR raises and acks racing on two host threads, old bare
# read-modify-write as the negative control vs the atomic helpers) and test_spu_coh_registry.c (no duplicate entries, patch 1109's growable registry). Needs work/ps3recomp-patched (or SPU_TREE=DIR, SPU_HDR=DIR).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_event_race.sh [ITERS]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"; H="${SPU_HDR:-$P/runtime/spu}"
[[ -f "$H/spu_context.h" ]] || { echo "SKIP: no patched toolkit at $H"; exit 0; }
grep -q spu_event_raise "$H/spu_context.h" || { echo "FAIL: $H/spu_context.h has no spu_event_raise (patch 1052)"; exit 1; }
gcc -std=gnu17 -O2 -w -pthread -I "$H" -I "$P/include" -I "$P/runtime/platform" \
    -o "$O/t_event_race" "$ROOT/tools/spu/test_spu_event_race.c" 2>&1 | grep -m5 error
"$O/t_event_race" "$@"; rc=$?
gcc -std=gnu17 -O2 -w -pthread -I "$H" -I "$P/include" -I "$P/runtime/platform" -I "$P/runtime/ppu" \
    -o "$O/t_coh_registry" "$ROOT/tools/spu/test_spu_coh_registry.c" 2>&1 | grep -m5 error
"$O/t_coh_registry" || rc=1
exit $rc
