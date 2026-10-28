#!/usr/bin/env bash
# Focused test for patch 5001 (EDAT reader takes the guest's klicensee from sceNpDrmIsAvailable[2]).
#   nix develop .#re --command tools/sandbox.sh -- tools/upstream/test_edat_v3.sh
# Decrypts the installed 1.14 update's added_data_ps3.psarc.edat to work/upstream-tests/ (git-ignored, never
# committed) and lists it with tools/psarc.py. Expected: v0.13.0 + 5001 PASS, then a clean psarc listing.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$ROOT/work/upstream-tests"; mkdir -p "$O"
EDAT="${EDAT:-$ROOT/GameUpdate/USRDIR/added_data_ps3.psarc.edat}"
ELF="${ELF:-$ROOT/work/bin/EBOOT-114.elf}"
OUT="$O/added_data_ps3.psarc"

T="$O/tree-5001"
rm -rf "$T"; mkdir -p "$T"
tar -C "$ROOT/third_party/ps3recomp" --exclude=.git -cf - libs include runtime | tar -C "$T" -xmf -
(cd "$T" && patch -p1 -s < "$ROOT"/port/patches/5001-*.patch) || { echo "FAIL: 5001 does not apply"; exit 1; }
# sceNp.c must still compile with the patch (syntax only: it needs the runtime headers).
gcc -std=gnu17 -w -fsyntax-only -I "$T/include" -I "$T/libs/network" \
    "$T/libs/network/sceNp.c" 2>&1 | grep -m5 "error:" && { echo "FAIL: sceNp.c"; exit 1; }
gcc -std=gnu17 -O2 -w -I "$T/libs/filesystem" -o "$O/t_edat_v3" "$ROOT/tools/upstream/test_edat_v3.c" \
    || { echo "FAIL: build"; exit 1; }
"$O/t_edat_v3" "$EDAT" "$ELF" "$OUT" 2>&1 | grep -v "^\[edat\] block\|^\[edat\]   " | tail -12
[[ "${PIPESTATUS[0]}" == 0 ]] || exit 1
python3 "$ROOT/tools/psarc.py" info "$OUT" 2>&1 | tail -5
