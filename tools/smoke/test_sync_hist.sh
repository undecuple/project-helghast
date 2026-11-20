#!/usr/bin/env bash
# Host test for KZ3 patch 2164 (LD_SYNC_HIST and the LD_FIFO_CAPTURE walker hooks): a synthetic FIFO of known frames goes
# through the real walker (cellGcmSys.c with stubs). plain = switches off, the call-stream digest is the reference; hist = the
# classifier must match the hand-worked numbers; capture / capture-a = the capture file must read back clean and hold the
# walker's commands, label writes and the NV0039 source bytes / result digest, with the same digest as plain (the hooks
# change nothing). SKIP without the patches. The capture goes to a 'work' directory under build/ (never committed).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_sync_hist.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/cellGcmSys.c" || ! -d "$PT/include" ]]; then
  echo "[test_sync_hist] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if [[ ! -f "$V/rsx_sync_hist.h" || ! -f "$V/rsx_fifo_capture.h" ]]; then
  echo "[test_sync_hist] SKIP: toolkit without patch 2164"; exit 0; fi
O="$ROOT/build/smoke-host/sync_hist"; mkdir -p "$O/work"
CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
EXTRA_SRC=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA_SRC=("$V/rsx_tiling.c")
gcc "${CF[@]}" "$ROOT/tools/smoke/test_sync_hist.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "${EXTRA_SRC[@]}" -o "$O/t"
rc=0
rm -f "$O/work/cap/fifo-capture.kz3fc" "$O/work/capa/fifo-capture.kz3fc"
plain="$("$O/t" plain)" || { echo "$plain"; rc=1; }
echo "$plain" | tail -3
hist="$("$O/t" hist 2>"$O/hist.err")" || { echo "$hist"; rc=1; }
echo "$hist" | tail -3; grep -c '^\[sync-hist\] window' "$O/hist.err" | sed 's/^/[test_sync_hist] window lines: /'
[[ "$(grep -c '^\[sync-hist\] window' "$O/hist.err")" == 2 && "$(grep -c '^\[sync-hist\] summary' "$O/hist.err")" == 1 ]] || { echo "[test_sync_hist] FAIL: expected 2 window lines (N=2) and 1 summary line"; rc=1; }
cap="$("$O/t" capture "$O/work/cap" 2>"$O/cap.err")" || { echo "$cap"; rc=1; }
echo "$cap" | tail -3
capa="$("$O/t" capture-a "$O/work/capa" 2>"$O/capa.err")" || { echo "$capa"; rc=1; }
echo "$capa" | tail -3
dg() { sed -n 's/.*digest //p' <<<"$1" | tail -1; }
[[ -n "$(dg "$plain")" && "$(dg "$plain")" == "$(dg "$hist")" && "$(dg "$plain")" == "$(dg "$cap")" && "$(dg "$plain")" == "$(dg "$capa")" ]] || { echo "[test_sync_hist] FAIL: call-stream digest differs between plain/hist/capture"; rc=1; }
# a capture directory without a 'work' path segment is refused and nothing is written (relative path: no segment)
( cd "$O" && rm -rf refused_dir && "$O/t" refuse refused_dir >"$O/refuse.out" 2>&1 ) || { cat "$O/refuse.out" | tail -3; rc=1; }
grep -q 'refused: LD_FIFO_CAPTURE=refused_dir has no' "$O/refuse.out" || { echo "[test_sync_hist] FAIL: no refusal message"; rc=1; }
[[ -e "$O/refused_dir" ]] && { echo "[test_sync_hist] FAIL: capture directory created outside work"; rc=1; }
[[ $rc -eq 0 ]] && echo "[test_sync_hist] PASS" || { echo "[test_sync_hist] FAIL"; exit 1; }
