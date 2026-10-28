#!/usr/bin/env bash
# Host test for KZ3 patch 2075 (texture cache recycling, rsx_tex_lru.h): builds tools/smoke/test_tex_lru.c against the
# patched toolkit and runs it (list victim == old serial scan under random workloads; K4-like ring of 8x8 blocks:
# steady state creates no resources, textures bound every frame never evicted). No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_tex_lru.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$PT/libs/video/rsx_tex_lru.h"
if [[ ! -f "$H" ]]; then echo "[test_tex_lru] SKIP: no patched toolkit ($H; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/tex_lru"; mkdir -p "$O"
CC="${CC:-cc}"
$CC -std=c11 -Wall -Wextra -Werror -O2 -I"$PT/libs/video" "$ROOT/tools/smoke/test_tex_lru.c" -o "$O/test_tex_lru"
"$O/test_tex_lru"
