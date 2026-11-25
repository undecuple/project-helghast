#!/usr/bin/env bash
# Host test of patch 1166: the CRT-free frameless setjmp/longjmp on MinGW x64.
#  1. Source check in the patched toolkit: ppu_setjmp.h, spu_context.h, spu_channels.c, spu_drain.c use rc_host_setjmp/longjmp on
#     MinGW x64, and no `_setjmp(..., NULL)` code line is left (a comment may mention it).
#  2. Functional run of the header's asm (native Linux x86-64 build with -D_WIN64 -D__MINGW64__; its ms_abi asm does not depend on
#     the OS): value 0 -> 1, callee-saved GPRs and xmm6/xmm15 restored, deep and sysv_abi frames, 100000 loops, a second thread.
#     gcc and clang (whichever exist; CC=... picks one). NEGATIVE CONTROL: the header with `incl %eax` removed must FAIL.
#  3. MinGW (x86_64-w64-mingw32-gcc, `nix develop .#win`): the test with the shipped header compiles and its import table has no
#     longjmp/_setjmp (static; no Wine in the sandbox, and Wine has no CFG anyway). NEGATIVE CONTROL: the old CRT form
#     (_setjmp(b,NULL) + longjmp) built the same way does import longjmp, so the import check can tell them apart.
# Self-contained (patched toolkit only, no game data); SKIP without work/ps3recomp-patched or without patch 1166 in it.
#   nix develop .#ci --command tools/spu/test_host_jmp_win64.sh      (and again under .#win for part 3)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$P/runtime/platform/host_jmp_win64.h"
[[ -f "$H" ]] || { echo "SKIP: patch 1166 not in $P (run tools/ps3recomp-patched.sh)"; exit 77; }
O="$ROOT/work/spu/t_host_jmp"; "$ROOT/tools/safe_rm.sh" "$O" 2>/dev/null; mkdir -p "$O"
fail=0; ok() { echo "PASS $*"; }; bad() { echo "FAIL $*"; fail=1; }
# 1
R="$P/runtime"
grep -q 'rc_host_setjmp(b)' "$R/ppu/ppu_setjmp.h" && grep -q 'rc_host_longjmp((b), (v))' "$R/ppu/ppu_setjmp.h" && ok "ppu_setjmp.h PPU_SJ_HOST_* use the pair" || bad "ppu_setjmp.h"
grep -q 'define SPU_SETJMP(b) rc_host_setjmp(b)' "$R/spu/spu_context.h" && grep -q 'define SPU_LONGJMP(b, v) rc_host_longjmp((b), (v))' "$R/spu/spu_context.h" && ok "spu_context.h SPU_SETJMP/LONGJMP use the pair" || bad "spu_context.h"
for f in spu/spu_channels.c spu/spu_drain.c; do
  grep -q '^#ifndef SPU_LONGJMP' "$R/$f" && ok "$f keeps spu_context.h's SPU_LONGJMP on Windows" || bad "$f still defines its own CRT longjmp"
done
n="$(grep -rn '_setjmp *(.*NULL' "$R" --include=*.c --include=*.cpp --include=*.h --include=*.inc | grep -v '^[^:]*:[0-9]*: *\(/\?\*\|//\)' | grep -vc '^[^:]*:[0-9]*:.*/\*.*_setjmp.*\*/')"
[[ "$n" == 0 ]] && ok "no frameless _setjmp(..., NULL) code line left in runtime/" || { bad "$n frameless _setjmp(...,NULL) line(s) left:"; grep -rn '_setjmp *(.*NULL' "$R" --include=*.c --include=*.cpp --include=*.h --include=*.inc | head -5; }
# 2
sed '/_Static_assert\|static_assert/d' "$H" > "$O/host_jmp_win64.h"
sed '/_Static_assert\|static_assert/d; /incl %eax/d' "$H" > "$O/neg.h"; mkdir -p "$O/neg"; cp "$O/neg.h" "$O/neg/host_jmp_win64.h"
T="$ROOT/tools/spu/test_host_jmp_win64.c"; ran=0
for cc in ${CC:-gcc clang}; do
  command -v "$cc" >/dev/null || { echo "SKIP $cc: not installed"; continue; }
  case "$("$cc" -dumpmachine 2>/dev/null)" in x86_64-*linux*) ;; *) echo "SKIP $cc: not a native x86-64 Linux compiler"; continue;; esac
  "$cc" -O2 -D_WIN64 -D__MINGW64__ -I"$O" "$T" -o "$O/t_$cc" -lpthread 2>"$O/$cc.err" || { bad "$cc build: $(head -3 "$O/$cc.err")"; continue; }
  if "$O/t_$cc" >"$O/$cc.out" 2>&1; then ok "$cc functional run: $(tail -1 "$O/$cc.out")"; else bad "$cc functional run: $(head -3 "$O/$cc.out")"; fi
  "$cc" -O2 -D_WIN64 -D__MINGW64__ -I"$O/neg" "$T" -o "$O/n_$cc" -lpthread 2>/dev/null && { if timeout 10 "$O/n_$cc" >"$O/n_$cc.out" 2>&1; then bad "$cc negative control (no incl) passed"; else ok "$cc negative control (value 0 not mapped to 1) fails as it must"; fi; }
  ran=1
done
[[ $ran == 1 ]] || echo "SKIP functional run: no gcc/clang"
# 3
W=x86_64-w64-mingw32-gcc
if command -v $W >/dev/null; then
  $W -O2 -DWINTEST -I"$O" "$T" -o "$O/t.exe" 2>"$O/w.err" && ok "MinGW gcc builds the test with the shipped header" || bad "MinGW build: $(head -3 "$O/w.err")"
  # the shipped header, not the sed'd copy (the static assert must hold on MinGW)
  mkdir -p "$O/ship"; cp "$H" "$O/ship/"; $W -O2 -DWINTEST -I"$O/ship" "$T" -o "$O/t.exe" 2>"$O/w.err" && ok "MinGW: jmp_buf >= 256 bytes (static assert holds)" || bad "MinGW shipped header: $(head -3 "$O/w.err")"
  imp() { x86_64-w64-mingw32-objdump -p "$1" | grep -cwE '(longjmp|_setjmp|__intrinsic_setjmp)$'; }
  [[ "$(imp "$O/t.exe")" == 0 ]] && ok "t.exe imports no CRT longjmp/_setjmp" || bad "t.exe imports a CRT jump function"
  cat > "$O/old.c" <<'C'
#include <setjmp.h>
static jmp_buf jb; __attribute__((noinline)) static void f(void) { longjmp(jb, 1); }
int main(void) { if (_setjmp(jb, NULL) == 0) f(); return 0; }
C
  $W -O2 "$O/old.c" -o "$O/old.exe" 2>"$O/o.err" || bad "old-form control build: $(head -2 "$O/o.err")"
  [[ "$(imp "$O/old.exe")" -ge 1 ]] && ok "negative control: the old _setjmp(b,NULL)+longjmp form DOES import the CRT's longjmp/_setjmp" || bad "negative control: old form shows no CRT import (check is blind)"
else echo "SKIP part 3: no x86_64-w64-mingw32-gcc (nix develop .#win)"; fi
[[ $fail == 0 ]] && echo "ALL PASS" || echo "FAILED"; exit $fail
