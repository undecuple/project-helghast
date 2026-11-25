#!/usr/bin/env bash
# Host test for KZ3 patch 2172 (LD_FRONT_PROBE): the GPU-free unit libs/video/rsx_front_probe.h with a fake clock (known inputs
# -> known counters and T, negative controls, gates at their thresholds), built plain and with -fsanitize=thread; then, when
# the replay harness sources are there, the real CG walker over a synthetic capture with LD_FRONT_PROBE=1 (the probe must not
# change the call-stream digest, and its [front-probe] lines must appear). SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_front_probe.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
source "$ROOT/tools/smoke/pub_srcs.inc"; pub_src_args "$V" "$PT/libs/kzr"   # patch 2197: the harness links the publisher
if [[ ! -f "$V/cellGcmSys.c" || ! -d "$PT/include" ]]; then echo "[test_front_probe] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
if [[ ! -f "$V/rsx_front_probe.h" ]]; then echo "[test_front_probe] SKIP: toolkit without patch 2172"; exit 77; fi
O="$ROOT/build/smoke-host/front_probe"; mkdir -p "$O"
rc=0
gcc -std=gnu17 -O1 -g -Wall -Wno-unused-function -I "$V" -pthread "$ROOT/tools/smoke/test_front_probe.c" -o "$O/unit" || { echo "[test_front_probe] FAIL: unit build"; exit 1; }
"$O/unit" || rc=1
# negative control: the old fixed table sizes (RSX_FP_NOGROW) must give the wrong answer on 40,000 spans / 3000 programs
gcc -std=gnu17 -O1 -g -Wall -Wno-unused-function -DRSX_FP_NOGROW -I "$V" -pthread "$ROOT/tools/smoke/test_front_probe.c" -o "$O/unit_nogrow" || { echo "[test_front_probe] FAIL: control build"; exit 1; }
"$O/unit_nogrow" || rc=1
if gcc -std=gnu17 -O1 -g -fsanitize=thread -Wno-unused-function -I "$V" -pthread "$ROOT/tools/smoke/test_front_probe.c" -o "$O/unit_tsan" 2>"$O/tsan_build.err"; then
  if "$O/unit_tsan" >"$O/tsan.out" 2>"$O/tsan.err"; then
    grep -q "ThreadSanitizer" "$O/tsan.err" && { echo "[test_front_probe] FAIL: TSan report"; head -20 "$O/tsan.err"; rc=1; } || echo "[test_front_probe] unit under -fsanitize=thread: no report"
  else echo "[test_front_probe] FAIL: unit under TSan"; tail -20 "$O/tsan.out" "$O/tsan.err"; rc=1; fi
else echo "[test_front_probe] SKIP tsan: sanitizer not available ($(head -1 "$O/tsan_build.err"))"; fi
# CG walker with LD_FRONT_PROBE=1 over the sync-hist test's synthetic FIFO: the probe changes nothing the walker does
if [[ -f "$ROOT/tools/smoke/test_sync_hist.c" && -f "$V/rsx_sync_hist.h" ]]; then
  CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
  EXTRA_SRC=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA_SRC=("$V/rsx_tiling.c")
  if gcc "${CF[@]}" "$ROOT/tools/smoke/test_sync_hist.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "${EXTRA_SRC[@]}" -o "$O/walker"; then
    plain="$("$O/walker" plain 2>/dev/null)"; probe="$(LD_FRONT_PROBE=1 LD_FRONT_PROBE_N=2 "$O/walker" plain 2>"$O/probe.err")"
    dg() { sed -n 's/.*digest //p' <<<"$1" | tail -1; }
    if [[ -n "$(dg "$plain")" && "$(dg "$plain")" == "$(dg "$probe")" ]]; then echo "[test_front_probe] walker digest with LD_FRONT_PROBE=1 == plain ($(dg "$plain"))"
    else echo "[test_front_probe] FAIL: call-stream digest differs with the probe on"; rc=1; fi
    grep -q '^\[front-probe\] on' "$O/probe.err" || { echo "[test_front_probe] FAIL: no [front-probe] on line"; rc=1; }
    n="$(grep -c '^\[front-probe\] window' "$O/probe.err")"; echo "[test_front_probe] window lines: $n; summary lines: $(grep -c '^\[front-probe\] summary' "$O/probe.err")"
    [[ "$n" -ge 1 ]] || { echo "[test_front_probe] FAIL: no window line from the walker run"; rc=1; }
  else echo "[test_front_probe] FAIL: walker build"; rc=1; fi
