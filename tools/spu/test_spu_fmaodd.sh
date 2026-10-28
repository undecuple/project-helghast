#!/usr/bin/env bash
# Patch 1106 host test: test_spu_fmaodd_tu.c built twice (-DSPU_FMA_ODD4=0 -> old_*, =1 -> new_*) with the Windows
# build's SPU ISA (-msse4.1 -mavx2 -mfma), linked with test_spu_fmaodd.c. Needs an FMA host CPU (else SKIP). No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_fmaodd.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t_fmaodd"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_helpers.h" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
grep -q kz3-fmaodd "$P/runtime/spu/spu_helpers.h" || { echo "SKIP: patch 1106 not in $P"; exit 0; }
grep -qw fma /proc/cpuinfo || { echo "SKIP: host CPU without FMA"; exit 0; }
F=(-std=gnu17 -O2 -msse4.1 -mavx2 -mfma -w -ffp-contract=off -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime")
gcc "${F[@]}" -c -DSPU_FMA_ODD4=0 -DTU_NAME=old -DTU_MAIN_STUBS -o "$O/old.o" "$ROOT/tools/spu/test_spu_fmaodd_tu.c" || exit 1
gcc "${F[@]}" -c -DSPU_FMA_ODD4=1 -DTU_NAME=new -o "$O/new.o" "$ROOT/tools/spu/test_spu_fmaodd_tu.c" || exit 1
gcc "${F[@]}" -o "$O/t" "$ROOT/tools/spu/test_spu_fmaodd.c" "$O/old.o" "$O/new.o" -lm || exit 1
"$O/t"
