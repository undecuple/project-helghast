#!/usr/bin/env bash
# Patch 1058 host test: tools/spu/test_pm_select.c (the selectWorkload answer vs the WWS PM's wklCurrentId check).
# Needs work/ps3recomp-patched with patch 1058 (or SEL_HDR=DIR holding spurs_select.h); says SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_pm_select.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
H="${SEL_HDR:-$ROOT/work/ps3recomp-patched/runtime/spu}"
[[ -f "$H/spurs_select.h" ]] || { echo "SKIP: no patched toolkit with patch 1058 at $H"; exit 0; }
gcc -std=gnu17 -O2 -Wall -Wextra -I "$H" -o "$O/t_pm_select" "$ROOT/tools/spu/test_pm_select.c" 2>&1 | grep -m5 -E "error|warning"
"$O/t_pm_select"
