#!/usr/bin/env bash
# Host test for KZ3 patches 2209 (rsx_walk_trivial.h, RSX_WALK_TRIVIAL) and 2210 (rsx_diag_delay.h, KZ3_DIAG_DELAY): the pure
# cores, built with $CC (default cc), run plain and under ASan+UBSan when the compiler can, then every negative control: a scratch
# copy of the shipped header with one exact-text substitution must FAIL the named checks. Source-checks the glue sites of both
# patches. No game data, no GPU; SKIP without the patched toolkit or the patches. The arming gates of the real cellGcmSys.c are
# test_probe_glue.sh.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_diag_probes.sh'     (gcc: CC unset)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"; V="$PT/libs/video"
skip() { echo "[test_diag_probes] SKIP: $1 (a SKIP tests nothing; KZ3_HOST_TEST_STRICT=1 makes it a failure)"; [[ "${KZ3_HOST_TEST_STRICT:-0}" == 1 ]] && exit 1; exit 77; }
[[ -d "$V" ]] || skip "no patched toolkit ($PT)"
[[ -f "$V/rsx_walk_trivial.h" && -f "$V/rsx_diag_delay.h" ]] || skip "toolkit without patches 2209/2210"
O="$ROOT/build/smoke-host/diag_probes"; mkdir -p "$O"
CCX="${CC:-cc}"; FL=(-std=gnu11 -Wall -Wextra -Werror -O2 -g)
fails=0
build_run() {   # NAME TESTSRC HEADERDIR [flags]
  local n="$1" src="$ROOT/tools/smoke/$2" hd="$3"; shift 3
  $CCX "${FL[@]}" "$@" "-I$hd" "$src" -o "$O/$n" 2>"$O/$n.build" || { echo "[test_diag_probes] FAIL: build $n"; head -8 "$O/$n.build"; return 98; }
  "$O/$n" >"$O/$n.out" 2>&1
}
SAN=0; echo 'int main(void){return 0;}' | $CCX -fsanitize=address,undefined -x c - -o "$O/san_probe" 2>/dev/null && SAN=1
for t in walk_trivial:rsx_walk_trivial.h diag_delay:rsx_diag_delay.h; do
  n="${t%%:*}"; h="${t##*:}"
  build_run "$n" "test_$n.c" "$V"; rc=$?; tail -1 "$O/$n.out"
  [[ $rc -eq 0 ]] || { echo "[test_diag_probes] FAIL: unmutated $h"; head -8 "$O/$n.out"; fails=$((fails+1)); continue; }
  if [[ $SAN == 1 ]]; then build_run "${n}_asan" "test_$n.c" "$V" -fsanitize=address,undefined -fno-sanitize-recover=all || { echo "[test_diag_probes] FAIL: ASan/UBSan $n"; head -6 "$O/${n}_asan.out"; fails=$((fails+1)); }; fi
