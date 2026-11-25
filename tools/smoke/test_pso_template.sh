#!/usr/bin/env bash
# Host test for KZ3 patches 2071, 2165, 2166 (pipeline template format, finder, step-8 image pass, merge tool): builds tools/smoke/test_pso_template.c
# against the patched toolkit's libs/video/rsx_pso_template.h and runs it with tools/smoke/pso_template.py.
# No game data, no GPU.   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pso_template.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_pso_template.h" ]]; then
  echo "[test_pso_template] SKIP: no patched toolkit with patch 2071 ($V/rsx_pso_template.h; run tools/ps3recomp-patched.sh)"; exit 77; fi
O="$ROOT/build/smoke-host/pso_template"; mkdir -p "$O"
${CC:-cc} -std=gnu11 -w -O2 -c -I"$V" -I"$PT/include" "$V/rsx_fp_decompiler.c" -o "$O/rsx_fp_decompiler.o"   # third-party code, own warnings not ours
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -I"$V" -I"$PT/include" "$ROOT/tools/smoke/test_pso_template.c" "$O/rsx_fp_decompiler.o" -o "$O/test_pso_template"
"$O/test_pso_template" "$O" "$ROOT/tools/smoke/pso_template.py"
