#!/usr/bin/env bash
# Host test for KZ3 patch 2197: RSX_PUB=1|verify and RSX_PUB_LAG over the replay harness
# (tools/smoke/test_log_replay.c with the walker's real funnel, P and kzr). No game data, no GPU; Linux cc only.
# Captures: the synthetic one built here, and a FIFO capture from a play-through when present (KZ3_FIFO_CAPTURE or work/captures, never committed).
# Per capture it checks, against the BASELINE (the patched toolkit with patch 2197 reverse-applied: the funnel as it was):
#   1. default off is byte-identical: RSX_PUB unset and RSX_PUB=0 give the baseline's guest-memory digest, re-captured event stream, funnel counters
#      and stderr, and P never runs (mode 0, rec 0);
#   2. the guest-write trace (seq kind ea val stream_off, wall-clock report words masked) of RSX_PUB=1, verify and lag 1 / 7 / 64 is IDENTICAL to the
#      direct path's trace (RSX_PUB=0 with the trace hook); the memory digest and re-captured stream equal the baseline's; every record retired, log
#      empty at the end, 0 [pub-bad], no empty / owner violations, the report value computed once (rep == repcalc); a lag run really held records.
# Negative controls (each must be caught, scratch copies of the tree): the retire stores a wrong label word (VAL: digest + verify), the record truncates a
# value (REC: verify + trace), the lag is ignored (NOLAG), the funnel's word path bypasses P (ROUTE: trace), a second
# report-value computation (REP2: C4 counters), RSX_PUB defaulting on (DEFAULTON: identity).
# (the pass-end drain is not observable here: every walker pass ends with a GET_END record, a barrier kind; test_pub_locks.sh covers it.)
# SKIP without the patched toolkit or patch 2197. The baseline is built from the submodule plus the patches below 2197 (not by reversing 2197). A skipped check
# (baseline unbuildable; the optional play-through capture being absent does not count) makes the test FAIL unless KZ3_ALLOW_SKIP=1; the final line then says SKIP.
#   nix develop .#re --command tools/smoke/test_pub_replay.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"; K="$PT/libs/kzr"
source "$ROOT/tools/smoke/pub_srcs.inc"
if [[ ! -f "$V/rsx_pub.c" ]]; then
  if [[ "${KZ3_ALLOW_SKIP:-0}" == 1 ]]; then echo "[test_pub_replay] SKIP: no patched toolkit with patch 2197 ($V) (KZ3_ALLOW_SKIP=1)"; exit 77; fi
  echo "[test_pub_replay] FAIL: no patched toolkit with patch 2197 ($V); run tools/ps3recomp-patched.sh, or KZ3_ALLOW_SKIP=1 to skip"; exit 1
