#!/usr/bin/env bash
# Host test for KZ3 patch 2187 (stutter Task 0): the whole-run present QPC stream and the vblank/flip timing window
# (libs/video/rsx_present_stream.h, rsx_stutter_diag.h), plus static checks that the patched sources keep the CSV
# column lines byte for byte and read the switches. No game data. Runs the C test plain and under ThreadSanitizer
# (SKIP of the TSan part when the compiler lacks it).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_stutter_diag.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_present_stream.h" ]]; then echo "[test_stutter_diag] SKIP: no patched toolkit with patch 2187 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
O="$ROOT/build/smoke-host/stutter-diag"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_stutter_diag.c" -o "$O/t" -lpthread
(cd "$O" && TMPDIR_TEST="$O" ./t) | grep -v "^\[PASS\]"
if echo 'int main(void){return 0;}' | $CC -fsanitize=thread -x c - -o "$O/tsan_probe" 2>/dev/null; then
  $CC -std=c11 -g -O1 -fsanitize=thread -I"$V" "$ROOT/tools/smoke/test_stutter_diag.c" -o "$O/t_tsan" -lpthread
  # fork + TSan is fine here: the child only writes a file and kills itself
  (cd "$O" && TMPDIR_TEST="$O" TSAN_OPTIONS=halt_on_error=1 ./t_tsan) | grep -v "^\[PASS\]" | sed 's/^/[tsan] /'
else
  echo "[test_stutter_diag] SKIP tsan: compiler has no -fsanitize=thread"
fi
# static checks on the patched toolkit: the non-stream dump still writes the unchanged columns; switches are read
L="$V/rsx_live_draw.c"; G="$V/cellGcmSys.c"
grep -qF 'fprintf(f, "present_id,guest_frame,qpc\n");' "$L" && grep -qF '"present_id,guest_frame,qpc,process_kernel_100ns,"' "$L" || { echo "[FAIL] shutdown-dump column lines changed"; exit 1; }
grep -qF 'getenv("RSX_PRESENT_QPC_STREAM")' "$L" && grep -qF 'getenv("RSX_ATTRIB")' "$L" && grep -qF 'getenv("RSX_ATTRIB")' "$G" || { echo "[FAIL] switch not read"; exit 1; }
echo "[test_stutter_diag] static checks ok"
