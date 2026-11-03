#!/usr/bin/env bash
# Host test for KZ3 patch 2146 (PS3RECOMP_PRODUCT_NAME): default name unchanged, an override changes the comment text
# of keybinds.ini and display.ini. No game data, no GPU. (The folder name is the same macro in cellPad.c; its Windows
# compile is checked by test_pad_keybinds.sh when MinGW is on PATH.)
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_product_name.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/libs/input/pad_keybinds.h" ]] || ! grep -q PS3RECOMP_PRODUCT_NAME "$PT/libs/input/pad_keybinds.h"; then
  echo "[test_product_name] SKIP: no patched toolkit with patch 2146 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/product_name"; mkdir -p "$O"
CC="${CC:-cc}"
W=(-std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -I"$PT/libs/input" -I"$PT/libs/video")
"$CC" "${W[@]}" "$ROOT/tools/smoke/test_product_name.c" -o "$O/default"
"$O/default" | grep -v "^\[PASS\]" || true; "$O/default" >/dev/null
"$CC" "${W[@]}" -DPS3RECOMP_PRODUCT_NAME='"Qwark'"'"'s Awakening"' -DEXPECT='"Qwark'"'"'s Awakening"' \
  "$ROOT/tools/smoke/test_product_name.c" -o "$O/override"
"$O/override" | grep -v "^\[PASS\]" || true; "$O/override" >/dev/null
echo "[PASS] test_product_name: default and override"
