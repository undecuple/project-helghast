#!/usr/bin/env bash
# Host test, first for patch 2060; since ps3recomp v0.15.0 for upstream's cellFontInitializeWithRevision (PR #324,
# 2060 dropped): the function initialises the font library, and upstream's NID generator registers NID 0xF03DCC29
# (upstream's tools/test_font_hle_nids.py, run in the patched tree). No game data, no fonts.
# Builds libs/font/cellFont.c + cellFontOutline.c from work/ps3recomp-patched (run tools/ps3recomp-patched.sh first).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/libs/font/cellFont.c" ]] || { echo "SKIP: no patched toolkit at $P (run tools/ps3recomp-patched.sh)"; exit 0; }
grep -q 'cellFontInitializeWithRevision(u64' "$P/libs/font/cellFont.c" || { echo "FAIL: $P/libs/font/cellFont.c defines no cellFontInitializeWithRevision"; exit 1; }
O="$ROOT/work/upstream-tests/font2060"; mkdir -p "$O"
D="-DKZ3_FONT_NO_COMMIT -DKZ3_FONT_POOL_BASE=0x80000u -DKZ3_FONT_POOL_SIZE=0x40000u"
INC=(-I "$P/libs/font" -I "$P" -I "$P/include" -I "$P/libs")
SRC=("$P/libs/font/cellFont.c"); [[ -f "$P/libs/font/cellFontOutline.c" ]] && SRC+=("$P/libs/font/cellFontOutline.c")
gcc -std=gnu11 -w $D "${INC[@]}" "$ROOT/tools/smoke/test_font_init2060.c" "${SRC[@]}" -o "$O/t_font2060" -lm || exit 1
out="$(KZ3_FONT_DIR="$O/none" KZ3_FONT_FALLBACK_DIR="$O/none" "$O/t_font2060" 2>&1)"; tail -4 <<<"$out"
grep -q "RESULT: PASS" <<<"$out" || { echo "FAIL"; exit 1; }
nids="$(cd "$P" && python3 tools/test_font_hle_nids.py 2>&1)" || { echo "$nids" | tail -4; echo "FAIL: NID 0xF03DCC29 registration"; exit 1; }
echo "$nids" | tail -2
