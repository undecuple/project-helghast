#!/usr/bin/env bash
# Host test for KZ3 patch 2211 (fence waits by site, [perf-split-w]): sum == walk, gap 0, negative controls, static checks on the patch file.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_wait_split.sh'   (cc is the default)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if ! grep -qs rsx_ws_fmt "$V/rsx_wait_split.h"; then echo "[test_wait_split] SKIP: no patched toolkit with patch 2211 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
CC="${CC:-cc}"
O="$ROOT/build/smoke-host/wait-split-$(basename "$CC")"; mkdir -p "$O"
$CC -std=gnu11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_wait_split.c" -o "$O/t" -lm
"$O/t"
negctl() {
    if "$O/t" "$1" >"$O/neg.txt" 2>&1; then echo "[FAIL] negative control $1 passed"; cat "$O/neg.txt"; exit 1; fi
    grep -q "$2" "$O/neg.txt" || { echo "[FAIL] negative control $1 failed for the wrong reason (want '$2')"; cat "$O/neg.txt"; exit 1; }
    echo "[test_wait_split] negative control $1 fails as required"
}
negctl --negctl-gap 'a fence wait outside the wait buckets'
negctl --negctl-window 'do not add up to the walk'
negctl --negctl-site 'wbdrain site missing or wrong'
# static checks on the PATCH FILE (the patched tree can lag): every ld_wait_fence caller goes through the per-site wrapper
P="$ROOT/port/patches/2211-perf-split-wait-sites.patch"
[ -f "$P" ] || { echo "[FAIL] $P missing"; exit 1; }
for s in 'ld_wait_fence_s(0 /*WBD_DRAIN*/' 'ld_wait_fence_s(1 /*PRESENT_SLOT*/' 'ld_wait_fence_s(2 /*RSX_WS_RING*/' 'ld_wait_fence_s(3 /*IDLE*/' 'ld_wait_fence_s(4 /*RESIZE*/' 'ld_wait_fence_s(5 /*VERIFY*/' 'RSX_PERF_WS_T0 + (reason == LD_FLUSH_READBACK ? 6 : 7)' 'rsx_ws_fmt(ln'; do
    grep -qF "$s" "$P" || { echo "[FAIL] patch 2211 lost: $s"; exit 1; }
done
echo "[test_wait_split] static checks ok"
