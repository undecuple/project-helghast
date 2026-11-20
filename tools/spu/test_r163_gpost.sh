#!/usr/bin/env bash
# Host test for the GPU post plan P1 core (port/spu/kz3_gpost.c). Self-contained (re/data TSVs and port/ are in the repo).
# Also: the generated module table is current; the lifted runner (if work/recomp*/ exists) has the loop the model mirrors.
set -u
cd "$(dirname "$0")/../.." || exit 1
OUT=${TMPDIR:-/tmp}/kz3_r163_$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
fail=0
python3 tools/spu/gen_gpost_modva.py --check || { echo "FAIL: port/spu/kz3_gpost_modva.h is stale (run tools/spu/gen_gpost_modva.py)"; fail=1; }
CC=${CC:-cc}
$CC -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    tools/spu/test_r163_gpost.c port/spu/kz3_gpost.c -o "$OUT/t" || { echo "FAIL: build"; exit 1; }
"$OUT/t" port/spu/kz3_ppu_overrides.cpp || fail=1
# the model mirrors the lifted runner: check the offsets in the lifted source when a lift is present (else SKIP)
chk() { # file func
    [ -f "$1" ] || return 2
    awk -v f="void $2(ppu_context" 'index($0,f)==1{on=1;next} on&&/^void func_/{exit} on' "$1" |
        grep -q -e '0x18D8' && return 0 || return 1
}
for pair in "work/recomp/ppu_recomp_006.cpp func_0020C010" "work/recomp-100/ppu_recomp_008.cpp func_002006C0"; do
    set -- $pair
    chk "$1" "$2"; r=$?
    if [ $r = 2 ]; then echo "SKIP lifted-source check ($2: no lift in work/)"; 
    elif [ $r = 0 ]; then
        body=$(awk -v f="void $2(ppu_context" 'index($0,f)==1{on=1;next} on&&/^void func_/{exit} on' "$1")
        ok=1
        for k in '+ 0x18D4' '+ 0x18D8' '+ 0x258' '+ 0x14)' '+ 0xC)' '+ 0x10)'; do
            case "$body" in *"$k"*) ;; *) ok=0; echo "FAIL lifted $2 lacks $k"; fail=1;; esac
        done
        [ $ok = 1 ] && echo "ok lifted-source offsets $2"
    else echo "FAIL lifted $2: no 0x18D8"; fail=1; fi
done
[ $fail = 0 ] && echo "test_r163_gpost: PASS" || echo "test_r163_gpost: FAIL"
exit $fail
