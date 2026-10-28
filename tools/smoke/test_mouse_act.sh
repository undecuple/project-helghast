#!/usr/bin/env bash
# Host test for KZ3 patch 4020 (mouse activity -> button prompts): tools/smoke/test_mouse_act.c against the patched
# toolkit's libs/input/pad_mouse.h + pad_host.h. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_mouse_act.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
I="$PT/libs/input"
if ! grep -q PadMouseAct "$I/pad_mouse.h" 2>/dev/null; then echo "[test_mouse_act] SKIP: toolkit without patch 4020"; exit 0; fi
O="$ROOT/build/smoke-host/mouse_act"; mkdir -p "$O"
cc -std=c11 -O1 -Wall -Wextra -Wno-unused-function -I"$I" -I"$PT/include" -I"$PT" "$ROOT/tools/smoke/test_mouse_act.c" "$I/pad_mouse.c" -lm -lpthread -o "$O/t"
"$O/t"
