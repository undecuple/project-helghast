#!/usr/bin/env bash
# Host test for KZ3 patch 2199: the default-on "[frame] first frame presented at N ms" line (libs/video/rsx_first_frame.h)
# prints exactly once; static checks that the patched rsx_live_draw.c calls it only after a successful, non-occluded
# Present and behind the pending test. Negative controls: a header that prints on every call, one that never prints,
# a non-atomic check-then-set (if (done == 0 && (done = 1))), and a live-draw copy without the call; each must make the test fail.
# The race part starts 4 threads behind a barrier on the very first call, 20000 rounds with fresh state. No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_first_frame.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_first_frame.h" ]]; then echo "[test_first_frame] SKIP: no patched toolkit with patch 2199 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/first-frame"; mkdir -p "$O"
CC="${CC:-cc}"

run_c() {   # $1 = include dir with rsx_first_frame.h; returns the test's exit status
  rm -f "$O/t"   # a failed build must never run a stale binary (and must not count as a caught control)
  $CC -std=c11 -Wall -Wextra -Werror -O2 -I"$1" "$ROOT/tools/smoke/test_first_frame.c" -o "$O/t" -lpthread || { echo "[test_first_frame] build failed"; return 99; }
  (cd "$O" && ./t "$O/stderr.txt")
}
static_ok() {   # $1 = rsx_live_draw.c; 0 when the call site is right
  python3 - "$1" <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
if '#include "rsx_first_frame.h"' not in s: sys.exit(1)
i = s.find("static HRESULT ld_present_swap")
j = s.find("\n}\n", i)
body = s[i:j]
p = body.find("->Present(g.swap")
n = body.find("rsx_first_frame_note(")
g = body.find("rsx_first_frame_pending()")
if min(p, n, g) < 0 or not (p < g < n): sys.exit(1)
# the guard must also exclude occluded and failed presents
guard = body[body.rfind("if (", 0, g): g + 40]
if "SUCCEEDED(hr)" not in guard or "DXGI_STATUS_OCCLUDED" not in guard: sys.exit(1)
# once only: the note appears once in the function
if body.count("rsx_first_frame_note(") != 1: sys.exit(1)
EOF
}

out="$(run_c "$V")" || { echo "$out"; exit 1; }
echo "$out" | grep -v "^\[PASS\]" || true
static_ok "$V/rsx_live_draw.c" || { echo "[FAIL] ld_present_swap does not call the marker after a good Present behind the pending test"; exit 1; }
echo "[test_first_frame] ok"

# negative controls
N="$O/neg"; rm -rf "$N"; mkdir -p "$N"
sed 's/if (atomic_exchange(&s_rsx_ff_done, 1) == 0) {/if (1) {/' "$V/rsx_first_frame.h" > "$N/rsx_first_frame.h"
cmp -s "$V/rsx_first_frame.h" "$N/rsx_first_frame.h" && { echo "[FAIL] control 1 did not change the header"; exit 1; }
rcn=0; run_c "$N" >/dev/null 2>&1 || rcn=$?; if [[ $rcn != 1 ]]; then echo "[FAIL] negative control 'prints every call' passed or did not build (rc $rcn)"; exit 1; fi
sed 's/fprintf(stderr, "\[frame\] first frame presented at %llu ms\\n", ms);/(void)ms;/' "$V/rsx_first_frame.h" > "$N/rsx_first_frame.h"
cmp -s "$V/rsx_first_frame.h" "$N/rsx_first_frame.h" && { echo "[FAIL] control 2 did not change the header"; exit 1; }
rcn=0; run_c "$N" >/dev/null 2>&1 || rcn=$?; if [[ $rcn != 1 ]]; then echo "[FAIL] negative control 'never prints' passed or did not build (rc $rcn)"; exit 1; fi
# negative control: check-then-set that is not one atomic step; the racing rounds must catch it, every time (repeated)
sed 's/if (atomic_exchange(&s_rsx_ff_done, 1) == 0) {/if (s_rsx_ff_done == 0 \&\& (s_rsx_ff_done = 1)) {/' "$V/rsx_first_frame.h" > "$N/rsx_first_frame.h"
cmp -s "$V/rsx_first_frame.h" "$N/rsx_first_frame.h" && { echo "[FAIL] control 3 did not change the header"; exit 1; }
caught=0; tries=${KZ3_FF_TRIES:-5}
for ((k = 0; k < tries; k++)); do rcn=0; run_c "$N" >"$O/neg_race.out" 2>&1 || rcn=$?; [[ $rcn == 99 ]] && { echo "[FAIL] control build failed"; exit 1; }; [[ $rcn == 1 ]] && caught=$((caught + 1)); done
if [[ $caught -ne $tries ]]; then echo "[FAIL] negative control 'non-atomic check-then-set' caught in $caught of $tries runs (must be every run)"; exit 1; fi
echo "[test_first_frame] control non-atomic check-then-set: caught in $caught of $tries runs ($(grep -m1 'racing rounds printed the line more than once' "$O/neg_race.out" | cut -c1-90))"
grep -v "rsx_first_frame_note(cv.QuadPart" "$V/rsx_live_draw.c" > "$N/rsx_live_draw.c"
if static_ok "$N/rsx_live_draw.c"; then echo "[FAIL] negative control 'call site removed' passed"; exit 1; fi
# call placed before Present
python3 - "$V/rsx_live_draw.c" "$N/rsx_live_draw.c" <<'EOF'
import sys
s = open(sys.argv[1]).read()
a = s.index("    /* patch 2199: the first Present")
b = s.index("    return hr;", a)
blk = s[a:b]
s = s[:a] + s[b:]
k = s.index("    const HRESULT hr = g.swap->lpVtbl->Present(g.swap, interval")
open(sys.argv[2], "w").write(s[:k] + blk + s[k:])
EOF
if static_ok "$N/rsx_live_draw.c"; then echo "[FAIL] negative control 'call before Present' passed"; exit 1; fi
echo "[test_first_frame] negative controls ok (4 header/call-site breakages and a misplaced call all fail the test)"
