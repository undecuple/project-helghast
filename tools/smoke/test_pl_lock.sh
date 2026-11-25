#!/usr/bin/env bash
# Host test for KZ3 patch 2182: per-key pipeline-library lock (rsx_pl_lock.h) with real threads under
# -fsanitize=thread, plus the lock-off negative control (the fake library uses atomics only, so TSan stays quiet and
# the checker flags the violations). No game data, no GPU; SKIP without the patched toolkit or patch 2182.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pl_lock.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
[[ -d "$V" ]] || { echo "[test_pl_lock] SKIP: no patched toolkit ($V)"; exit 77; }
[[ -f "$V/rsx_pl_lock.h" ]] || { echo "[test_pl_lock] SKIP: toolkit without patch 2182"; exit 77; }
O="$ROOT/build/smoke-host/pl_lock"; mkdir -p "$O"
CCX="${CC:-cc}"
SAN=""
if echo 'int main(void){return 0;}' | $CCX -x c -fsanitize=thread -o "$O/tsan_probe" - 2>/dev/null && "$O/tsan_probe" 2>/dev/null; then SAN="-fsanitize=thread"; else echo "[test_pl_lock] note: TSan unavailable, running without it"; fi
$CCX -std=gnu11 -Wall -Wextra -Werror -O1 -g $SAN -I"$V" "$ROOT/tools/smoke/test_pl_lock.c" -o "$O/t" -lpthread
out="$("$O/t" 2>&1)" && rc=0 || rc=$?
echo "$out" | tail -12
[[ $rc -eq 0 ]] || { echo "[test_pl_lock] FAIL (rc $rc)"; exit 1; }
if echo "$out" | grep -q 'WARNING: ThreadSanitizer'; then echo "[test_pl_lock] FAIL: TSan report"; exit 1; fi
echo "[test_pl_lock] PASS ${SAN:+(tsan)}"
