#!/usr/bin/env bash
# Host test for KZ3 patch 2188 (command-thread overhaul O0): the exclusive split core, the [perf-split] line format and bucket
# sum, the O0 counters (libs/video/rsx_o0.h), plus static checks that the patched sources read the switches and keep the
# shared line's keys. Also builds and runs the wake-latency microbenchmark (numbers only, nothing asserted).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_perf_split.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_o0.h" ]]; then echo "[test_perf_split] SKIP: no patched toolkit with patch 2188 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
O="$ROOT/build/smoke-host/perf-split"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_perf_split.c" -o "$O/t" -lm
"$O/t"
if [[ "$(ulimit -v)" == unlimited ]] && echo 'int main(void){return 0;}' | $CC -fsanitize=address,undefined -x c - -o "$O/asan_probe" 2>/dev/null; then
  $CC -std=c11 -g -O1 -fsanitize=address,undefined -I"$V" "$ROOT/tools/smoke/test_perf_split.c" -o "$O/t_asan" -lm
  "$O/t_asan" | sed 's/^/[asan] /'
else echo "[test_perf_split] SKIP asan: no sanitizers, or ulimit -v is set (ASan needs its shadow mapping)"; fi
G="$V/cellGcmSys.c"; L="$V/rsx_live_draw.c"
for k in 'gcm_env_on("RSX_PERF_SPLIT")' 'gcm_env_on("RSX_O0")'; do grep -qF "$k" "$G" || { echo "[FAIL] switch not read: $k"; exit 1; }; done
grep -qF '"[%s] t=%.0f flips=%lld frame=%.2f walk=%.2f decode=%.2f tex=%.2f pso=%.2f desc=%.2f vplan=%.2f fill=%.2f rec=%.2f"' "$V/rsx_o0.h" \
  && grep -qF '" wb=%.2f rbwait=%.2f submit=%.2f park=%.2f other=%.2f gpu=%.2f"' "$V/rsx_o0.h" || { echo "[FAIL] shared line keys changed"; exit 1; }
n=$(grep -c 'LDS_PUSH' "$L"); m=$(grep -c 'LDS_POP' "$L")
[[ "$n" -gt 10 && "$m" -ge "$n" ]] || { echo "[FAIL] LDS_PUSH ($n) / LDS_POP ($m): every push needs a pop"; exit 1; }
echo "[test_perf_split] static checks ok (LDS push/pop sites: $n)"
$CC -std=gnu11 -O2 "$ROOT/tools/smoke/bench_wake_latency.c" -o "$O/bench" -lpthread && timeout 120 "$O/bench"
