#!/usr/bin/env bash
# Patch 1054 host test: tools/spu/test_timer_sched.c (absolute periodic-timer deadlines, catch-up/skip policy).
# Needs work/ps3recomp-patched (or TIMER_HDR=DIR holding sys_timer_sched.h); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_timer_sched.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
H="${TIMER_HDR:-$ROOT/work/ps3recomp-patched/runtime/syscalls}"
[[ -f "$H/sys_timer_sched.h" ]] || { echo "SKIP: no sys_timer_sched.h (patch 1054) at $H"; exit 0; }
gcc -std=gnu17 -O2 -Wall -Wextra -I "$H" -o "$O/t_timer_sched" "$ROOT/tools/spu/test_timer_sched.c" 2>&1 | grep -m5 -E "error|warning"
"$O/t_timer_sched"
