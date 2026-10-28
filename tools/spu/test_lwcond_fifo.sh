#!/usr/bin/env bash
# Patch 1059 host test: tools/spu/test_lwcond_fifo.c builds runtime/ppu/kz3_lwcond_fifo.h (the sys_lwcond per-waiter
# FIFO) with pthreads and reproduces the signal_all lost wakeup on the old counting-semaphore algorithm (negative
# control) while the FIFO wakes every waiter; plus signal/signal_to order and a timeout. Needs work/ps3recomp-patched
# (or LWC_HDR=DIR with the header).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_lwcond_fifo.sh [ITERS]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="$ROOT/work/ps3recomp-patched"; H="${LWC_HDR:-$P/runtime/ppu}"
[[ -f "$H/kz3_lwcond_fifo.h" ]] || { echo "SKIP: no kz3_lwcond_fifo.h at $H (patch 1059 not applied / no patched toolkit)"; exit 0; }
gcc -std=gnu17 -O2 -w -pthread -I "$H" -o "$O/t_lwcond_fifo" "$ROOT/tools/spu/test_lwcond_fifo.c" 2>&1 | grep -m5 error
"$O/t_lwcond_fifo" "$@"
