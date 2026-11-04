#!/usr/bin/env bash
# Host test for KZ3 patch 2147 (RSX pass 137): the [perf-flush] ring-wrap segment. Builds the toolkit's cellGcmSys.c with the
# stubs of its own test_gcm_legacy_state_filter.c. No game data, no GPU; SKIP without the patched toolkit (or without 2147).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_gcm_r137.sh
# KZ3_PATCHED_DIR may point at another toolkit tree (it must hold libs/video/tests/test_gcm_legacy_state_filter.c).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT" ]] && PT="$(cd "$PT" && pwd)"
T="$PT/libs/video/tests/test_gcm_legacy_state_filter.c"
if [[ ! -f "$T" ]] || ! grep -q gcm_perf_flush_line "$PT/libs/video/cellGcmSys.c"; then
  echo "[test_gcm_r137] SKIP: toolkit without patch 2147 or without the legacy-state-filter test ($PT; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/gcm_r137"; mkdir -p "$O"
gcc -std=gnu17 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -I "$PT/include" -I "$PT/libs/video" -DLEGACY_TEST_C="\"$T\"" \
    "$ROOT/tools/smoke/test_gcm_r137.c" -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -lm -o "$O/t"
"$O/t" | tail -8
[[ ${PIPESTATUS[0]} -eq 0 ]] || { echo "[test_gcm_r137] FAIL"; exit 1; }
echo "[test_gcm_r137] PASS"
