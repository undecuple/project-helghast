#!/usr/bin/env bash
# Host test (SPU pass 33): port/spu/kz3_gameplay.c and the override macros of kz3_gameplay.h (gameplay-test-plan
# B4 trace, B7 SP unlock, B8 Motion Sensor default). Self-contained: a stub ppu_context (the macros read gpr[3]/[4]
# only), a 1 MB guest buffer as vm_base, stub func_<ADDR>__lifted bodies that store what the game's instructions
# store. Checks: KZ3_* env parsing; the SP-unlock byte written only with the switch on, logged once; the Motion
# Sensor filters (no gyro: 1 -> 0, 0 stays, PADTYPE restores the pre-call byte); the trace limiter (first call,
# then one line per KZ3_TRACE_PERIOD, none in between) and its line format; the inert path: with both switches off
# the hooks call the lifted body and write no byte and no line. No game data, no work/.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_kz3_gameplay.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cp "$ROOT/tools/spu/test_kz3_gameplay.c" "$T/main.c"
gcc -std=gnu11 -Wall -Wextra -O1 -I "$ROOT/port/spu" "$T/main.c" "$ROOT/port/spu/kz3_gameplay.c" -o "$T/t"
env -u KZ3_SP_UNLOCK -u KZ3_GAMEPLAY_TRACE "$T/t" "$T/err.txt"
