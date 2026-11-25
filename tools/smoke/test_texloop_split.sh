#!/usr/bin/env bash
# Host test for KZ3 patch 2205: libs/video/rsx_texloop_split.h, the sampled texture-loop split of RSX_PERF_DRAW=1 (pure core).
# Builds tools/smoke/test_texloop_split.c with $CC (default cc), runs it plain and under ASan+UBSan when the compiler can, then
# runs every negative control: a scratch copy of the shipped header with one exact-text substitution must FAIL the named checks
# (the shipped header carries no test-only code). Source-checks the glue sites of the patch. No game data, no GPU; SKIP without
# the patched toolkit or patch 2205.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_texloop_split.sh'     (gcc: CC unset)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT/libs/video" ]] || { echo "[test_texloop_split] SKIP: no patched toolkit ($PT; a SKIP tests nothing; KZ3_HOST_TEST_STRICT=1 makes it a failure)"; [[ "${KZ3_HOST_TEST_STRICT:-0}" == 1 ]] && exit 1; exit 77; }
[[ -f "$PT/libs/video/rsx_texloop_split.h" ]] || { echo "[test_texloop_split] SKIP: toolkit without patch 2205 ($PT; a SKIP tests nothing; KZ3_HOST_TEST_STRICT=1 makes it a failure)"; [[ "${KZ3_HOST_TEST_STRICT:-0}" == 1 ]] && exit 1; exit 77; }
O="$ROOT/build/smoke-host/texloop_split"; mkdir -p "$O"
CCX="${CC:-cc}"
SRC="$ROOT/tools/smoke/test_texloop_split.c"
FL=(-std=gnu11 -Wall -Wextra -Werror -O2 -g)
fails=0
build_run() {   # NAME HEADERDIR [extra flags]: builds and runs, output in $O/NAME.out, returns the program's status
  local n="$1" hd="$2"; shift 2
  $CCX "${FL[@]}" "$@" "-I$hd" "$SRC" -o "$O/$n" -lm 2>"$O/$n.build" || { echo "[test_texloop_split] FAIL: build $n"; head -8 "$O/$n.build"; return 98; }
  "$O/$n" >"$O/$n.out" 2>&1
}
build_run plain "$PT/libs/video"; rc=$?
tail -1 "$O/plain.out"
[[ $rc -eq 0 ]] || { echo "[test_texloop_split] FAIL: unmutated header"; head -8 "$O/plain.out"; exit 1; }
if echo 'int main(void){return 0;}' | $CCX -fsanitize=address,undefined -x c - -o "$O/san_probe" 2>/dev/null; then
  build_run asan "$PT/libs/video" -fsanitize=address,undefined -fno-sanitize-recover=all; rc=$?
  [[ $rc -eq 0 ]] && echo "[test_texloop_split] ASan+UBSan run ok" || { echo "[test_texloop_split] FAIL: ASan/UBSan run"; head -8 "$O/asan.out"; fails=$((fails+1)); }
