#!/usr/bin/env bash
# Pass 91: upstream PR #375 (9375, draw-engine sparse texture re-hash; the engine is not used by KZ3, carried for
# parity) on the patched toolkit (tools/ps3recomp-patched.sh): the PR's test_rsx_draw_engine must pass with the patch,
# and fail with the engine source put back (patch reversed for rsx_draw_engine.c only, new test kept). Exit 1 on any
# unexpected result. Source list as in upstream's linux.yml.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${PS3RECOMP_PATCHED:-$ROOT/work/ps3recomp-patched}"
PATCH="$ROOT/port/patches/9375-upstream-pr375-draw-engine-sparse-texture-rehash.patch"
[[ -f $T/libs/video/rsx_draw_engine.c && -f $PATCH ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q eng_texture_sparse_hash "$T/libs/video/rsx_draw_engine.c" || { echo "SKIP: patched toolkit lacks 9375 (run tools/ps3recomp-patched.sh)"; exit 0; }
OUT="$(mktemp -d)"
fail=0
SRC="libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c
 libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c"
build() { # name source-tree
  (cd "$2" && ${CC:-cc} -std=gnu17 -w -I include -I libs/video -o "$OUT/$1" \
     "$T/libs/video/tests/test_rsx_draw_engine.c" $SRC -lm -lpthread) >"$OUT/$1.log" 2>&1; }
if ! build new "$T"; then echo "FAIL build: $(grep -m3 -E 'undefined|error' "$OUT/new.log" | cut -c1-200)"; exit 1; fi
if "$OUT/new" >"$OUT/new.run" 2>&1; then echo "PASS test_rsx_draw_engine with 9375: $(grep -c PASS "$OUT/new.run") checks"
else echo "FAIL test_rsx_draw_engine with 9375: $(grep -m3 FAIL "$OUT/new.run" | cut -c1-160)"; fail=1; fi
# before: the engine source without the patch
# (inside the repo: git apply skips --directory targets outside it, as in tools/ps3recomp-patched.sh)
OLDREL="build/r91-pr375-old"; OLD="$ROOT/$OLDREL"; "$ROOT/tools/safe_rm.sh" "$OLD" 2>/dev/null; mkdir -p "$OLD"
cp -r "$T/include" "$T/libs" "$OLD/"
if ! (cd "$ROOT" && git apply -R --verbose --include="$OLDREL/libs/video/rsx_draw_engine.c" --directory="$OLDREL" "$PATCH") >"$OUT/rev.log" 2>&1 \
   || ! grep -q "^Applied patch .*rsx_draw_engine.c cleanly" "$OUT/rev.log"; then
  echo "FAIL reverse apply: $(head -2 "$OUT/rev.log" | cut -c1-200)"; exit 1; fi
if ! build old "$OLD"; then echo "FAIL old build: $(head -3 "$OUT/old.log" | cut -c1-200)"; fail=1
elif "$OUT/old" >"$OUT/old.run" 2>&1; then echo "FAIL: the test passes without the engine change (shows nothing)"; fail=1
else echo "PASS before/after: old engine fails ($(grep -m1 FAIL "$OUT/old.run" | cut -c1-110))"; fi
exit $fail
