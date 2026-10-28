#!/usr/bin/env bash
# Patch 1136 host test (tools/spu/test_r112_sched.c): the P-core mask choice. No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r112_sched.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$P/runtime/spu/kz3_spu_sched.h" ]]; then echo "SKIP: $P has no patch 1136"; exit 0; fi
gcc -std=gnu17 -O2 -Wall -I "$P/runtime/spu" -o "$O/t_r112_sched" "$ROOT/tools/spu/test_r112_sched.c" && "$O/t_r112_sched"
