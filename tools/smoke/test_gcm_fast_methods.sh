#!/usr/bin/env bash
# Host test for KZ3 patch 2149 (GCM_FAST_METHODS): builds the toolkit's cellGcmSys.c with stubs, walks a synthetic ring
# (increment and non-increment headers, 1 MB IO page edges) with the switch off and on, and requires identical call
# streams (live draw, draw engine, legacy state) in three configurations. Also runs the toolkit's own libs/video/tests/
# test_gcm_* walker fixtures that need no extra stubs, unchanged, with the switch off and on. SKIP without patch 2149.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_gcm_fast_methods.sh
# KZ3_PATCHED_DIR may point at another toolkit tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/cellGcmSys.c" || ! -d "$PT/include" ]]; then
  echo "[test_gcm_fast_methods] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q gcm_fast_methods "$V/cellGcmSys.c"; then
  echo "[test_gcm_fast_methods] SKIP: toolkit without patch 2149"; exit 0; fi
O="$ROOT/build/smoke-host/gcm_fast_methods"; mkdir -p "$O"
CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -lm)
gcc "${CF[@]}" "$ROOT/tools/smoke/test_gcm_fast_methods.c" "$ROOT/tools/smoke/test_gcm_fast_methods_stubs.c" -o "$O/t"
rc=0
for cfg in "vtable live" "novtable live" "vtable nolive"; do
  set -- $cfg
  a="$("$O/t" slow "$1" "$2" | tail -1)"; b="$("$O/t" fast "$1" "$2" | tail -1)"
  echo "$a"; echo "$b"
  [[ "${a##*digest }" == "${b##*digest }" && "$a" == *"walked all"* ]] || { echo "[test_gcm_fast_methods] FAIL ($cfg): streams differ"; rc=1; }
done
# the toolkit's own walker fixtures: unchanged, switch off and on
for t in test_gcm_legacy_state_filter test_gcm_flip_stream test_gcm_nop_runs test_gcm_get_on_jump; do
  src="$V/tests/$t.c"; [[ -f "$src" ]] || continue
  if gcc "${CF[@]}" "$src" -o "$O/$t" 2>"$O/$t.log"; then
    for sw in 0 1; do
      if GCM_FAST_METHODS=$sw "$O/$t" >"$O/$t.out" 2>&1; then echo "[fixture] $t switch=$sw PASS"; else echo "[fixture] $t switch=$sw FAIL"; rc=1; fi
    done
  else echo "[fixture] $t SKIP (does not build standalone)"; fi
done
[[ $rc -eq 0 ]] && echo "[test_gcm_fast_methods] PASS" || { echo "[test_gcm_fast_methods] FAIL"; exit 1; }
