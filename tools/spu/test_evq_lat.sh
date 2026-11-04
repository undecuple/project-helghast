#!/usr/bin/env bash
# Patch 1155 host test: the real-wait / wake-latency table behind KZ3_WAITSTAT=1 in sys_event_queue_receive. Builds
# tools/spu/test_evq_lat.c against runtime/platform/kz3_evq_lat.h of work/ps3recomp-patched (R145_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_evq_lat.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${R145_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/platform/kz3_evq_lat.h" ]] || { echo "SKIP: patch 1155 not in $T (run tools/ps3recomp-patched.sh)"; exit 0; }
gcc -std=gnu17 -O2 -Wall -pthread -I "$T/runtime/platform" -o "$O/t_evq_lat" "$ROOT/tools/spu/test_evq_lat.c" 2>&1 | grep -m5 "error" ; [[ -x "$O/t_evq_lat" ]] || { echo "BUILD FAIL"; exit 1; }
"$O/t_evq_lat" 2>&1 | grep -v '^\[evq-lat\]' ; "$O/t_evq_lat" 2>&1 | grep -c '^\[evq-lat\]' | sed 's/^/report rows printed: /'
