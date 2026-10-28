#!/usr/bin/env bash
# Patch 1091 host test (tools/spu/test_vm_inline.cpp): the lifted code's inline guest loads. Extracts the preamble
# block from the patched lifter (work/ps3recomp-patched or SPU_TREE=DIR; SKIP without it), builds at -O2 and checks
# load equivalence, special-address routing, poll liveness with a hoisted negative control, and that the inline loads
# are plain instructions (no libatomic call).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_vm_inline.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
L="$T/tools/ppu_lifter.py"
[[ -f "$L" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "KZ3 patch 1091 begin" "$L" || { echo "FAIL: $T has no patch 1091"; exit 1; }
sed -n '/KZ3 patch 1091 begin/,/KZ3 patch 1091 end/p' "$L" > "$O/vmi_block.h"
rm -f "$O/t_vm_inline"
${CXX:-g++} -std=gnu++17 -O2 -pthread -DVMI_BLOCK="\"$O/vmi_block.h\"" -o "$O/t_vm_inline" \
    "$ROOT/tools/spu/test_vm_inline.cpp" 2>&1 | grep -m8 error
[[ -x "$O/t_vm_inline" ]] || { echo "FAIL: build"; exit 1; }
if nm -u "$O/t_vm_inline" | grep -q '__atomic_load'; then echo "FAIL: inline loads call libatomic"; exit 1; fi
out="$(timeout 120 "$O/t_vm_inline" 2>&1)"; r=$?
echo "$out" | tail -8
[[ $r == 0 ]] && echo PASS || echo FAIL
exit $r