else echo "[test_texloop_split] sanitizers not available for $CCX: skipped"; fi
# glue sites of the patch (source text; the draw loop itself needs the Windows build)
chk() { grep -q -- "$2" "$PT/libs/video/$1" || { echo "[test_texloop_split] FAIL: glue missing in $1: $2"; fails=$((fails+1)); }; }
cnt() { [[ "$(grep -c -- "$2" "$PT/libs/video/$1")" == "$3" ]] || { echo "[test_texloop_split] FAIL: expected $3 of '$2' in $1"; fails=$((fails+1)); }; }
chk rsx_live_draw.c 'rsx_tls_begin(&g_rsx_tls);   /\* patch 2205 \*/'
chk rsx_live_draw.c 'rsx_tls_end(&g_rsx_tls);'
chk rsx_live_draw.c 'ld_tls_report(us_tick, frames);'
chk rsx_live_draw.c 'rsx_tls_count(&g_rsx_tls, RSX_TLS_EN'
cnt rsx_live_draw.c 'LDT_ADD(RSX_TLS_PLAIN' 3
cnt rsx_live_draw.c 'LDT_ADD(RSX_TLS_DESC' 2
for b in L1 GET SMP ZSNAP SWIN RG16 LOOP; do cnt rsx_live_draw.c "LDT_ADD(RSX_TLS_$b," 1; done
# off: the sampling flag is set only inside ld_pd_draw (RSX_PERF_DRAW), so no other writer may exist
cnt rsx_live_draw.c 'rsx_tls_begin(' 1
# control NAME 'exact text' 'replacement' 'checks that must fail'
control() {
  local n="$1" d="$O/mut_$1"; rm -rf "$d"; mkdir -p "$d"
  python3 -I - "$PT/libs/video/rsx_texloop_split.h" "$d/rsx_texloop_split.h" "$2" "$3" <<'PY' || { echo "[test_texloop_split] FAIL: mutation $n does not apply"; fails=$((fails+1)); return; }
import sys
src, dst, a, b = sys.argv[1:]
s = open(src).read()
assert s.count(a) == 1, (a, s.count(a))
open(dst, 'w').write(s.replace(a, b))
PY
  build_run "m_$n" "$d"; local rc=$?
  if [[ $rc -eq 0 || $rc -ge 98 ]]; then echo "[test_texloop_split] FAIL: control $n not caught (rc=$rc)"; fails=$((fails+1)); return; fi
  local c
  for c in $4; do grep -q "FAIL $c" "$O/m_$n.out" || { echo "[test_texloop_split] FAIL: control $n did not fail check $c"; head -6 "$O/m_$n.out"; fails=$((fails+1)); return; }; done
  echo "[test_texloop_split] control $n: caught ($(grep -o '^FAIL [a-zA-Z0-9-]*' "$O/m_$n.out" | cut -d' ' -f2 | sort -u | tr '\n' ' '))"
}
# negative control: the sampled sums are not scaled back to all draws
control NO_SCALING 'return t->sampled ? (double)t->draws / (double)t->sampled : 0.0;' 'return t->sampled ? 1.0 : 0.0;' "row-4-us-multiple row-4-us-ragged loop-us-multiple varied-plain"
control SCALE_FIXED_16 'return t->sampled ? (double)t->draws / (double)t->sampled : 0.0;' 'return t->sampled ? 16.0 : 0.0;' "row-4-us-ragged row-4-us-tiny"
control CALLS_NOT_SCALED '*calls_draw = (double)t->cnt[b] * s / n;' '*calls_draw = (double)t->cnt[b] / n;' "row-4-calls-multiple units-per-draw-multiple varied-units"
control EVERY_DRAW 't->act = (t->draws % RSX_TLS_PERIOD) == t->pick;' 't->act = 1;' "sampled-count-multiple clock-reads-multiple"
control ACT_NOT_CLEARED 'static inline void rsx_tls_end(rsx_tls* t) { t->act = 0; }' 'static inline void rsx_tls_end(rsx_tls* t) { (void)t; }' "act-cleared-after-sampled"
control OTHER_MISSES_RG16 'for (int b = RSX_TLS_L1; b <= RSX_TLS_RG16; b++)' 'for (int b = RSX_TLS_L1; b < RSX_TLS_RG16; b++)' "other-us-multiple"
control NEGATIVE_NOT_CLAMPED '    if (ticks < 0) ticks = 0;
' '' "negative-span"
control PERIOD_OFF_BY_ONE 't->act = (t->draws % RSX_TLS_PERIOD) == t->pick;' 't->act = (t->draws % RSX_TLS_PERIOD) == t->pick + 1;' "sampling-phase"
# review of 2205: the old fixed phase (every block times offset 0) aliases with period-16 and period-8 draw patterns
control FIXED_PHASE 't->pick = (uint32_t)(((uint64_t)t->rng * RSX_TLS_PERIOD) >> 32);' 't->pick = 0;' "sampling-offsets-vary alias-period16-phase0 alias-period16-phase15 alias-period8-phase0"
[[ $fails -eq 0 ]] && echo "[test_texloop_split] PASS (toolkit $PT, rsx_texloop_split.h sha256 $(sha256sum "$PT/libs/video/rsx_texloop_split.h" | cut -c1-12))" || { echo "[test_texloop_split] $fails FAILURE(S)"; exit 1; }
