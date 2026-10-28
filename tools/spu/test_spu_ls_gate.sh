#!/usr/bin/env bash
# Patch 1100 host test (tools/spu/test_spu_ls_gate.c): LS gates and the WWS range probes. Self-contained.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_ls_gate.sh   (KZ3_PATCHED_DIR = another patched tree)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$P/runtime/spu/spu_context.h" ]]; then echo "SKIP: no patched tree at $P"; exit 0; fi
gcc -std=gnu17 -O2 -msse4.1 -w -fno-strict-aliasing -I "$P/runtime/spu" -I "$P/include" -I "$P/runtime" \
    -o "$O/t_ls_gate" "$ROOT/tools/spu/test_spu_ls_gate.c" -lm 2>&1 | grep -m5 -i "error\|undefined"
"$O/t_ls_gate"
