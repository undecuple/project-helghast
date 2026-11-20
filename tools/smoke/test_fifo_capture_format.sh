#!/usr/bin/env bash
# Host test for KZ3 patch 2164: round trip of the FIFO capture format (libs/video/rsx_fifo_capture.h) with negative controls
# (flipped byte, dropped record, truncation, missing END, bad END counts, bad magic, newer version). Header only, no game data.
# SKIP without the patch.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fifo_capture_format.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fifo_capture.h" ]]; then echo "[test_fifo_capture_format] SKIP: toolkit without patch 2164 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/fifo_capture_format"; mkdir -p "$O"
gcc -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-function -I "$V" "$ROOT/tools/smoke/test_fifo_capture_format.c" -o "$O/t"
"$O/t" "$O"
