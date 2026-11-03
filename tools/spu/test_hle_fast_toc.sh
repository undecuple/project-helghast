#!/usr/bin/env bash
# Patch 1145 host test: the HLE fast path stores the caller's TOC in 0x28(r1) (J-Stars port patch 0004). Builds
# runtime/ppu/ppu_hle.cpp from work/ps3recomp-patched (or HT_TREE=DIR) with stubs; SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_hle_fast_toc.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/toc-test"; mkdir -p "$O"
T="${HT_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/ppu/ppu_hle.cpp" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
rm -f "$O/t_hle_fast_toc"
(cd "$T" && g++ -std=gnu++17 -O1 -w -I "$ROOT/tools/spu/toc_fast_stub" -I include -I runtime -I runtime/ppu -I runtime/platform -I runtime/diag \
   "$ROOT/tools/spu/test_hle_fast_toc.cpp" runtime/ppu/ppu_hle.cpp -pthread -o "$O/t_hle_fast_toc" 2>&1 | grep -m8 "error\|undefined")
[[ -x "$O/t_hle_fast_toc" ]] || { echo "BUILD FAIL"; exit 1; }
"$O/t_hle_fast_toc"
