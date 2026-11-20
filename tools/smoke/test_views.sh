#!/usr/bin/env bash
# Host test for KZ3 patch 2202: libs/video/rsx_views.h, the RSX_VIEWS=1 split-screen per-view counter (pure core).
# Builds tools/smoke/test_views.c with $CC (default cc), runs it plain and under ASan+UBSan when the compiler can, then runs
# every negative control: a scratch copy of the shipped header with one exact-text substitution (the shipped header carries
# no test-only code) must FAIL the named checks. Source-checks the five glue sites of the patch. No game data, no GPU;
# SKIP without the patched toolkit or patch 2202.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_views.sh'     (gcc: CC unset)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT/libs/video" ]] || { echo "[test_views] SKIP: no patched toolkit ($PT)"; exit 0; }
[[ -f "$PT/libs/video/rsx_views.h" ]] || { echo "[test_views] SKIP: toolkit without patch 2202"; exit 0; }
O="$ROOT/build/smoke-host/views"; mkdir -p "$O"
CCX="${CC:-cc}"
SRC="$ROOT/tools/smoke/test_views.c"
FL=(-std=gnu11 -Wall -Wextra -Werror -O2 -g)
fails=0
build_run() {   # NAME HEADERDIR [extra flags]: builds and runs, output in $O/NAME.out, returns the program's status
  local n="$1" hd="$2"; shift 2
  $CCX "${FL[@]}" "$@" "-I$hd" "$SRC" -o "$O/$n" 2>"$O/$n.build" || { echo "[test_views] FAIL: build $n"; head -8 "$O/$n.build"; return 98; }
  "$O/$n" >"$O/$n.out" 2>&1
}
build_run plain "$PT/libs/video"; rc=$?
tail -1 "$O/plain.out"
[[ $rc -eq 0 ]] || { echo "[test_views] FAIL: unmutated header"; head -8 "$O/plain.out"; exit 1; }
if echo 'int main(void){return 0;}' | $CCX -fsanitize=address,undefined -x c - -o "$O/san_probe" 2>/dev/null; then
  build_run asan "$PT/libs/video" -fsanitize=address,undefined -fno-sanitize-recover=all; rc=$?
  [[ $rc -eq 0 ]] && echo "[test_views] ASan+UBSan run ok" || { echo "[test_views] FAIL: ASan/UBSan run"; head -8 "$O/asan.out"; fails=$((fails+1)); }
else echo "[test_views] sanitizers not available for $CCX: skipped"; fi
# glue sites of the patch (source text; the sinks themselves need the Windows build)
chk() { grep -q -- "$2" "$PT/libs/video/$1" || { echo "[test_views] FAIL: glue missing in $1: $2"; fails=$((fails+1)); }; }
chk rsx_live_draw.c 'ld_views_note_draw(r);   /\* patch 2202 \*/'
chk rsx_live_draw.c 'rsx_views_flip(&g_rsx_views'
chk rsx_live_draw.c 'rsx_views_word(&g_rsx_views)'
chk cellGcmSys.c '^rsx_views g_rsx_views;'
chk cellGcmSys.c 'rsx_perf_views_on();'
[[ "$(grep -c 'ld_views_note_draw(r);   /\* patch 2202 \*/' "$PT/libs/video/rsx_live_draw.c")" == 3 ]] || { echo "[test_views] FAIL: expected 3 draw-packet sinks hooked"; fails=$((fails+1)); }
# control NAME 'exact text' 'replacement' 'checks that must fail'
control() {
  local n="$1" d="$O/mut_$1"; rm -rf "$d"; mkdir -p "$d"
  python3 -I - "$PT/libs/video/rsx_views.h" "$d/rsx_views.h" "$2" "$3" <<'PY' || { echo "[test_views] FAIL: mutation $n does not apply"; fails=$((fails+1)); return; }
import sys
src, dst, a, b = sys.argv[1:]
s = open(src).read()
assert s.count(a) == 1, (a, s.count(a))
open(dst, 'w').write(s.replace(a, b))
PY
  build_run "m_$n" "$d"; local rc=$?
  if [[ $rc -eq 0 || $rc -ge 98 ]]; then echo "[test_views] FAIL: control $n not caught (rc=$rc)"; fails=$((fails+1)); return; fi
  local c
  for c in $4; do grep -q "FAIL $c " "$O/m_$n.out" || { echo "[test_views] FAIL: control $n did not fail check $c"; head -6 "$O/m_$n.out"; fails=$((fails+1)); return; }; done
  echo "[test_views] control $n: caught ($(grep -o '^FAIL [a-zA-Z0-9-]*' "$O/m_$n.out" | cut -d' ' -f2 | sort -u | tr '\n' ' '))"
}
control NO_AREA_RULE '            if (area * 100u < (uint64_t)g->tw * g->th * RSX_VIEWS_AREA_PCT) continue;
' '            (void)area;
' "two-view-count"
control NO_OVERLAP_RULE '            if (clash) continue;' '            (void)clash;' "one-view-count overlap-is-one-view"
control GLOBAL_TARGET_SIZE '            if (g->tw != v->g[c].tw || g->th != v->g[c].th) continue;
' '' "per-size-selection"
control NO_MIN_DRAWS '            if (g->draws < v->min_draws) continue;
' '' "min-draws-below"
control WORDS_NOT_GROUPED 'v->g[i].draws++; v->g[i].words += v->pend_words;' 'v->g[i].draws++;' "one-view-line two-view-left-numbers"
control IGNORE_WRITTEN_FLAG '    if (written) {' '    if (written || 1) {' "one-view-line min-draws-at"
control NO_CLAMP '        if (sx + sw < x1) x1 = sx + sw;
        if (sy + shh < y1) y1 = sy + shh;
' '        x1 = sx + sw;
        y1 = sy + shh;
' "clamp"
control NO_TAIL '    v->tail_words = v->pend_words;' '    v->tail_words = 0;' "tail-words"
control NO_FLIP_RESET '    v->ng = 0; v->draws' '    v->draws' "per-flip-reset"
control NO_SURF_COUNT '        if (col != v->last_col || zeta != v->last_zeta) v->surf_changes++;
' '' "surf-changes"
[[ $fails -eq 0 ]] && echo "[test_views] PASS" || { echo "[test_views] $fails FAILURE(S)"; exit 1; }
