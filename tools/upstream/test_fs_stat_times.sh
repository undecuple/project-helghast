#!/usr/bin/env bash
# Patch 5006 (cellFsStat/cellFsFstat times) against the patched toolkit: nix develop .#re --command tools/sandbox.sh -- tools/upstream/test_fs_stat_times.sh
# KZ3_PATCHED_DIR overrides the toolkit (default work/ps3recomp-patched). Needs no game data. Exit 1 on any FAIL.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"; [[ "$P" = /* ]] || P="$ROOT/$P"
[[ -f "$P/runtime/ppu/ppu_fs.cpp" ]] || { echo "SKIP: $P has no runtime/ppu/ppu_fs.cpp (run tools/ps3recomp-patched.sh)"; exit 0; }
RH="$ROOT/tools/upstream/stub"
O="$(mktemp -d)"; trap 'rm -rf "$O"' EXIT
INC=(-I "$RH" -I "$P" -I "$P/include" -I "$P/runtime" -I "$P/runtime/ppu" -I "$P/runtime/memory" -I "$P/libs")
g++ -std=gnu++17 -O1 -w "${INC[@]}" -o "$O/t" "$ROOT/tools/upstream/test_fs_stat_times.cpp" 2>"$O/err" || { echo "FAIL build"; head -15 "$O/err"; exit 1; }
"$O/t" "$O" >"$O/out" 2>&1; grep "^\[FAIL\]\|^PASS\|^FAIL" "$O/out"; grep -q "^PASS" "$O/out"
