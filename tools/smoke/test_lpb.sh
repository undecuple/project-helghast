#!/usr/bin/env bash
# Host test for KZ3 patch 2208: libs/video/rsx_lpb.h, the RSX_O0=1 [o0-lpb] LightProbe 8x8 block probe (pure core).
# Builds tools/smoke/test_lpb.c with $CC (default cc), runs it plain and under ASan+UBSan when the compiler can, then runs
# every negative control: a scratch copy of the shipped header with one exact-text substitution (the shipped header carries
# no test-only code) must FAIL the named checks. Source-checks the glue sites of the patch. No game data, no GPU;
# SKIP without the patched toolkit or patch 2208.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_lpb.sh'     (gcc: CC unset)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT/libs/video" ]] || { echo "[test_lpb] SKIP: no patched toolkit ($PT)"; exit 0; }
[[ -f "$PT/libs/video/rsx_lpb.h" ]] || { echo "[test_lpb] SKIP: toolkit without patch 2208"; exit 0; }
O="$ROOT/build/smoke-host/lpb"; mkdir -p "$O"
CCX="${CC:-cc}"
SRC="$ROOT/tools/smoke/test_lpb.c"
FL=(-std=gnu11 -Wall -Wextra -Werror -O2 -g)
fails=0
build_run() {   # NAME HEADERDIR [extra flags]: builds and runs, output in $O/NAME.out, returns the program's status
  local n="$1" hd="$2"; shift 2
  $CCX "${FL[@]}" "$@" "-I$hd" "$SRC" -o "$O/$n" 2>"$O/$n.build" || { echo "[test_lpb] FAIL: build $n"; head -8 "$O/$n.build"; return 98; }
  "$O/$n" >"$O/$n.out" 2>&1
}
build_run plain "$PT/libs/video"; rc=$?
tail -1 "$O/plain.out"
[[ $rc -eq 0 ]] || { echo "[test_lpb] FAIL: unmutated header"; head -8 "$O/plain.out"; exit 1; }
if echo 'int main(void){return 0;}' | $CCX -fsanitize=address,undefined -x c - -o "$O/san_probe" 2>/dev/null; then
  build_run asan "$PT/libs/video" -fsanitize=address,undefined -fno-sanitize-recover=all; rc=$?
  [[ $rc -eq 0 ]] && echo "[test_lpb] ASan+UBSan run ok" || { echo "[test_lpb] FAIL: ASan/UBSan run"; head -8 "$O/asan.out"; fails=$((fails+1)); }
