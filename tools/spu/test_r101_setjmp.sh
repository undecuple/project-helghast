#!/usr/bin/env bash
# Patch 1116 host test (SPU pass 101): guest setjmp/longjmp host frames. Lifts a synthetic Lua-pcall-shaped guest
# program with the toolkit's ppu_lifter.py (tools/spu/test_r101_setjmp_gen.py) and runs it against the toolkit's
# runtime/ppu/ppu_setjmp_rt.inc (tools/spu/test_r101_setjmp.cpp). No game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r101_setjmp.sh
# SPU_TREE=DIR: toolkit (default work/ps3recomp-patched; SKIP without it). EXPECT_FAIL=1: a tree without patch 1116
# must fail every case but the control (the mutation check).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/r101t"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/tools/ppu_lifter.py" ]] || { echo "SKIP: no toolkit at $T"; exit 0; }
python3 "$ROOT/tools/spu/test_r101_setjmp_gen.py" "$T" "$O" || { echo "FAIL: lift"; exit 1; }
rm -f "$O/t_sj"
${CXX:-g++} -std=gnu++17 -O2 -pthread -Wno-unused-function -I "$O" -I "$T/runtime/ppu" -o "$O/t_sj" \
  "$ROOT/tools/spu/test_r101_setjmp.cpp" 2>&1 | grep -m8 -E "error|undefined"
[[ -x "$O/t_sj" ]] || { echo "FAIL: build"; exit 1; }
timeout 120 "$O/t_sj" > "$O/out.txt" 2> "$O/err.txt"; r=$?
cat "$O/out.txt"; echo "stderr lines: $(wc -l < "$O/err.txt") (first: $(head -1 "$O/err.txt" | cut -c1-150))"
if [[ "${EXPECT_FAIL:-0}" == 1 ]]; then
  n=$(grep -c '^FAIL [a-z]' "$O/out.txt"); [[ $r != 0 && $n -ge 6 ]] && { echo "EXPECTED FAIL: $n cases"; exit 0; }
  echo "FAIL: expected the unpatched tree to fail 6 cases (rc=$r, $n failed)"; exit 1
fi
[[ $r == 0 ]] || { echo "FAIL: rc=$r"; exit 1; }
