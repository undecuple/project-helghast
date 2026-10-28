#!/usr/bin/env bash
# Pass 90: build and run the PR #367 test (rsx draw engine, FP16 colour targets in 2D transfers) and the toolkit's own
# test_spu_helpers (SPU helper semantics, upstream's) on the patched toolkit (work/ps3recomp-patched).
# #369/#370 are not carried (pass 90), so test_spu_helpers here is the pre-#370 file.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_r90_pr_tests.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${PS3RECOMP_PATCHED:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/libs ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; fail=0
V=libs/video
if (cd "$T" && ${CC:-cc} -std=gnu17 -w -I include -I $V -o "$OUT/t_rsx" $V/tests/test_rsx_draw_engine.c $V/rsx_draw_engine.c \
      $V/rsx_dispatch.c $V/rsx_vertex_compact.c $V/rsx_texture_layout.c $V/rsx_vp_decompiler.c $V/rsx_fp_decompiler.c -lm -lpthread) >"$OUT/rsx.log" 2>&1; then
  if "$OUT/t_rsx" >"$OUT/rsx.run" 2>&1; then echo "PASS test_rsx_draw_engine ($(tail -1 "$OUT/rsx.run" | cut -c1-120))"; else echo "FAIL test_rsx_draw_engine: $(tail -2 "$OUT/rsx.run" | cut -c1-200)"; fail=1; fi
else echo "FAIL test_rsx_draw_engine (build): $(grep -m2 -E 'error|undefined' "$OUT/rsx.log" | cut -c1-200)"; fail=1; fi
for isa in "-msse4.1" "-msse4.1 -mfma"; do
  if (cd "$T" && ${CC:-cc} -std=gnu17 -O2 $isa -w -ffp-contract=off -I include -I runtime/spu -o "$OUT/t_sh" runtime/spu/tests/test_spu_helpers.c "$ROOT/tools/spu/r90_stubs.c" -lm) >"$OUT/sh.log" 2>&1; then
    if "$OUT/t_sh" >"$OUT/sh.run" 2>&1; then echo "PASS test_spu_helpers [$isa] ($(tail -1 "$OUT/sh.run" | cut -c1-100))"; else echo "FAIL test_spu_helpers [$isa]: $(grep -m2 FAIL "$OUT/sh.run" | cut -c1-200)"; fail=1; fi
  else echo "FAIL test_spu_helpers [$isa] (build): $(grep -m2 -E 'error|undefined' "$OUT/sh.log" | cut -c1-200)"; fail=1; fi
done
exit $fail
