#!/usr/bin/env bash
# Pass 88: build and run the upstream PR tests carried as 9NNN patches (single TU, the way their headers or the
# PRs' workflow lines build them) on the patched toolkit (work/ps3recomp-patched, tools/ps3recomp-patched.sh).
# test_avconf_gamma (#355 on our 2070) checks that the f1 gamma reaches cellAvconfExt_display_gamma(), the value
# rsx_live_draw.c's ld_gamma_present uses as its scan-out exponent. Exit 1 if any test fails.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${PS3RECOMP_PATCHED:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/libs ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"   # under the sandbox's private /tmp
fail=0
# Open: test_gcm_flip_status (#354) and test_dialog_callbacks (#360) do not link as one TU (they need runtime stubs).
for t in libs/misc/tests/test_avconf_gamma.c libs/system/tests/test_game_category.c; do
  n=$(basename "$t" .c)
  if ! (cd "$T" && ${CC:-cc} -std=gnu17 -w -I include -o "$OUT/$n" "$t" -lm -lpthread) >"$OUT/$n.log" 2>&1; then
    echo "FAIL $n (build): $(grep -m1 error "$OUT/$n.log" | cut -c1-200)"; fail=1; continue; fi
  if "$OUT/$n" >"$OUT/$n.run" 2>&1; then echo "PASS $n"; else echo "FAIL $n: $(tail -1 "$OUT/$n.run" | cut -c1-200)"; fail=1; fi
done
exit $fail
