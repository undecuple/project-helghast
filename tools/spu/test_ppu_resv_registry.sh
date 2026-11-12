#!/usr/bin/env bash
# Host test for patch 3009 (PPU reservation registry idempotent). Extracts the real registration/invalidation code from
# the patched ppu_loader.cpp (anchors fail loudly if it is refactored), builds it with test_ppu_resv_registry.cpp (from the
# Ratchet & Clank port, MIT) and runs baseline/churn/concurrent/capacity/overflow. No unresolved-symbol link flags.
# NEGATIVE CONTROL: the same test against a copy of ppu_loader.cpp with patch 3009 reversed (git apply -R) must FAIL
# (churn: a distinct victim stays reserved after 256 re-registrations of one slot). Self-contained: no game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_ppu_resv_registry.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; HERE="$ROOT/tools/spu"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/ppu/ppu_loader.cpp" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
O="$ROOT/work/spu/t_resv"; "$ROOT/tools/safe_rm.sh" "$O" 2>/dev/null; mkdir -p "$O"
CXX="${CXX:-c++}"; command -v "$CXX" >/dev/null || CXX=g++
build() {  # build TREE OUTDIR -> OUTDIR/t
  python3 - "$1/runtime/ppu/ppu_loader.cpp" "$2/ppu_resv_runtime.inc" <<'PY' || return 1
from pathlib import Path
import re, sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
prefix = re.search(r'^#define PPU_RESV_MAX\b.*?(?=^/\* Where every guest thread is)', source, re.M | re.S)
if not prefix or 'void ppu_resv_register(' not in prefix.group():
    raise SystemExit("reservation registry extraction anchors changed; review the host reproduction")
parts = [source[:source.index('*/') + 2], prefix.group()]
for name in ('ppu_resv_break_store', 'ppu_resv_break_line', 'ppu_resv_break'):
    m = list(re.finditer(r'^(?:extern "C"|static inline) void ' + name + r'\([^\n]*\)\s*(?:/\*[^\n]*\*/)?\n\{.*?^\}', source, re.M | re.S))
    if len(m) != 1: raise SystemExit(f"expected one definition of {name}")
    parts.append(m[0].group())
Path(sys.argv[2]).write_text('\n\n'.join(parts) + '\n', encoding="utf-8")
PY
  "$CXX" -std=c++17 -O1 -Wall -Wextra -pthread -I"$P" -I"$2" "$HERE/test_ppu_resv_registry.cpp" -o "$2/t" || return 1
}
run_all() {  # run_all OUTDIR -> 0 when every mode passes
  local d="$1" rc=0
  for m in baseline churn concurrent capacity; do "$d/t" $m || rc=1; done
  ( ulimit -c 0; "$d/t" overflow-child 2>"$d/ov.err"; r=$?
    [[ $r -ne 0 ]] && grep -q 'reservation registry exhausted: capacity=128' "$d/ov.err" ) || { echo "FAIL: overflow did not abort explicitly"; rc=1; }
  return $rc
}
build "$P" "$O" || { echo "FAIL: build of the patched runtime test"; exit 1; }
run_all "$O" || { echo "FAIL: patched tree"; exit 1; }
# negative control: reverse patch 3009 in a scratch copy of ppu_loader.cpp
N="$O/neg"; mkdir -p "$N/runtime/ppu"; cp "$P/runtime/ppu/ppu_loader.cpp" "$N/runtime/ppu/"
# GNU patch, not git apply: in lane worktrees work/spu is a symlink to the main checkout's, and git apply refuses
# paths "beyond a symbolic link" (lane/rsx merge, 2026-11-11).
if ! patch -s -R -p1 --no-backup-if-mismatch -d "$N" < "$ROOT/port/patches/3009-ppu-reservation-registry-idempotent.patch" >"$O/rev.err" 2>&1; then
  echo "FAIL: cannot reverse patch 3009 for the negative control: $(head -2 "$O/rev.err")"; exit 1; fi
mkdir -p "$O/negb"; build "$N" "$O/negb" 2>/dev/null || { echo "FAIL: negative-control build"; exit 1; }
if run_all "$O/negb" >"$O/neg.out" 2>&1; then echo "FAIL: negative control passed (patch reversed) - the test proves nothing"; exit 1; fi
echo "PASS: patch 3009 registry test passes; reversed patch fails ($(grep -c '^FAIL' "$O/neg.out") failing checks)"
