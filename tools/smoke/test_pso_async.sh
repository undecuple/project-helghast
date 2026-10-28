#!/usr/bin/env bash
# Host test for KZ3 patch 2048 (asynchronous PSO builds: pending entries, merge, one in-flight compile per shader
# blob): builds tools/smoke/test_pso_async.c against the patched toolkit's libs/video/rsx_pso_async.h and runs it.
# No game data, no GPU.   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pso_async.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_pso_async.h" ]]; then
  echo "[test_pso_async] SKIP: no patched toolkit with patch 2048 ($V/rsx_pso_async.h; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/pso_async"; mkdir -p "$O"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -pthread -I"$V" "$ROOT/tools/smoke/test_pso_async.c" \
  -o "$O/test_pso_async"
"$O/test_pso_async"
