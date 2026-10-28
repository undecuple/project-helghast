#!/usr/bin/env bash
# Patch 1062 host test (tools/spu/test_spurs_attr.c): the internal _cellSpursAttributeInitialize argument mapping
# with KZ3's two calls. Runs (default = SDK order since SPU pass 60): clean --sdk = the default (HiPrio 5, LoPrio 1,
# must pass), KZ3_SPURS_ATTR_SDK=0 = the old mapping (nSpus 2/2, must pass), KZ3_SPURS_ATTR_SDK=0 --sdk = negative
# control (must fail with nSpus = 2). Needs work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spurs_attr.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -d "$T/libs/spurs" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
[[ -f "$T/libs/spurs/kz3_spurs_attr.h" ]] || { echo "FAIL: $T has no patch 1062 (libs/spurs/kz3_spurs_attr.h)"; exit 1; }
gcc -std=gnu17 -O2 -Wall -Wextra -I "$T/libs/spurs" -o "$O/t_spurs_attr" "$ROOT/tools/spu/test_spurs_attr.c" 2>&1 | grep -m5 -E "error|warning"
[[ -x "$O/t_spurs_attr" ]] || { echo "FAIL: build"; exit 1; }
rc=0
out="$(KZ3_SPURS_ATTR_SDK=0 "$O/t_spurs_attr" 2>&1)"; r=$?; echo "KZ3_SPURS_ATTR_SDK=0 (old mapping): rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -5; rc=1; }
out="$(env -u KZ3_SPURS_ATTR_SDK "$O/t_spurs_attr" --sdk 2>&1)"; r=$?; echo "clean --sdk (default): rc=$r $(tail -1 <<<"$out")"; [[ $r == 0 ]] || { echo "$out" | head -5; rc=1; }
out="$(KZ3_SPURS_ATTR_SDK=0 "$O/t_spurs_attr" --sdk 2>&1)"; r=$?
if [[ $r != 0 ]] && grep -q 'HiPrio nSpus = 2' <<<"$out"; then echo "--sdk with KZ3_SPURS_ATTR_SDK=0: fails (nSpus read as 2): negative control OK"
else echo "--sdk with KZ3_SPURS_ATTR_SDK=0: expected a failure with nSpus = 2 (rc=$r)"; echo "$out" | head -4; rc=1; fi
exit $rc
