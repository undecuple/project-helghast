#!/usr/bin/env bash
# padrun.sh NAME SECS VIDEO EXE_STAGE_EXE PAD... : one closed-loop Proton run (PAD_FILE + tools/release/padloop.py) of an
# exe on the maintainer's first-boot profile; clip KZ3_VIDEO=VIDEO (START:SECS). Extra env passes (KZ3_EBOOT_VER...).
#   tools/release/padrun.sh rel30-opt 120 40:70 "/path/build/kz3-win/kz3.exe" boot-to-menu menu-to-options options-audio
# PAD names are files in tools/release/pad/ (without .pad). Takes the GPU lock via tools/spu/run.sh.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N=$1 S=$2 V=$3 EXE=$4; shift 4
files=(); for p in "$@"; do files+=("tools/release/pad/$p.pad"); done
mkdir -p "work/proton/$N"; rm -f "work/proton/$N/padloop.log"
python3 tools/release/padloop.py "$N" "${files[@]}" > "work/proton/$N.padloop.out" 2>&1 &
LP=$!
rc=0
env KZ3_STAGE_EXE="$EXE" PAD_FILE=padin.txt PAD_SCRIPT="9999:0x0001" KZ3_VIDEO="$V" GCM_FLIPCOUNT=1 KZ3_LOG_STDOUT=1 KZ3_NOTIFY=0 \
  tools/spu/run.sh "$N" "$S" --save-from "${KZ3_SAVE_FROM:-$PWD/work/proton/coord-firstboot}" > "work/proton/$N.run.out" 2>&1 || rc=$?
kill $LP 2>/dev/null || true
exit $rc
