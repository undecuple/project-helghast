#!/usr/bin/env bash
# Negative controls for patch 2206 (Options > Graphics): each mutant breaks one rule in a COPY of the patched toolkit's
# rsx_display_mode.h / rsx_gfx_page.h, and tools/smoke/test_display_mode.c must then FAIL (a failing [FAIL] check, not a compile
# error). The unmutated copy must pass first. No game data, no GPU.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_gfx_mutants.sh'
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_gfx_page.h" ]]; then echo "[test_gfx_mutants] SKIP: no patched toolkit with patch 2206 ($V/rsx_gfx_page.h; run tools/ps3recomp-patched.sh)"; exit 0; fi
CC="${CC:-cc}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
W=(-std=c11 -Wall -Wextra -O1)

# name | file | old text (exactly once) | new text
python3 - "$T" <<'PY'
import sys, json
t = sys.argv[1]
M = [
 ("lock-off", "rsx_gfx_page.h", "return game_type >= 1 && game_type <= 3;", "return game_type > 99;"),
 ("lock-only-type-1", "rsx_gfx_page.h", "return game_type >= 1 && game_type <= 3;", "return game_type == 1;"),
 ("lock-counts-no-session", "rsx_gfx_page.h", "return game_type >= 1 && game_type <= 3;", "return game_type != 0;"),
 ("no-custom-state", "rsx_gfx_page.h", "    return RSX_GFX_CUSTOM;\n}\n/* Load:", "    return RSX_GFX_HIGH;\n}\n/* Load:"),
 ("ultra-always-offered", "rsx_gfx_page.h", "static inline int rsx_gfx_ultra_offered(unsigned levers) { return rsx_gfx_ultra_levers(levers) != 0; }",
  "static inline int rsx_gfx_ultra_offered(unsigned levers) { (void)levers; return 1; }"),
 ("unknown-quality-reads-ultra", "rsx_display_mode.h", 'rsx_dm_ieq(s, n, "custom") ? RSX_GFX_CUSTOM : -1;', 'rsx_dm_ieq(s, n, "custom") ? RSX_GFX_CUSTOM : RSX_GFX_ULTRA;'),
 ("load-trusts-file", "rsx_gfx_page.h", "if ((want == RSX_GFX_HIGH || want == RSX_GFX_ULTRA) && want != actual) return RSX_GFX_CUSTOM;", "(void)want;"),
 ("high-is-not-today", "rsx_gfx_page.h", '{ RSX_GFX_L_BLOOM, "bloom", 100, 100, 0,', '{ RSX_GFX_L_BLOOM, "bloom", 90, 100, 0,'),
 ("ultra-lever-equals-high", "rsx_gfx_page.h", '{ RSX_GFX_L_RES, "res", 100, 150, 1,', '{ RSX_GFX_L_RES, "res", 100, 100, 1,'),
 ("quality-not-written", "rsx_display_mode.h", '"quality = %s\\n", rsx_gfx_quality_name(quality));', '"quality = %s\\n", rsx_gfx_quality_name(quality & 0));'),
 ("guard-missing-in-onpageon", "rsx_gfx_page.h", '"\\t\\tfunction OnPageOn()\\n"\n        "\\t\\t\\tgKz3G = 0\\n"', '"\\t\\tfunction OnPageOn()\\n"'),
 ("refresh-without-guard", "rsx_gfx_page.h", '"\\t\\tfunction KZ3Refresh()\\n"\n        "\\t\\t\\tgKz3G = 0\\n"', '"\\t\\tfunction KZ3Refresh()\\n"'),
 ("reset-forgets-graphics", "rsx_display_mode.h", '        "\\t\\t\\tKZ3DisplayCmd(\\"gfx reset\\")\\n"', ''),
 ("preset-ignores-lock", "rsx_gfx_page.h", "if (*a && !locked) {\n            const int q = rsx_gfx_quality_parse(a);", "if (*a) {\n            const int q = rsx_gfx_quality_parse(a);"),
 ("lever-ignores-lock", "rsx_gfx_page.h", "if (*a && !locked && (st->levers & l->bit)) {", "if (*a && (st->levers & l->bit)) {"),
 ("custom-pick-writes", "rsx_gfx_page.h", "if (q == RSX_GFX_HIGH || (q == RSX_GFX_ULTRA && rsx_gfx_ultra_offered(st->levers))) rsx_gfx_apply(st, q);",
  "if (q >= 0) rsx_gfx_apply(st, q == RSX_GFX_CUSTOM ? RSX_GFX_ULTRA : q);"),
 ("motion-blur-row", "rsx_gfx_page.h", '        "\\t\\t</list>\\n\\n"', '        "\\t\\t\\t<select id=\\"slc_kz3_mblur\\" label=\\"Motion blur\\"></select>\\n\\t\\t</list>\\n\\n"'),
 ("status-line-dropped", "rsx_gfx_page.h", 'static inline const char* rsx_gfx_when_marker(int when)\n{', 'static inline const char* rsx_gfx_when_marker(int when)\n{\n    return "";'),
 ("bloom-row-back-on-display", "rsx_display_mode.h", '        "\\t\\t\\tKZ3DisplayCmd(\\"gfx reset\\")\\n"', '        "\\t\\t\\tif (KZ3BloomOn) then KZ3DisplayCmd(\\"bloom 100\\") end\\n"'),
 ("legend-always", "rsx_gfx_page.h", 'rsx_gfx_put(&b, locked ? ">\\n\\n"', 'rsx_gfx_put(&b, (locked & 0) ? ">\\n\\n"'),
 ("bloom-reply-raw", "rsx_gfx_page.h", "i == 0 ? rsx_dm_pct_round(st->v.v[i]) : st->v.v[i]", "st->v.v[i]"),
]
json.dump(M, open(t + "/mutants.json", "w"))
PY

