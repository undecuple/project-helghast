#!/usr/bin/env bash
# Host test for KZ3 patch 2213 (LD_SMP_RING_NODRAIN): per-table last-use fences (rsx_smp_use.h) against an actual-reader oracle, compared with the
# old drain and with patch 2155's live-prefix marks; negative controls (broken policy, submit that never fences, bind that never stamps) and a
# source check that the draw path and the take call the new functions. No game data, no GPU; SKIP without patch 2213.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_smp_nodrain.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="${KZ3_VTX_DIR:-$PT/libs/video}"
if [[ ! -f "$V/rsx_smp_use.h" ]]; then echo "[test_smp_nodrain] SKIP: toolkit without patch 2213 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
O="$ROOT/build/smoke-host/smp_nodrain"; mkdir -p "$O/mut"
CC="${CC:-cc}"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g"
$CC $W -I"$V" "$ROOT/tools/smoke/test_smp_nodrain.c" -o "$O/t"
"$O/t"
neg() {   # name, sed expression: the mutated header must FAIL the test
  sed "$2" "$V/rsx_smp_use.h" > "$O/mut/rsx_smp_use.h"
  if cmp -s "$V/rsx_smp_use.h" "$O/mut/rsx_smp_use.h"; then echo "[test_smp_nodrain] FAIL: mutation '$1' did not change the header"; exit 1; fi
  cp "$V/rsx_ring_marks.h" "$O/mut/rsx_ring_marks.h"
  $CC $W -I"$O/mut" "$ROOT/tools/smoke/test_smp_nodrain.c" -o "$O/t_$1"
  if "$O/t_$1" >/dev/null 2>&1; then echo "[test_smp_nodrain] FAIL: negative control '$1' not detected"; exit 1; fi
  echo "[test_smp_nodrain] negative control '$1': detected (PASS)"
}
neg nofence 's#/\*SUBMIT\*/ if (u->f\[s\]\[t\] == RSX_SMPU_UNSUB) u->f\[s\]\[t\] = fence;#(void)u, (void)fence;#'          # a submit that never turns UNSUB into a fence
neg nobind 's#if (t < RSX_SMPU_CAP) u->f\[slot % RSX_SMPU_SLOTS\]\[t\] = RSX_SMPU_UNSUB;#(void)u, (void)slot, (void)t;#'   # a bind that never stamps
neg oldestfence 's#/\*SUBMIT\*/ if (u->f\[s\]\[t\] == RSX_SMPU_UNSUB) u->f\[s\]\[t\] = fence;#if (u->f[s][t] == RSX_SMPU_UNSUB) u->f[s][t] = 1 + 0 * fence;#'   # a fence older than the submit
# the sources must call the new functions
if [[ -f "$V/rsx_live_draw.c" ]]; then
  srcchk() { local n; n=$(grep -c -- "$2" "$1" || true); [[ "$n" -ge "$3" ]] || { echo "[test_smp_nodrain] FAIL: $1 lacks '$2' (found $n, need $3)"; return 1; }; }
  F="$V/rsx_live_draw.c"
  srcchk "$F" 'if (smp_ring_full && ld_smp_wrap_nodrain()) smp_only = 1;' 1
  srcchk "$F" 'if (!ld_smp_ensure_table(g.smp_ring_used)) return LD_TAKE_FAIL;' 1
  srcchk "$F" 'ld_smp_bind(i);' 1
  srcchk "$F" 'ld_smp_bind(g.smp_ring_used);' 1
  srcchk "$F" 'rsx_smpu_submit(&g_ld_smpu, fence);' 1
  srcchk "$F" 'ld_smp_note_submit(v);' 2   # both submit paths of g.list
  srcchk "$F" 'if (!smp_only) { g.srv_ring_used = 0; g_srv_desc_gen++; }' 1
  grep -v 'ld_smp_bind(i);' "$F" > "$O/mut/rsx_live_draw.c"
  if srcchk "$O/mut/rsx_live_draw.c" 'ld_smp_bind(i);' 1 >/dev/null 2>&1; then echo "[test_smp_nodrain] FAIL: source-check control not detected"; exit 1; fi
  echo "[test_smp_nodrain] source check + negative control: PASS"
fi
echo "[test_smp_nodrain] PASS"
