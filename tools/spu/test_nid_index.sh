#!/usr/bin/env bash
# Patch 1055 host test: tools/spu/test_nid_index.c (hashed NID index vs the linear ps3_nid_table_find: every
# registered NID, 1e6 unknown probes, growth, a context-handler table, readers during growth, timing).
# Needs work/ps3recomp-patched (or NID_HDR=DIR holding kz3_nid_index.h); says SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_nid_index.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="$ROOT/work/ps3recomp-patched"; H="${NID_HDR:-$P/runtime/ppu}"
[[ -f "$H/kz3_nid_index.h" && -f "$P/include/ps3emu/nid.h" ]] || { echo "SKIP: no patched toolkit with patch 1055 at $H"; exit 0; }
gcc -std=gnu17 -O2 -Wall -pthread -I "$H" -I "$P/include" -o "$O/t_nid_index" "$ROOT/tools/spu/test_nid_index.c" 2>&1 | grep -m5 -E "error|warning"
"$O/t_nid_index"