fi
REL="build/smoke-host/pub-replay"; O="$ROOT/$REL"; rm -rf "$O"; mkdir -p "$O/work"
rc=0; fail() { echo "[test_pub_replay] FAIL: $*"; rc=1; }
CF=(-std=gnu17 -O1 -g -w -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread)
build() { # treeroot out [extra gcc args]: the replay harness over the tree's walker (and P when the tree has it)
  local t="$1" out="$2"; shift 2
  local vd="$t/libs/video" ex=() kz="${KZR_DIR:-$K}"; [[ -f "$vd/rsx_tiling.c" ]] && ex=("$vd/rsx_tiling.c")
  pub_src_args "$vd" "$kz"
  "${CC:-gcc}" "${CF[@]}" -I "$t/include" -I "$vd" "$@" "$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" \
      "$vd/rsx_commands.c" "${PUB_SRC[@]}" "${ex[@]}" -lm -o "$out"
}
build "$PT" "$O/t" || { echo "[test_pub_replay] FAIL: build"; exit 1; }
# baseline tree: the funnel as it was before patch 2197 = the pinned submodule's libs/video with every patch numbered below 2197 applied (the video files only;
# the harness does not need the rest). Built from the submodule, NOT by reversing 2197, so a later patch touching 2197's sites cannot silently skip the check.
HAVE_BASE=0; SKIPS=0
skip() { echo "[test_pub_replay] SKIP: $*"; SKIPS=$((SKIPS+1)); }
SUB="$ROOT/third_party/ps3recomp"
if [[ -d "$SUB/libs/video" ]]; then
  mkdir -p "$O/base/libs/video"; cp -a "$SUB/libs/video/." "$O/base/libs/video/"; ln -s "$PT/runtime" "$O/base/runtime"; ln -s "$PT/include" "$O/base/include"
  okb=1
  for pp in "$ROOT"/port/patches/*.patch; do
    n="$(basename "$pp" | cut -d- -f1)"; [[ "$n" =~ ^[0-9]+$ && "$n" -lt 2197 ]] || continue
    grep -q '^+++ b/libs/video/' "$pp" || continue
    (cd "$ROOT" && git apply --include="$REL/base/libs/video/*" --whitespace=nowarn --directory="$REL/base" "$pp") >>"$O/base.log" 2>&1 || { okb=0; break; }
  done
  if [[ $okb == 1 && ! -f "$O/base/libs/video/rsx_pub.h" ]]; then build "$O/base" "$O/tb" && HAVE_BASE=1 || fail "build baseline"
  else skip "identity-to-baseline: the pre-2197 baseline tree could not be built ($(head -1 "$O/base.log" 2>/dev/null | cut -c1-120))"; fi
else skip "identity-to-baseline: no third_party/ps3recomp submodule checkout"; fi
# scratch copies for the negative controls
mkdir_tree() { mkdir -p "$O/$1/libs"; cp -a "$V" "$O/$1/libs/video"; ln -s "$PT/runtime" "$O/$1/runtime"; ln -s "$PT/include" "$O/$1/include"; ln -s "$PT/libs/kzr" "$O/$1/libs/kzr"; }
mkdir_tree n_route; mkdir_tree n_rep2; mkdir_tree n_default
python3 -I - "$O" <<'PY' || { echo "[test_pub_replay] FAIL: control anchors"; exit 1; }
import sys
o = sys.argv[1]
def sub(p, a, b):
    s = open(p).read(); assert s.count(a) == 1, (p, a); open(p, 'w').write(s.replace(a, b))
sub(o + '/n_route/libs/video/rsx_commands.c', "rsx_pub_take('W', kind, ea, val, NULL)", "0 && rsx_pub_take('W', kind, ea, val, NULL)")   # the word path bypasses P
sub(o + '/n_rep2/libs/video/cellGcmSys.c', 'val = gcm_report_value_once(off, val); }', 'val = gcm_report_value_once(off, val); val = gcm_report_value_once(off, val); }')   # a second computation
sub(o + '/n_default/libs/video/rsx_pub.h', 'if (!s || !*s || (s[0] == \'0\' && !s[1])) return RSX_PUB_OFF;', 'if (!s || !*s) return RSX_PUB_ON; if (s[0] == \'0\' && !s[1]) return RSX_PUB_OFF;')   # RSX_PUB unset = on
PY
build "$PT" "$O/t_val" -DRSX_PUB_MUTANT_VAL || fail "build VAL"
build "$PT" "$O/t_rec" -DRSX_PUB_MUTANT_REC || fail "build REC"
build "$PT" "$O/t_nolag" -DRSX_PUB_MUTANT_NOLAG || fail "build NOLAG"
build "$O/n_route" "$O/t_route" || fail "build ROUTE"
build "$O/n_rep2" "$O/t_rep2" || fail "build REP2"
build "$O/n_default" "$O/t_default" || fail "build DEFAULTON"

"$O/t" gen "$O/work/cap" >"$O/gen.out" 2>&1 || { fail "gen: $(tail -2 "$O/gen.out")"; exit 1; }

# run BIN CAPTURE TAG [ENV=VAL ...]: replay with the trace, the digest and the stats written under $O/<tag>.*
run() {
  local bin="$1" cap="$2" tag="$3"; shift 3
  mkdir -p "$O/work/o_$tag"
  env "$@" LD_REPLAY_PUBTRACE="$O/$tag.trace" LD_REPLAY_MEMDIGEST="$O/$tag.dig" "$bin" replay "$cap" "$O/work/o_$tag" >"$O/$tag.out" 2>"$O/$tag.err"
  local r=$?
  [[ $r == 0 || $r == 4 ]] || { fail "$tag: replay rc $r: $(tail -2 "$O/$tag.out") $(head -2 "$O/$tag.err")"; return 1; }
}
st() { sed -n "s/.*\[replay-pub\].* $2=\([0-9]*\).*/\1/p" "$O/$1.out" | head -1; }   # st TAG FIELD
cmpev() { "$O/t" cmp "$O/work/o_$1/fifo-capture.kz3fc" "$O/work/o_$2/fifo-capture.kz3fc" 2>&1 | grep -c "mismatches 0"; }
gsline() { grep -m1 '^\[replay-gs\]' "$O/$1.out"; }
errfilter() { grep -v '^\[pub\]' "$O/$1.err" | sed -e 's@/o_[A-Za-z0-9._]*@/o_X@g' -e 's/[0-9]*\.[0-9]* *ms//g' -e 's/t=[0-9.]*//g'; }