else echo "[test_lpb] sanitizers not available for $CCX: skipped"; fi
# glue sites of the patch (source text; the sinks themselves need the Windows build)
chk() { grep -q -- "$2" "$PT/libs/video/$1" || { echo "[test_lpb] FAIL: glue missing in $1: $2"; fails=$((fails+1)); }; }
chk rsx_live_draw.c 'ld_lpb_bind(t);   /\* patch 2208 \*/'
chk rsx_live_draw.c 'rsx_lpb_hash(&g_rsx_lpb'
chk rsx_live_draw.c 'rsx_lpb_redecode(&g_rsx_lpb'
chk rsx_live_draw.c 'rsx_lpb_new(&g_rsx_lpb'
chk rsx_live_draw.c 'rsx_lpb_flip(&g_rsx_lpb'
chk cellGcmSys.c '^rsx_lpb g_rsx_lpb;'
chk cellGcmSys.c 'rsx_lpb_start();'
[[ "$(grep -c 'ld_lpb_bind(t);   /\* patch 2208 \*/' "$PT/libs/video/rsx_live_draw.c")" == 2 ]] || { echo "[test_lpb] FAIL: expected the bind hook in the existing-entry and the new-entry path"; fails=$((fails+1)); }
# control NAME 'exact text' 'replacement' 'checks that must fail'
control() {
  local n="$1" d="$O/mut_$1"; rm -rf "$d"; mkdir -p "$d"
  python3 -I - "$PT/libs/video/rsx_lpb.h" "$d/rsx_lpb.h" "$2" "$3" <<'PY' || { echo "[test_lpb] FAIL: mutation $n does not apply"; fails=$((fails+1)); return; }
import sys
src, dst, a, b = sys.argv[1:]
s = open(src).read()
assert s.count(a) == 1, (a, s.count(a))
open(dst, 'w').write(s.replace(a, b))
PY
  build_run "m_$n" "$d"; local rc=$?
  if [[ $rc -eq 0 || $rc -ge 98 ]]; then echo "[test_lpb] FAIL: control $n not caught (rc=$rc)"; fails=$((fails+1)); return; fi
  local c
  for c in $4; do grep -q "FAIL $c " "$O/m_$n.out" || { echo "[test_lpb] FAIL: control $n did not fail check $c"; head -6 "$O/m_$n.out"; fails=$((fails+1)); return; }; done
  echo "[test_lpb] control $n: caught ($(grep -o '^FAIL [a-zA-Z0-9-]*' "$O/m_$n.out" | cut -d' ' -f2 | sort -u | tr '\n' ' '))"
}
control POINT_MAG_IGNORED '(minf == 1u || minf == 3u) && magf == 1u;' '(minf == 1u || minf == 3u) && magf != 99u;' "point-maglinear-is-not"
control POINT_NN_NOT_POINT '(minf == 1u || minf == 3u) && magf == 1u;' '(minf == 1u) && magf == 1u;' "point-nn"
control POINT_MIN_OR_MAG '(minf == 1u || minf == 3u) && magf == 1u;' '(minf == 1u || minf == 3u) || magf == 1u;' "point-minlinear-is-not point-miplinear-is-not"
control BLOCK_ANY_LOCATION 'location == 1u && ' 'location != 77u && ' "block-not-loc0"
control BLOCK_ANY_SIZE 'width == 8u && height == 8u && ' 'width != 77u && height != 77u && ' "block-not-w block-not-h"
control BLOCK_FMT_EXACT '(format & 0x9Fu) == 0x9Au' 'format == 0x9Au' "block-fmt-mask"
control BLOCK_CUBE_OK ' && !cubemap;' ' && cubemap != 77u;' "block-not-cube"
control NO_BIND_COUNT '    l->cur[RSX_LPB_N_BINDS]++;
' '' "binds"
control NO_NONPOINT_COUNT '    if (!pt) l->cur[RSX_LPB_N_NONPOINT]++;
' '' "nonpoint"
control NO_RECYCLED_COUNT '    if (recycled) l->cur[RSX_LPB_N_RECYCLED]++;
' '' "recycled"
control CHANGED_ALWAYS '    if (changed) l->cur[RSX_LPB_N_CHANGED]++;' '    l->cur[RSX_LPB_N_CHANGED]++;' "changed"
control INPLACE_SWAPPED 'l->cur[inplace ? RSX_LPB_N_INPLACE : RSX_LPB_N_REPLACED]++;' 'l->cur[inplace ? RSX_LPB_N_REPLACED : RSX_LPB_N_INPLACE]++;' "inplace replaced"
control NO_GEN_BUMP '    l->gen++;
    if (l->gen == 0)' '    if (l->gen == 0)' "gen-distinct"
control WRAP_NO_CLEAR '        memset(l->tab, 0, sizeof l->tab);
' '' "wrap-no-stale"
control DISTINCT_UNCAPPED 'if (!create || l->n_distinct >= RSX_LPB_MAXB) return NULL;' 'if (!create) return NULL;' "ovf-binds"
control NO_PAGE_DEDUP '        if (pg[i] == pg[i - 1]) continue;
' '' "dpages"
control SPANS_NO_SPLIT '        if (pg[i] - pg[i - 1] > 1u) r0++;
' '' "gen-dspans"
control NO_GAP_MERGE 'if (pg[i] - pg[i - 1] > RSX_LPB_GAP + 1u) r16++;' 'if (pg[i] - pg[i - 1] > 1u) r16++;' "gen-dspans16"
control PAGE_START_ONLY '(b->off + s - 1u) / RSX_LPB_PAGE' '(b->off + s - 1u - (s - 1u)) / RSX_LPB_PAGE' "gen-dpages"
control DIRTY_NEW_ONLY '!(b->flags & (RSX_LPB_F_NEW | RSX_LPB_F_CHANGED))' '!(b->flags & (RSX_LPB_F_NEW))' "gen-dpages"
control USERS_NOT_DEDUP 'if (u->seen_flip != l->gen) {' 'if (1) {' "users"
control NOPSO_MERGED 'u->fp_size == fp_size && u->valid == valid' 'u->fp_size == fp_size' "nopso-separate"
control LINE_CAP_IGNORED 'if (l->lines < l->max_lines)' 'if (1)' "cap-second-silent cap-zero"
control NO_WINDOW_RESET '        memset(l->sum, 0, sizeof l->sum);
        memset(l->max, 0, sizeof l->max);
' '' "window2"
control IDLE_WINDOW_PRINTS 'if (l->win_active) {' 'if (1) {' "inactive-window-silent"
control ANISO_SHIFT 'an = (control0 >> 4) & 7u' 'an = (control0 >> 5) & 7u' "aniso-field"
control FPOFF_NOT_IDENTITY 'u->fp_loc == fp_loc && u->fp_off == fp_off && ' 'u->fp_loc == fp_loc && ' "fpoff-separate"
[[ $fails -eq 0 ]] && echo "[test_lpb] PASS" || { echo "[test_lpb] $fails FAILURE(S)"; exit 1; }