fi
# the replay harness (patch 2171 tools) over a synthetic capture and, when present, a FIFO capture from a play-through (not in the repo), with LD_FRONT_PROBE=1:
# the probe must not change the replayed event stream (cmp against the replay without it), and its counters are printed
if [[ -f "$ROOT/tools/smoke/test_log_replay.c" ]] && grep -q rsx_fc_range_check "$V/rsx_fifo_capture.h" 2>/dev/null; then
  R="$O/replay"; rm -rf "$R"; mkdir -p "$R/work"
  CF2=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
  EXTRA2=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA2=("$V/rsx_tiling.c")
  if gcc "${CF2[@]}" "$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" "$V/rsx_commands.c" "${PUB_SRC[@]}" "${EXTRA2[@]}" -o "$R/t"; then
    "$R/t" gen "$R/work/cap" >"$R/gen.out" 2>&1 || { echo "[test_front_probe] FAIL: gen"; tail -2 "$R/gen.out"; rc=1; }
    CAP="$R/work/cap/fifo-capture.kz3fc"
    "$R/t" replay "$CAP" "$R/work/r0" >"$R/r0.out" 2>&1 || { echo "[test_front_probe] FAIL: replay without the probe"; tail -2 "$R/r0.out"; rc=1; }
    LD_FRONT_PROBE=1 LD_FRONT_PROBE_N=2 "$R/t" replay "$CAP" "$R/work/r1" >"$R/r1.out" 2>"$R/r1.err" || { echo "[test_front_probe] FAIL: replay with the probe"; tail -2 "$R/r1.out"; rc=1; }
    "$R/t" cmp "$R/work/r0/fifo-capture.kz3fc" "$R/work/r1/fifo-capture.kz3fc" >"$R/c.out" 2>&1 && grep -q "mismatches 0" "$R/c.out" \
      && echo "[test_front_probe] synthetic replay: event stream with the probe == without ($(sed -n 's/.*events \([0-9]*\) vs.*/\1 events/p' "$R/c.out"))" || { echo "[test_front_probe] FAIL: the probe changed the replayed stream: $(cat "$R/c.out")"; rc=1; }
    grep -q '^\[front-probe\] window' "$R/r1.err" || { echo "[test_front_probe] FAIL: no [front-probe] window line in the synthetic replay"; rc=1; }
    grep -m1 '^\[front-probe\] summary' "$R/r1.err" | cut -c1-600
    RC="${KZ3_REPLAY_CAP:-$ROOT/work/captures/fifo-capture.kz3fc}"
    if [[ -f "$RC" ]]; then
      mkdir -p "$R/work/real"
      LD_FRONT_PROBE=1 LD_FRONT_PROBE_N=1 "$R/t" replay "$RC" "$R/work/real" >"$R/real.out" 2>"$R/real.err"; echo "[test_front_probe] real capture replay exit $? ($(grep -h '^\[replay\] records' "$R/real.out" | head -1 | cut -c1-120))"
      grep -h '^\[front-probe\] \(window\|summary\)' "$R/real.err" | cut -c1-1500 | tail -4
    else echo "[test_front_probe] no real capture ($RC): skipped"; fi
  else echo "[test_front_probe] FAIL: replay harness build"; rc=1; fi
fi
[[ $rc -eq 0 ]] && echo "[test_front_probe] PASS" || { echo "[test_front_probe] FAIL"; exit 1; }
