#!/usr/bin/env bash
# Patch 1074 host test: tools/spu/test_spurs_kern.c (the per-SPU SPURS kernel's selection rule, poll answer and idle
# wake, runtime/spu/spurs_kern_sched.h). Needs work/ps3recomp-patched with patch 1074 (or KERN_HDR=DIR holding
# spurs_kern_sched.h); says SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spurs_kern.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
H="${KERN_HDR:-$ROOT/work/ps3recomp-patched/runtime/spu}"
[[ -f "$H/spurs_kern_sched.h" ]] || { echo "SKIP: no patched toolkit with patch 1074 at $H"; exit 0; }
gcc -std=gnu17 -O2 -Wall -Wextra -pthread -I "$H" -o "$O/t_spurs_kern" "$ROOT/tools/spu/test_spurs_kern.c" 2>&1 | grep -m5 -E "error|warning"
[[ -x "$O/t_spurs_kern" ]] || { echo "FAIL: build"; exit 1; }
"$O/t_spurs_kern"
