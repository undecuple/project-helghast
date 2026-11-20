#!/usr/bin/env bash
# Host test for KZ3 patch 2185 (LD_WB_DEFER): the pending-store queue in rsx_wb_defer.h (order, one wait, slot reuse forcing a drain,
# drain idempotence, overlap queries, queue full, wait failure, verify compare, published-range snapshot under TSan when available)
# and the 2196 env rule (unset = on, =0 = off), and one negative control per unit (newest-first order, no slot-taken bookkeeping / no forced drain, verify that ignores a byte).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_wb_defer.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_wb_defer.h" ]]; then echo "[test_wb_defer] SKIP: toolkit without patch 2185 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/wb_defer"; mkdir -p "$O/mut"
CC="${CC:-cc}"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -pthread"
$CC $W -O1 -g -I"$V" "$ROOT/tools/smoke/test_wb_defer.c" -o "$O/t"
"$O/t"
if echo 'int main(void){return 0;}' | $CC -fsanitize=thread -x c - -o "$O/tsan_probe" 2>/dev/null; then
  $CC $W -O1 -g -fsanitize=thread -I"$V" "$ROOT/tools/smoke/test_wb_defer.c" -o "$O/t_tsan"
  "$O/t_tsan" race
else echo "[test_wb_defer] TSan not available here: race stress built without it"; $CC $W -O1 -I"$V" "$ROOT/tools/smoke/test_wb_defer.c" -o "$O/t_race"; "$O/t_race" race; fi
neg() {   # name, sed expression: the mutated header must FAIL the test
  sed "$2" "$V/rsx_wb_defer.h" > "$O/mut/rsx_wb_defer.h"
  if cmp -s "$V/rsx_wb_defer.h" "$O/mut/rsx_wb_defer.h"; then echo "[test_wb_defer] FAIL: mutation '$1' did not change the header"; exit 1; fi
  $CC $W -O1 -I"$O/mut" "$ROOT/tools/smoke/test_wb_defer.c" -o "$O/t_$1"
  if "$O/t_$1" >/dev/null 2>&1; then echo "[test_wb_defer] FAIL: negative control '$1' not detected"; exit 1; fi
  echo "[test_wb_defer] negative control '$1': detected (PASS)"
}
neg order 's#/\*ORDER\*/ for (uint32_t i = 0; i < n; i++) {#for (uint32_t i = n; i-- > 0; ) {#'
neg noforce 's#return rsx_wbd_free_slots(d) < need;#return (void)d, (void)need, 0;#'
neg verifybyte 's#/\*CMP\*/ for (uint32_t i = 0; i < n; i++)#for (uint32_t i = 0; i + 1 < n; i++)#'
neg envoldoff 's#return !(v \&\& v\[0\] == .0.);#return v \&\& v[0] \&\& v[0] != 48;#'   # the pre-2196 rule (unset = off)
neg envalwayson 's#return !(v \&\& v\[0\] == .0.);#return (void)v, 1;#'   # ignores LD_WB_DEFER=0
echo "[test_wb_defer] PASS"
