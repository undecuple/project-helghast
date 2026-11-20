#!/usr/bin/env bash
# Host test for KZ3 patch 2189 (command-thread overhaul O1): GCM_LEGACY_STATE_SKIP (default skip, 0 off, verify) and the
# GCM_FAST_METHODS default. Needs no game data and no GPU. Checks:
#  1. the pure core (tools/smoke/test_legacy_skip.c): mode parse, the exact effect list, reader accounting, line format
#  2. static: switch names and defaults in the patched sources, the 12 reader-counting sites in rsx_commands.c, the null backend's
#     state-pointer callbacks read `state` only under ps3_log_verbose()
#  3. replay (the real walker, tools/smoke/test_log_replay.c, synthetic capture): skip, off and verify give the same re-captured
#     event stream (register digest per draw, guest writes, ranges), skip really leaves methods out, verify runs them all and
#     finds 0 readers; negative control: a build that drops 0x1D74 from the list diverges
#  4. the real in-level capture work/captures when present (skip vs off: both 0 mismatches), else SKIP
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_legacy_skip.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
source "$ROOT/tools/smoke/pub_srcs.inc"; pub_src_args "$V" "$PT/libs/kzr"   # patch 2197: the harness links the publisher
if [[ ! -f "$V/rsx_legacy_skip.h" ]]; then echo "[test_legacy_skip] SKIP: no patched toolkit with patch 2189 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/legacy-skip"; rm -rf "$O"; mkdir -p "$O/work"
CC="${CC:-cc}"; rc=0
fail() { echo "[test_legacy_skip] FAIL: $*"; rc=1; }
# 1. core
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_legacy_skip.c" -o "$O/core" || { echo "[test_legacy_skip] FAIL: build core"; exit 1; }
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$V" -DRSX_LGS_NEG_DROP=0x1D74 "$ROOT/tools/smoke/test_legacy_skip.c" -o "$O/core_neg" || { echo "[test_legacy_skip] FAIL: build core_neg"; exit 1; }
(ulimit -v 4194304 2>/dev/null; "$O/core") || fail "core"   # the pure core runs under the 4 GB cap; the replay harness reserves 4 GB of NORESERVE address space itself, so it cannot
"$O/core_neg" || fail "core negative control build"
# 2. static
G="$V/cellGcmSys.c"; R="$V/rsx_commands.c"
grep -qF 'rsx_lgs_mode(getenv("GCM_LEGACY_STATE_SKIP"))' "$G" || fail "GCM_LEGACY_STATE_SKIP not read through rsx_lgs_mode"
grep -qF 'getenv("GCM_FAST_METHODS"); on = e && *e ? atoi(e) != 0 : 1;' "$G" || fail "GCM_FAST_METHODS default is not 1"
grep -qF 'rsx_lgs_effect(m)' "$G" || fail "gcm_legacy_method_has_effect no longer uses the shared list"
n=$(grep -c 'g_rsx_lg_rd_state++\|g_rsx_lg_rd_param++' "$R"); [[ "$n" == 12 ]] || fail "reader-counting sites in rsx_commands.c: $n (want 12: 8 set_*, bind_texture, clear, 2 draws)"
N="$V/rsx_null_backend.c"
for f in null_set_render_target null_set_viewport; do
  body="$(awk -v f="static void $f" 'index($0,f)==1{p=1} p{print} p&&/^}/{exit}' "$N")"
  [[ "$(grep -c 'state->' <<<"$body")" -ge 1 && "$(grep -c 'ps3_log_verbose' <<<"$body")" == 1 ]] || fail "$f: state is read outside ps3_log_verbose()"
  grep -q '^ *if (ps3_log_verbose()) printf' <<<"$body" || fail "$f: the only state read must be the verbose log"
