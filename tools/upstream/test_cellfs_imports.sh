#!/usr/bin/env bash
# Focused test, first for patch 3002 (the cellFs imports KZ3 needs). ps3recomp v0.15.0 absorbed 3002 (PR #257/#258),
# so it now runs test_cellfs_imports.cpp against upstream (third_party/ps3recomp) and the full patched tree.
#   nix develop .#re --command tools/sandbox.sh -- tools/upstream/test_cellfs_imports.sh
# Expected: both PASS (v0.13.0 without 3002 FAILed: handlers missing). ppu_fs.cpp needs the generated ppu_recomp.h
# (ppu_context), so it uses work/recomp from the PPU lift, or tools/upstream/stub (the lifter's generic preamble)
# where there is no lift (public CI). Exit 1 unless both PASS.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$ROOT/work/upstream-tests"; mkdir -p "$O"
RH="$ROOT/work/recomp"; [[ -f "$RH/ppu_recomp.h" ]] || RH="$ROOT/tools/upstream/stub"
verdict=0
for t in "upstream:$ROOT/third_party/ps3recomp" "full-series:$ROOT/work/ps3recomp-patched"; do
  name="${t%%:*}" tree="${t#*:}"
  rm -f "$O/t_fs_$name"
  (cd "$tree" && g++ -std=gnu++17 -w -I . -I include -I runtime -I runtime/ppu -I runtime/memory -I "$RH" \
     -I libs -o "$O/t_fs_$name" "$ROOT/tools/upstream/test_cellfs_imports.cpp" 2>&1 | grep -m5 "error" )
  echo "== $name"; out="$("$O/t_fs_$name" 2>&1)"; tail -7 <<<"$out"
  last="$(tail -n1 <<<"$out")"
  [[ "$last" == *PASS* ]] || { echo "want PASS on $name"; verdict=1; }
done
exit $verdict