done
control() {   # NAME TEST HEADER 'exact text' 'replacement' 'checks that must fail'
  local n="$1" t="$2" h="$3" d="$O/mut_$1"; rm -rf "$d"; mkdir -p "$d"
  python3 -I - "$V/$h" "$d/$h" "$4" "$5" <<'PY' || { echo "[test_diag_probes] FAIL: mutation $n does not apply"; fails=$((fails+1)); return; }
import sys
src, dst, a, b = sys.argv[1:]
s = open(src).read()
assert s.count(a) == 1, (a, s.count(a))
open(dst, 'w').write(s.replace(a, b))
PY
  build_run "m_$n" "test_$t.c" "$d"; local rc=$?
  if [[ $rc -eq 0 || $rc -ge 98 ]]; then echo "[test_diag_probes] FAIL: control $n not caught (rc=$rc)"; fails=$((fails+1)); return; fi
  local c; for c in $6; do grep -q "FAIL $c" "$O/m_$n.out" || { echo "[test_diag_probes] FAIL: control $n did not fail check $c"; head -6 "$O/m_$n.out"; fails=$((fails+1)); return; }; done
  echo "[test_diag_probes] control $n: caught ($(grep -o '^FAIL [a-zA-Z0-9-]*' "$O/m_$n.out" | cut -d' ' -f2 | sort -u | tr '\n' ' '))"
}
W=rsx_walk_trivial.h; D=rsx_diag_delay.h
control WT_ZERO_IS_ON walk_trivial $W 'return e && *e && *e != '"'0'"';' 'return e && *e;' "zero-off"
control WT_NO_PERIOD walk_trivial $W 'if (now - w->t_last < freq) return 0;' '' "early-tick-silent"
control WT_TOTALS_NOT_DELTAS walk_trivial $W 'f - w->p_flips, m - w->p_methods' 'f, m' "second-delta-flips"
control WT_LABEL_DELTA walk_trivial $W 'labels - w->p_labels' 'labels' "second-delta-labels"
control WT_WB_HIDDEN walk_trivial $W 'gpuwb - w->p_gpuwb' '0' "second-gpuwb-reported"
control WT_ARM_SILENT_ON_WB walk_trivial $W 'NOT zero-filled' 'zeroed' "arm-says-not-zero-filled"
control WT_OFF_STILL_TICKS walk_trivial $W 'if (!w->on || freq <= 0) return 0;' 'if (freq <= 0) return 0;' "tick-silent-when-off"
control DD_SPIN_SHORT diag_delay $D 'while (t - t0 < want) t = d->now();' 'while (t - t0 < want - 1) t = d->now();' "spins-asked-ticks"
control DD_NO_SPIN diag_delay $D 'while (t - t0 < want) t = d->now();' '(void)want;' "spins-asked-ticks busy-waits-reads-clock"
control DD_TID_EVERY diag_delay $D 'if (__atomic_fetch_add(&d->count[side], 1ull, __ATOMIC_RELAXED) == 0) d->first_tid[side] = tid;' '__atomic_fetch_add(&d->count[side], 1ull, __ATOMIC_RELAXED); d->first_tid[side] = tid;' "tid-kept"
control DD_ZERO_ACCEPTED diag_delay $D 'if (out[0] <= 0.0 && out[1] <= 0.0) {' 'if (0) {' "parse-zero"
control DD_NO_RANGE diag_delay $D 'v > 1000.0' 'v > 1e9' "parse-huge"
control DD_TOTAL_NOT_DELTA diag_delay $D 'const unsigned long long dc = c - d->p_count[s], dt = t - d->p_ticks[s];' 'const unsigned long long dc = c, dt = t;' "second-delta-count"
control DD_TICK_EVERY_CALL diag_delay $D 'if (now - last < d->freq) return 0;' '' "tick-once-per-period"
control DD_WALK_IGNORED diag_delay $D 'else if (!strncmp(p, "walk:", 5)) { side = RSX_DD_WALK; p += 5; }' 'else if (!strncmp(p, "walk:", 5)) { side = RSX_DD_MAIN; p += 5; }' "parse-walk-frac parse-both"
# glue sites (source text; the game path needs the Windows build)
chk() { grep -q -- "$2" "$V/$1" || { echo "[test_diag_probes] FAIL: glue missing in $1: $2"; fails=$((fails+1)); }; }
cnt() { [[ "$(grep -c -- "$2" "$V/$1")" == "$3" ]] || { echo "[test_diag_probes] FAIL: expected $3 of '$2' in $1"; fails=$((fails+1)); }; }
chk rsx_live_draw.c 'if (rsx_walk_trivial_on()) {'
chk rsx_live_draw.c 'sink.flip = wt_flip;'
chk rsx_live_draw.c 'sink.end = wt_end'
chk rsx_live_draw.c 'sink.clear = wt_clear'
cnt rsx_live_draw.c 'wt_flip(void\* u' 1
cnt rsx_live_draw.c 'rsx_dd_hook(RSX_DD_WALK)' 2        # sink_flip and wt_flip: once per walker flip each (never both: the trivial sink replaces sink_flip)
chk rsx_live_draw.c 'RSX_WT_ADD(&g_rsx_wt, wb_req, 1)'
chk rsx_live_draw.c 'RSX_WT_ADD(&g_rsx_wt, methods, 1)'
cnt cellGcmSys.c 'rsx_dd_hook(RSX_DD_MAIN)' 1
chk cellGcmSys.c 'getenv("RSX_WALK_TRIVIAL")'
chk cellGcmSys.c 'getenv("KZ3_DIAG_DELAY")'
# the trivial sink must replace the present: no rsx_live_draw_present call inside the wt_ functions
awk '/^static void wt_begin/,/^static void sink_flip/' "$V/rsx_live_draw.c" | grep -q 'rsx_live_draw_present\|sink_end_impl\|sink_clear_impl' && { echo "[test_diag_probes] FAIL: a wt_ sink function submits or presents"; fails=$((fails+1)); }
[[ $fails -eq 0 ]] && echo "[test_diag_probes] PASS (toolkit $PT, $CCX)" || { echo "[test_diag_probes] $fails FAILED"; exit 1; }
