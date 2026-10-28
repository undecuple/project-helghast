#!/usr/bin/env bash
# Host test for KZ3 patches 2092/2093 (device-resident vertex cache, rsx_vtx_dev.h): builds tools/smoke/test_vtx_dev.c against
# the patched toolkit and runs it (arena with fence-deferred frees, tracked-write and full-compare hit rule). No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vtx_dev.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_vtx_dev.h"
if [[ ! -f "$H" ]]; then echo "[test_vtx_dev] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/vtx_dev"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 ${KZ3_SAN:+-fsanitize=address,undefined -fno-omit-frame-pointer} \
  -I"$PT/libs/video" "$ROOT/tools/smoke/test_vtx_dev.c" -o "$O/test_vtx_dev"
(cd "$O" && ./test_vtx_dev) | grep -v "^\[PASS\]"
