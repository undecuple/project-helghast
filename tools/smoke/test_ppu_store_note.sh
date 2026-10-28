#!/usr/bin/env bash
# Host test for KZ3 patch 2082 (recompiled PPU stores into pages with the NOTE bit -- the RSX local window -- call
# the renderer's page-note hook). Compiles the patch's own blocks out of the patched toolkit (ppu_loader.cpp:
# VM_STORE_NOTE + ppu_store_note*; ppu_lifter.py: PPU_ST_NOTE) into tools/smoke/test_ppu_store_note.cpp, checks
# which stores note, and times the gate (ns per 8-byte store: plain, gated page clear, gated page noted). No game data.
#   nix develop .#re --command tools/sandbox.sh -- bash tools/smoke/test_ppu_store_note.sh
# (KZ3_PATCHED_DIR=<tree> for a scratch tree; the lane could not set the executable bit: committed as 0644)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if ! grep -q 'KZ3 patch 2082 begin' "$PT/runtime/ppu/ppu_loader.cpp" 2>/dev/null; then
  echo "[test_ppu_store_note] SKIP: no patched toolkit with patch 2082 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/ppu_store_note"; mkdir -p "$O"
python3 "$ROOT/tools/smoke/ppu_store_note_extract.py" "$PT" "$O"
g++ -std=gnu++17 -O2 -Wall -Wno-unused-function -I "$O" -o "$O/test_ppu_store_note" "$ROOT/tools/smoke/test_ppu_store_note.cpp"
"$O/test_ppu_store_note"
