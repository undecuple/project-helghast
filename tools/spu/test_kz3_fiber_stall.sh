#!/usr/bin/env bash
# Host test (SPU pass 34): port/spu/kz3_fiber_stall.c, the fiber stall breaker's classification (DeferredSchedule
# arena waits by static slot or wait-site lr, both builds) and its decision sequence (report, re-kick at one
# KZ3_FIBER_DEADLOCK period, completion write at two). Self-contained: no game data, no work/, no runtime.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_kz3_fiber_stall.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
gcc -std=gnu11 -Wall -Wextra -Werror -O1 -I "$ROOT/port/spu" "$ROOT/tools/spu/test_kz3_fiber_stall.c" \
    "$ROOT/port/spu/kz3_fiber_stall.c" -o "$T/t"
"$T/t"
