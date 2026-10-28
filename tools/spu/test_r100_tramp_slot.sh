#!/usr/bin/env bash
# Patch 1115 host test (SPU pass 100, tools/spu/test_r100_tramp_slot.cpp): the generated code's TEB-slot path to
# g_trampoline_fn keeps the TLS variable's per-thread semantics. Takes the SOURCE_PREAMBLE block from the patched
# lifter (SPU_TREE=DIR, default work/ps3recomp-patched); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r100_tramp_slot.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/r100t"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
L="$T/tools/ppu_lifter.py"
[[ -f "$L" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "PPU_TRAMP_TEB" "$L" || { echo "FAIL: $L has no patch 1115"; exit 1; }
python3 - "$L" "$O/preamble.h" <<'PY' || { echo "FAIL: preamble block"; exit 1; }
import sys, re
s = open(sys.argv[1]).read()
a = s.index('/* KZ3 patch 1115'); b = s.index('} while(0)', a) + len('} while(0)')
blk = s[a:b].replace('\\\\', '\\')
# the macro must follow the extern declaration (else the declaration itself would expand)
d = s.rfind('extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);', 0, a)
assert d >= 0 and s.index('#define g_trampoline_fn', a) > d
# default: MinGW x86_64 only
assert 'defined(_WIN32) && defined(__GNUC__) && !defined(_MSC_VER) && defined(__x86_64__)' in blk
open(sys.argv[2], 'w').write(blk + '\n')
PY
rm -f "$O/t_tramp"
${CXX:-g++} -std=gnu++17 -O2 -pthread -I "$O" -o "$O/t_tramp" "$ROOT/tools/spu/test_r100_tramp_slot.cpp" 2>&1 | grep -m8 error
[[ -x "$O/t_tramp" ]] || { echo "FAIL: build"; exit 1; }
timeout 60 "$O/t_tramp"; r=$?
[[ $r == 0 ]] || { echo "FAIL: rc=$r"; exit 1; }
