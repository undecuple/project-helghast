#!/usr/bin/env bash
# Host test for KZ3 patch 1161 (KZ3_VM_INLINE=0 turns the lifted code's inline guest loads and stores off): extracts the
# VmInlineInit block from the patched ppu_loader.cpp (work/ps3recomp-patched or SPU_TREE), compiles it with the two flags
# as plain globals and checks the flags for: unset, =1, =0, a read diagnostic (PPU_RWATCH), a store diagnostic (PT).
# SKIP without a patched toolkit that has patch 1161.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_r161_vm_inline_off.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/host-tests/vm-inline-off"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"; L="$T/runtime/ppu/ppu_loader.cpp"
grep -q "KZ3 patch 1161" "$L" 2>/dev/null || { echo "SKIP: no patched toolkit with patch 1161 at $T"; exit 77; }
{ echo '#include <cstdio>'; echo '#include <cstdlib>'
  sed -n '/^extern "C" int vm_inline_ok = 0;/,/^} s_vm_inline_init;/p' "$L" | sed 's/^extern "C" int \(vm_inline[a-z_]*\) = 0;/extern "C" { int \1 = 0; }/'
  echo 'int main() { printf("%d %d\n", vm_inline_ok, vm_inline_st_ok); return 0; }'; } > "$O/vmi_off.cpp"
${CXX:-g++} -std=gnu++17 -O1 -o "$O/t_vmi_off" "$O/vmi_off.cpp" 2>&1 | grep -m5 error
[[ -x "$O/t_vmi_off" ]] || { echo "FAIL: build"; exit 1; }
fail=0
chk() { local want="$1"; shift; local got; got="$(env -u KZ3_VM_INLINE -u PPU_RWATCH -u PT "$@" "$O/t_vmi_off")"
  [[ "$got" == "$want" ]] && echo "PASS ($*) -> $got" || { echo "FAIL ($*): got '$got' want '$want'"; fail=1; }; }
chk "1 1" env
chk "1 1" env KZ3_VM_INLINE=1
chk "0 0" env KZ3_VM_INLINE=0
chk "0 1" env PPU_RWATCH=1
chk "1 0" env PT=1
[[ $fail == 0 ]] && echo PASS || echo FAIL
exit $fail