run() {   # dir -> output; exit status of the test binary
  "$CC" "${W[@]}" -Werror -I"$1" "$ROOT/tools/smoke/test_display_mode.c" -o "$1/t" 2>"$1/cc.err" || { echo "COMPILE-ERROR"; cat "$1/cc.err"; return 3; }
  "$1/t" || true
}

mkdir -p "$T/base"; cp "$V"/*.h "$T/base/"
base_out="$("$CC" "${W[@]}" -Werror -I"$T/base" "$ROOT/tools/smoke/test_display_mode.c" -o "$T/base/t" && "$T/base/t" | tail -1)"
[[ "$base_out" == "PASS: 0 failure(s)" ]] || { echo "[FAIL] the unmutated test does not pass: $base_out"; exit 1; }
echo "[PASS] unmutated: $base_out"

n=$(python3 -c "import json,sys; print(len(json.load(open('$T/mutants.json'))))")
bad=0
for ((i = 0; i < n; i++)); do
  d="$T/m$i"; mkdir -p "$d"; cp "$V"/*.h "$d/"
  name="$(python3 - "$T" "$i" "$d" <<'PY'
import sys, json
t, i, d = sys.argv[1], int(sys.argv[2]), sys.argv[3]
name, f, old, new = json.load(open(t + "/mutants.json"))[i]
s = open(f"{d}/{f}").read()
if s.count(old) != 1:
    print("BAD-MUTANT " + name + f" (old text found {s.count(old)} times)")
    sys.exit(0)
open(f"{d}/{f}", "w").write(s.replace(old, new))
print(name)
PY
)"
  if [[ "$name" == BAD-MUTANT* ]]; then echo "[FAIL] $name"; bad=1; continue; fi
  out="$("$CC" "${W[@]}" -Werror -I"$d" "$ROOT/tools/smoke/test_display_mode.c" -o "$d/t" 2>&1 && "$d/t" 2>&1 | grep -E "^\[FAIL\]|^FAIL" | head -2 || true)"
  if [[ "$out" == *"[FAIL]"* ]]; then echo "[PASS] mutant $name fails the test: $(echo "$out" | head -1 | cut -c1-110)"
  else echo "[FAIL] mutant $name was NOT caught: ${out:-(test passed)}"; bad=1; fi
done
[[ $bad == 0 ]] && echo "PASS: $n mutants all caught" || { echo "FAIL: a negative control did not fail"; exit 1; }
