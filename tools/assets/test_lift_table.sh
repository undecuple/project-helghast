#!/usr/bin/env bash
# Host test (no game data): the build guard's table choice (tools/assets/lift_table.sh) and the 1.00/1.14 row counts.
set -euo pipefail
H="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$H/../.." && pwd)"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fail=0; chk() { [[ "$2" == "$3" ]] || { echo "FAIL $1: got $2 want $3"; fail=1; }; }
rel() { echo "${1#"$ROOT"/}"; }
mkdir -p "$T/recomp" "$T/recomp-114" "$T/recomp-100" "$T/x"
P100=port/spu/ppu_overrides.txt P114=re/data/overrides/ppu_overrides-114.txt
[[ -f "$ROOT/port/spu/ppu_overrides-114.txt" ]] && P114=port/spu/ppu_overrides-114.txt
lt() { rel "$(env -u KZ3_EBOOT_VER "$@" "$H/lift_table.sh" "$ROOT" "$DIRARG")"; }
# Default version (kz3ver.py DEFAULT) for a directory with no stamp and no known name, and for no directory.
dflt="$(cd "$ROOT" && env -u KZ3_EBOOT_VER python3 tools/spu/kz3ver.py default)"
[[ "$dflt" == 114 ]] && PD=$P114 || PD=$P100
DIRARG="$T/x";          chk default "$(lt)" "$PD"
DIRARG="";              chk nodir "$(lt)" "$PD"
DIRARG="$T/x";          chk env100 "$(lt KZ3_EBOOT_VER=100)" "$P100"
DIRARG="$T/x";          chk env114 "$(lt KZ3_EBOOT_VER=114)" "$P114"
# Directory names (kz3ver.py LIFT_DIRS, pass 149): recomp = 1.14, recomp-100 = 1.00, the old recomp-114 = 1.14; the
# name wins over KZ3_EBOOT_VER.
DIRARG="$T/recomp";     chk dir-recomp "$(lt KZ3_EBOOT_VER=100)" "$P114"
DIRARG="$T/recomp-100"; chk dir-recomp-100 "$(lt KZ3_EBOOT_VER=114)" "$P100"
DIRARG="$T/recomp-114"; chk dir-recomp-114 "$(lt KZ3_EBOOT_VER=100)" "$P114"
# The stamp wins over the name.
echo "elf=work/bin/EBOOT-114.elf overrides=x" > "$T/x/.lift-stamp"
DIRARG="$T/x";          chk stamp114 "$(lt KZ3_EBOOT_VER=100)" "$P114"
echo "elf=work/bin/EBOOT.elf overrides=x" > "$T/recomp/.lift-stamp"
DIRARG="$T/recomp";     chk stamp100-in-recomp "$(lt)" "$P100"
echo "elf=work/bin/EBOOT-114.elf overrides=x" > "$T/recomp-100/.lift-stamp"
DIRARG="$T/recomp-100"; chk stamp114-in-recomp-100 "$(lt)" "$P114"
# row counts: every 1.00 row with a 114 twin has the twin row
n() { grep -cE '^[0-9A-Fa-f]{8}[[:space:]]' "$1"; }
a="$(n "$ROOT/port/spu/ppu_overrides.txt")"; b="$(n "$ROOT/re/data/overrides/ppu_overrides-100.txt")"
[[ "$a" == "$b" ]] || echo "note: port/spu/ppu_overrides.txt has $a rows, re/data -100 copy $b (spu added rows; re-data syncs the copy)"
tw="$(grep -E '^[0-9A-Fa-f]{8}[[:space:]]' "$ROOT/re/data/overrides/ppu_overrides-100.txt" | grep -c '\[114=')"
ob="$(grep -E '^[0-9A-Fa-f]{8}[[:space:]]' "$ROOT/re/data/overrides/ppu_overrides-114.txt" | grep -c '\[100=')"
chk twins "$tw" "$ob"
echo "100: $a rows ($tw with a 1.14 twin); 114: $(n "$ROOT/re/data/overrides/ppu_overrides-114.txt") rows ($ob with a 1.00 twin)"
[[ $fail == 0 ]] && echo PASS || exit 1
