#!/usr/bin/env bash
# Host test for KZ3 patch 2161 (sampler cache hash index): equivalence with the linear scan, negative control,
# micro-benchmark. Needs only the patched toolkit's rsx_smp_index.h.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_smp_index.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_smp_index.h" ]]; then echo "[test_smp_index] SKIP: toolkit without patch 2161"; exit 0; fi
O="$ROOT/build/smoke-host/smp_index"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_smp_index.c" -o "$O/test_smp_index"
"$O/test_smp_index"