one() { # label capture
  local L="$1" cap="$2"
  echo "[test_pub_replay] == $L"
  run "$O/t" "$cap" "$L.unset" || return
  run "$O/t" "$cap" "$L.off" RSX_PUB=0 || return
  run "$O/t" "$cap" "$L.on" RSX_PUB=1 || return
  run "$O/t" "$cap" "$L.ver" RSX_PUB=verify || return
  # the direct path's trace (RSX_PUB unset, the hook only) is the reference for every other mode
  local n; n=$(wc -l <"$O/$L.unset.trace"); [[ "$n" -gt 20 ]] || fail "$L: too few guest signals traced ($n)"
  [[ "$(st "$L.unset" mode)" == 0 && "$(st "$L.unset" rec)" == 0 && "$(st "$L.off" mode)" == 0 && "$(st "$L.off" rec)" == 0 ]] || fail "$L: default off ran P: $(grep -m1 replay-pub "$O/$L.unset.out") / $(grep -m1 replay-pub "$O/$L.off.out")"
  cmp -s "$O/$L.unset.trace" "$O/$L.off.trace" || fail "$L: unset and RSX_PUB=0 traces differ"
  if [[ $HAVE_BASE == 1 ]]; then
    run "$O/tb" "$cap" "$L.base" || return
    cmp -s "$O/$L.base.dig" "$O/$L.unset.dig" || fail "$L: default-off guest memory differs from the baseline: $(diff "$O/$L.base.dig" "$O/$L.unset.dig" | head -3 | tr '\n' ' ')"
    [[ "$(gsline "$L.base")" == "$(gsline "$L.unset")" ]] || fail "$L: default-off funnel counters differ from the baseline"
    [[ "$(cmpev "$L.base" "$L.unset")" == 1 ]] || fail "$L: default-off re-captured stream differs from the baseline"
    [[ "$(errfilter "$L.base")" == "$(errfilter "$L.unset")" ]] || fail "$L: default-off stderr differs from the baseline: $(diff <(errfilter "$L.base") <(errfilter "$L.unset") | head -3 | tr '\n' ' ')"
    echo "[test_pub_replay] $L default off = baseline: memory digest, event stream, funnel counters, stderr ($(wc -l <"$O/$L.base.dig") pages)"
  fi
  [[ "$(sed -n 's/^\[replay-hbe\] //p' "$O/$L.unset.out")" == 11223344 ]] || fail "$L: default-off direct H-store (report word, big-endian) stored '$(sed -n 's/^\[replay-hbe\] //p' "$O/$L.unset.out")', want 11223344"
  if [[ $HAVE_BASE == 1 ]]; then [[ "$(grep -m1 '^\[replay-hbe\]' "$O/$L.base.out")" == "$(grep -m1 '^\[replay-hbe\]' "$O/$L.unset.out")" ]] || fail "$L: default-off H-store differs from the baseline"; fi
  local bdig="$O/$L.unset.dig"; [[ $HAVE_BASE == 1 ]] && bdig="$O/$L.base.dig"
  local tag
  for tag in on ver; do
    cmp -s "$O/$L.unset.trace" "$O/$L.$tag.trace" || fail "$L: RSX_PUB=$tag trace differs from the direct path: $(diff "$O/$L.unset.trace" "$O/$L.$tag.trace" | head -3 | tr '\n' ' ')"
    cmp -s "$bdig" "$O/$L.$tag.dig" || fail "$L: RSX_PUB=$tag guest memory differs: $(diff "$bdig" "$O/$L.$tag.dig" | head -3 | tr '\n' ' ')"
    [[ "$(cmpev "$L.unset" "$L.$tag")" == 1 ]] || fail "$L: RSX_PUB=$tag re-captured stream differs"
    [[ "$(st "$L.$tag" rec)" == "$n" && "$(st "$L.$tag" retired)" == "$n" && "$(st "$L.$tag" pending)" == 0 ]] || fail "$L: RSX_PUB=$tag rec/retired/pending: $(grep -m1 replay-pub "$O/$L.$tag.out") (want $n traced)"
    [[ "$(st "$L.$tag" bad)" == 0 && "$(st "$L.$tag" empty_bad)" == 0 && "$(st "$L.$tag" owner_bad)" == 0 ]] || fail "$L: RSX_PUB=$tag violations: $(grep -m1 replay-pub "$O/$L.$tag.out")"
    [[ "$(st "$L.$tag" rep)" == "$(st "$L.$tag" repcalc)" ]] || fail "$L: RSX_PUB=$tag report value computed $(st "$L.$tag" repcalc)x for $(st "$L.$tag" rep) reports"
    grep -q '^\[pub-bad\]' "$O/$L.$tag.err" && fail "$L: RSX_PUB=$tag printed [pub-bad]: $(grep -m1 '^\[pub-bad\]' "$O/$L.$tag.err")"
  done
  [[ "$(st "$L.on" stores)" -gt 5 && "$(st "$L.on" barriers)" -gt 0 && "$(st "$L.on" held_max)" -ge 1 ]] || fail "$L: P made no stores or saw no barrier: $(grep -m1 replay-pub "$O/$L.on.out")"
  echo "[test_pub_replay] $L RSX_PUB=1 / verify: $n signals, trace = direct trace, memory = baseline, 0 bad ($(grep -m1 replay-pub "$O/$L.on.out" | cut -c14-130))"
  local lag prev_held=1
  for lag in 1 7 64; do
    run "$O/t" "$cap" "$L.lag$lag" RSX_PUB=1 RSX_PUB_LAG=$lag || return
    run "$O/t" "$cap" "$L.vlag$lag" RSX_PUB=verify RSX_PUB_LAG=$lag || return
    for tag in lag$lag vlag$lag; do
      cmp -s "$O/$L.unset.trace" "$O/$L.$tag.trace" || fail "$L: $tag trace differs from the direct path: $(diff "$O/$L.unset.trace" "$O/$L.$tag.trace" | head -3 | tr '\n' ' ')"
      cmp -s "$bdig" "$O/$L.$tag.dig" || fail "$L: $tag guest memory differs: $(diff "$bdig" "$O/$L.$tag.dig" | head -3 | tr '\n' ' ')"
      [[ "$(cmpev "$L.unset" "$L.$tag")" == 1 ]] || fail "$L: $tag re-captured stream differs"
      [[ "$(st "$L.$tag" rec)" == "$n" && "$(st "$L.$tag" retired)" == "$n" && "$(st "$L.$tag" pending)" == 0 ]] || fail "$L: $tag rec/retired/pending: $(grep -m1 replay-pub "$O/$L.$tag.out")"
      [[ "$(st "$L.$tag" bad)" == 0 && "$(st "$L.$tag" empty_bad)" == 0 && "$(st "$L.$tag" owner_bad)" == 0 ]] || fail "$L: $tag violations: $(grep -m1 replay-pub "$O/$L.$tag.out")"
    done
    local h; h="$(st "$L.lag$lag" held_max)"
    [[ "${h:-0}" -ge 2 ]] || fail "$L: lag $lag held at most ${h:-0} record(s): the lag is not real"
    [[ "${h:-0}" -le $((lag + 1)) ]] || fail "$L: lag $lag held $h records (more than lag + the new one)"
    [[ "${h:-0}" -ge "$prev_held" ]] || fail "$L: lag $lag held fewer records ($h) than the smaller lag ($prev_held)"
    prev_held="${h:-1}"
    echo "[test_pub_replay] $L lag $lag: trace, memory, stream identical; held_max $h"
  done
  # ---- negative controls (each must be caught)
  run "$O/t_val" "$cap" "$L.n_val" RSX_PUB=1 && { cmp -s "$bdig" "$O/$L.n_val.dig" && fail "$L: control VAL (wrong label word) not caught by the memory digest" || echo "[test_pub_replay] $L control VAL: memory digest differs as it must"; }
  run "$O/t_val" "$cap" "$L.n_valv" RSX_PUB=verify && { [[ "$(st "$L.n_valv" bad)" -gt 0 ]] && echo "[test_pub_replay] $L control VAL: verify counts $(st "$L.n_valv" bad) [pub-bad]" || fail "$L: control VAL not caught by verify"; }
  run "$O/t_rec" "$cap" "$L.n_rec" RSX_PUB=verify && { [[ "$(st "$L.n_rec" bad)" -gt 0 ]] && echo "[test_pub_replay] $L control REC: verify counts $(st "$L.n_rec" bad) [pub-bad]" || fail "$L: control REC not caught by verify"; }
  run "$O/t_rec" "$cap" "$L.n_rect" RSX_PUB=1 && { cmp -s "$O/$L.unset.trace" "$O/$L.n_rect.trace" && fail "$L: control REC not caught by the trace" || echo "[test_pub_replay] $L control REC: trace differs as it must"; }
  run "$O/t_nolag" "$cap" "$L.n_nl" RSX_PUB=1 RSX_PUB_LAG=7 && { [[ "$(st "$L.n_nl" held_max)" -ge 2 ]] && fail "$L: control NOLAG not caught (the ignored lag still held records)" || echo "[test_pub_replay] $L control NOLAG: held_max $(st "$L.n_nl" held_max) with RSX_PUB_LAG=7 (the lag check fails as it must)"; }
  run "$O/t_route" "$cap" "$L.n_rt" RSX_PUB=1 && { cmp -s "$O/$L.unset.trace" "$O/$L.n_rt.trace" && fail "$L: control ROUTE (word path bypasses P) not caught by the trace" || echo "[test_pub_replay] $L control ROUTE: trace differs as it must ($(wc -l <"$O/$L.n_rt.trace") of $n lines)"; }
  run "$O/t_rep2" "$cap" "$L.n_r2" RSX_PUB=verify && { [[ "$(st "$L.n_r2" rep)" != "$(st "$L.n_r2" repcalc)" && "$(st "$L.n_r2" bad)" -gt 0 ]] && echo "[test_pub_replay] $L control REP2: rep $(st "$L.n_r2" rep) repcalc $(st "$L.n_r2" repcalc), bad $(st "$L.n_r2" bad)" || fail "$L: control REP2 (report value computed twice) not caught"; }
  run "$O/t_default" "$cap" "$L.n_df" && { [[ "$(st "$L.n_df" mode)" != 0 || "$(st "$L.n_df" rec)" != 0 ]] && echo "[test_pub_replay] $L control DEFAULTON: unset ran P (mode $(st "$L.n_df" mode), rec $(st "$L.n_df" rec)): the identity check fails as it must" || fail "$L: control DEFAULTON not caught"; }
}
one synth "$O/work/cap/fifo-capture.kz3fc"
RCAP="${KZ3_FIFO_CAPTURE:-$ROOT/work/captures/fifo-capture.kz3fc}"
if [[ -f "$RCAP" ]]; then one playthrough "$RCAP"; else echo "[test_pub_replay] SKIP playthrough: no capture ($RCAP)"; fi
if [[ $rc != 0 ]]; then echo "[test_pub_replay] FAIL"; exit 1; fi
if [[ $SKIPS -gt 0 ]]; then
  if [[ "${KZ3_ALLOW_SKIP:-0}" == 1 ]]; then echo "[test_pub_replay] SKIP ($SKIPS check(s) skipped, KZ3_ALLOW_SKIP=1; the default-off identity was NOT proven)"; exit 77; fi
  echo "[test_pub_replay] FAIL: $SKIPS check(s) skipped (set KZ3_ALLOW_SKIP=1 to accept)"; exit 1
fi
echo "[test_pub_replay] PASS"
