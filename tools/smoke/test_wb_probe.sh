#!/usr/bin/env bash
# Host test for KZ3 patch 2184 (LD_WB_READ / LD_WB_SKIP / LD_WB_HASH): rsx_wb_probe.h skip-list parser, slack bookkeeping
# (store/GET/PUT, dirty GETs, median/min), guest hash, a two-thread stress under TSan when available, and one negative
# control per unit (parser without the whole-token check, GET bookkeeping without the PUT coverage test, hash without the tail).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_wb_probe.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_wb_probe.h" ]]; then echo "[test_wb_probe] SKIP: toolkit without patch 2184 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
O="$ROOT/build/smoke-host/wb_probe"; mkdir -p "$O/mut"
CC="${CC:-cc}"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -pthread"
$CC $W -O1 -g -I"$V" "$ROOT/tools/smoke/test_wb_probe.c" -o "$O/t"
"$O/t"
if echo 'int main(void){return 0;}' | $CC -fsanitize=thread -x c - -o "$O/tsan_probe" 2>/dev/null; then
  $CC $W -O1 -g -fsanitize=thread -I"$V" "$ROOT/tools/smoke/test_wb_probe.c" -o "$O/t_tsan"
  "$O/t_tsan" race
else echo "[test_wb_probe] TSan not available here: race stress built without it"; $CC $W -O1 -I"$V" "$ROOT/tools/smoke/test_wb_probe.c" -o "$O/t_race"; "$O/t_race" race; fi
neg() {   # name, sed expression: the mutated header must FAIL the test
  sed "$2" "$V/rsx_wb_probe.h" > "$O/mut/rsx_wb_probe.h"
  if cmp -s "$V/rsx_wb_probe.h" "$O/mut/rsx_wb_probe.h"; then echo "[test_wb_probe] FAIL: mutation '$1' did not change the header"; exit 1; fi
  $CC $W -O1 -I"$O/mut" "$ROOT/tools/smoke/test_wb_probe.c" -o "$O/t_$1"
  if "$O/t_$1" >/dev/null 2>&1; then echo "[test_wb_probe] FAIL: negative control '$1' not detected"; exit 1; fi
  echo "[test_wb_probe] negative control '$1': detected (PASS)"
}
neg parser 's/ || \*end || tmp\[0\] == .-.//'
neg dirty 's/if (!(s->written\[l >> 3\] \& (1u << (l \& 7u)))) { dirty = 1; break; }/{ dirty = 0; }/'
neg hashtail 's/for (; i < n; i++) { h = (h ^ p\[i\]) \* 0x100000001b3ull; }//'
echo "[test_wb_probe] PASS"
