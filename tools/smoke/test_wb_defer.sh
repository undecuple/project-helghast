#!/usr/bin/env bash
# Host test for KZ3 patch 2185 (LD_WB_DEFER): the pending-store queue in rsx_wb_defer.h (order, one wait, slot reuse forcing a drain,
# drain idempotence, overlap queries, queue full, wait failure, verify compare, published-range snapshot under TSan when available)
# and the 2196 env rule (unset = on, =0 = off), and one negative control per unit (newest-first order, no slot-taken bookkeeping / no forced drain, verify that ignores a byte).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_wb_defer.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_wb_defer.h" ]]; then echo "[test_wb_defer] SKIP: toolkit without patch 2185 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
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
# patch 2212: the sources must call the predicate / the NV4097 site setter (a missing call would leave the K4 0x4A release synchronous)
if grep -q rsx_wbd_site_defer "$V/rsx_wb_defer.h" && [[ -f "$V/rsx_commands.c" ]]; then
  srcchk() {   # file, pattern, count: the pattern must occur at least `count` times
    local n; n=$(grep -c -- "$2" "$1" || true); [[ "$n" -ge "$3" ]] || { echo "[test_wb_defer] FAIL: $1 lacks '$2' (found $n, need $3)"; return 1; }; }
  srcchk "$V/rsx_commands.c" 'rsx_live_draw_wbd_set_site_4097((int)((s_sem_off' 1
  srcchk "$V/rsx_commands.c" 'rsx_live_draw_wbd_set_site_4097(-1)' 1
  srcchk "$V/rsx_live_draw.c" 'rsx_wbd_site_defer(ld_wbd_mode(), g_wbd_label, label, 1,' 1
  srcchk "$V/rsx_live_draw.c" 'rsx_wbd_batch_drain_first(g_wbd.n, defer_ok, ld_wbd_mode())' 1
  # negative control: the commands file without the site call must fail the check
  grep -v 'rsx_live_draw_wbd_set_site_4097((int)((s_sem_off' "$V/rsx_commands.c" > "$O/mut/rsx_commands.c"
  if srcchk "$O/mut/rsx_commands.c" 'rsx_live_draw_wbd_set_site_4097((int)((s_sem_off' 1 >/dev/null 2>&1; then echo "[test_wb_defer] FAIL: source-check control not detected"; exit 1; fi
  echo "[test_wb_defer] source check + negative control: PASS"
  neg site4097 's#if (is_4097 \&\& !sw4097) return 0;#if (is_4097) return (void)sw4097, 0;#'     # a 4097 release never defers (the bug before 2212)
  neg swignored 's#if (is_4097 \&\& !sw4097) return 0;#(void)is_4097, (void)sw4097;#'   # the switch is ignored
  neg drainfirst 's#return pending \&\& !(defer_ok \&\& mode);#return (void)defer_ok, (void)mode, pending != 0;#'   # the batch drains first even at a deferring site
  neg notouch 's#if (!d->n || !rsx_wbd_overlap(d, loc, off, size)) return 0;#(void)loc, (void)off, (void)size; return 0;#'   # a reader of a pending range is not made to wait
  neg holdoff 's#if (!armed || !pending || h->n >= RSX_WBD_HOLD_MAX) return 0;#(void)armed; (void)pending; return 0;#'   # the label is never held (the reviewed window)
  neg holdearly 's#if (pending || !h->n) return 0;#(void)pending; if (!h->n) return 0;#'   # published while stores are still pending
  neg holdorder 's#for (uint32_t i = 0; i < n; i++) publish(h->ea\[i\], h->val\[i\]);#for (uint32_t i = n; i-- > 0; ) publish(h->ea[i], h->val[i]);#'   # labels out of order
  srcchk "$V/rsx_commands.c" 'rsx_live_draw_wbd_hold_label(' 3
  srcchk "$V/rsx_live_draw.c" 'rsx_wbd_hold_flush(' 3
  srcchk "$V/cellGcmSys.c" 'rsx_live_draw_wbd_idle()' 1
  grep -v 'rsx_wbd_hold_flush(&g_wbd_hold, g_wbd.n, reason' "$V/rsx_live_draw.c" > "$O/mut/rsx_live_draw.c"
  if srcchk "$O/mut/rsx_live_draw.c" 'rsx_wbd_hold_flush(' 3 >/dev/null 2>&1; then echo "[test_wb_defer] FAIL: hold source-check control not detected"; exit 1; fi
  echo "[test_wb_defer] label-hold source check + negative control: PASS"
fi
echo "[test_wb_defer] PASS"
