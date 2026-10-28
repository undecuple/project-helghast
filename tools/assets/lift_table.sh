#!/usr/bin/env bash
# Which override table matches the lift in RECOMP_DIR (used by tools/build-kz3.sh's stale-lift guard; 1.14 series step 4).
# usage: lift_table.sh ROOT RECOMP_DIR   -> prints the table path (absolute)
# The lift's version comes from tools/spu/kz3ver.py lift-ver (pass 149, series step 6): the lift's own stamp
# (tools/lift.sh writes "elf=...EBOOT-114.elf" / "elf=...EBOOT.elf"), else the directory name (kz3ver.py LIFT_DIRS:
# recomp = 1.14, recomp-100 = 1.00; the pre-flip name recomp-114 = 1.14), else KZ3_EBOOT_VER, else kz3ver.py DEFAULT (114).
# 1.00 keeps port/spu/ppu_overrides.txt; 1.14 uses port/spu/ppu_overrides-114.txt when it exists (after the planned
# move), else re/data/overrides/ppu_overrides-114.txt (same lookup as lift.sh).
set -euo pipefail
ROOT="$1"; DIR="${2:-}"
ver="$(cd "$ROOT" && python3 tools/spu/kz3ver.py lift-ver "$DIR")"
[[ "$ver" == 100 || "$ver" == 114 ]] || { echo "lift_table: tools/spu/kz3ver.py lift-ver gave '$ver' (needs the round-149 kz3ver.py)" >&2; exit 2; }
if [[ "$ver" == 114 ]]; then
  t="$ROOT/port/spu/ppu_overrides-114.txt"; [[ -f "$t" ]] || t="$ROOT/re/data/overrides/ppu_overrides-114.txt"
else
  t="$ROOT/port/spu/ppu_overrides.txt"
fi
echo "$t"