done
echo "[test_legacy_skip] static checks done"
# 3. replay
CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
EXTRA=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA=("$V/rsx_tiling.c")
SRC=("$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" "$V/rsx_commands.c" "${PUB_SRC[@]}" "${EXTRA[@]}")
gcc "${CF[@]}" "${SRC[@]}" -o "$O/t" || { echo "[test_legacy_skip] FAIL: build replay"; exit 1; }
gcc "${CF[@]}" -DRSX_LGS_NEG_DROP=0x1D74 "${SRC[@]}" -o "$O/t_neg" || { echo "[test_legacy_skip] FAIL: build replay (negative control)"; exit 1; }
T="$O/t"; W="$O/work"
"$T" gen "$W/cap" >"$O/gen.out" 2>&1 || { fail "gen: $(tail -2 "$O/gen.out")"; exit 1; }
CAP="$W/cap/fifo-capture.kz3fc"
lgs() { sed -n 's/^\[replay-lgs\] //p' "$1" | head -1; }
val() { sed -n "s/.*\b$2=\([0-9]*\).*/\1/p" <<<"$1" | head -1; }
declare -A ln
for mode in skip off verify; do
  case $mode in skip) E=();; off) E=(GCM_LEGACY_STATE_SKIP=0);; verify) E=(GCM_LEGACY_STATE_SKIP=verify);; esac
  mkdir -p "$W/r_$mode"
  env "${E[@]}" "$T" replay "$CAP" "$W/r_$mode" >"$O/r_$mode.out" 2>"$O/r_$mode.err" || fail "replay $mode: $(tail -2 "$O/r_$mode.out") $(head -3 "$O/r_$mode.err")"
  "$T" cmp "$CAP" "$W/r_$mode/fifo-capture.kz3fc" >"$O/c_$mode.out" 2>&1 || fail "$mode: re-capture differs from the capture: $(cat "$O/c_$mode.out")"
  grep -q "mismatches 0" "$O/c_$mode.out" || fail "$mode cmp line: $(cat "$O/c_$mode.out")"
  ln[$mode]="$(lgs "$O/r_$mode.out")"
done
"$T" cmp "$W/r_skip/fifo-capture.kz3fc" "$W/r_off/fifo-capture.kz3fc" >"$O/c_so.out" 2>&1 && grep -q "mismatches 0" "$O/c_so.out" || fail "skip and off streams differ: $(cat "$O/c_so.out")"
ev="$(sed -n 's/.*events \([0-9]*\) vs.*/\1/p' "$O/c_so.out")"; [[ "${ev:-0}" -gt 200 ]] || fail "too few events compared: ${ev:-0}"
m=$(val "${ln[skip]}" m3d); r=$(val "${ln[skip]}" ran); s=$(val "${ln[skip]}" skipped)
[[ "${m:-0}" -gt 0 && "$((r + s))" == "$m" && "${s:-0}" -gt "${r:-0}" && "${r:-0}" -gt 0 ]] || fail "skip counters: ${ln[skip]}"
[[ "$(val "${ln[off]}" skipped)" == 0 && "$(val "${ln[off]}" ran)" == "$(val "${ln[off]}" m3d)" ]] || fail "off counters: ${ln[off]}"
[[ "$(val "${ln[verify]}" skipped)" == 0 && "$(val "${ln[verify]}" ran)" == "$(val "${ln[verify]}" m3d)" && "$(val "${ln[verify]}" readers)" == 0 ]] || fail "verify counters: ${ln[verify]}"
[[ "$(val "${ln[off]}" m3d)" == "$m" ]] || fail "skip and off saw a different number of methods"
echo "[test_legacy_skip] replay: events $ev identical (skip = off = capture) | skip: ${ln[skip]} | off: ${ln[off]} | verify: ${ln[verify]}"
# negative control: 0x1D74 (the label write) wrongly skipped
mkdir -p "$W/r_neg"
"$O/t_neg" replay "$CAP" "$W/r_neg" >"$O/r_neg.out" 2>"$O/r_neg.err"; nrc=$?
"$O/t_neg" cmp "$CAP" "$W/r_neg/fifo-capture.kz3fc" >"$O/c_neg.out" 2>&1; ncc=$?
if [[ $nrc == 0 && $ncc == 0 ]] && grep -q "mismatches 0" "$O/c_neg.out"; then fail "negative control (0x1D74 skipped) was NOT flagged"; else echo "[test_legacy_skip] negative control 0x1D74 skipped flagged (replay rc $nrc, cmp rc $ncc: $(head -c 120 "$O/c_neg.out" | tr '\n' ' '))"; fi
# 4. real capture
RC="$ROOT/work/captures"
if [[ -f "$RC/fifo-capture.kz3fc" ]]; then
  for mode in skip off; do
    case $mode in skip) E=();; off) E=(GCM_LEGACY_STATE_SKIP=0);; esac
    env "${E[@]}" LD_FIFO_REPLAY="$RC" "$T" >"$O/real_$mode.out" 2>"$O/real_$mode.err"; rrc=$?
    echo "[test_legacy_skip] playthrough $mode: rc $rrc | $(grep -h '^\[replay\] records' "$O/real_$mode.out" | head -1) | $(lgs "$O/real_$mode.out") | $(grep -h '^\[cmp\]' "$O/real_$mode.out" | head -1)"
    [[ $rrc == 0 ]] || fail "playthrough $mode replay rc $rrc: $(tail -2 "$O/real_$mode.err" | cut -c1-200)"
  done
else echo "[test_legacy_skip] SKIP playthrough: no work/captures capture"; fi
[[ $rc == 0 ]] && echo "[test_legacy_skip] PASS" || { echo "[test_legacy_skip] FAIL"; exit 1; }
