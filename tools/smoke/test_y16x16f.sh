#!/usr/bin/env bash
# Host test for KZ3 patches 2200/2201 (Y16_X16_FLOAT channel order: backend remap = ps3recomp PR #836, live-draw
# ld_float_tex_mapping sel_rg {0,1,0,1} + patch 2065's compute pass writing component 0 = Y). Extracts the real
# mapping function and the shader's write expression from the patched sources (no copy of the logic in the test),
# builds tools/smoke/test_y16x16f.c, then requires every negative control (an exact-text mutant of the sources:
# one half of the live-draw change reverted, both reverted, the backend line removed) to FAIL the named check.
# No game data, no GPU; SKIP without the patched toolkit or patch 2201.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_y16x16f.sh'     (gcc: CC unset)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT/libs/video" ]] || { echo "[test_y16x16f] SKIP: no patched toolkit ($PT)"; exit 0; }
grep -q "patch 2201" "$PT/libs/video/rsx_live_draw.c" 2>/dev/null || { echo "[test_y16x16f] SKIP: toolkit without patch 2201"; exit 0; }
O="$ROOT/build/smoke-host/y16x16f"; mkdir -p "$O"
CCX="${CC:-cc}"
SRC="$ROOT/tools/smoke/test_y16x16f.c"
# run NAME [MUTATION]: extract from (a mutated copy of) the patched sources, build, run; prints the run's output, returns its status
run() {
  local d="$O/$1"; rm -rf "$d"; mkdir -p "$d"
  python3 -I - "$PT" "$d" "${2:-}" <<'PY' || return 99
import re, sys, shutil
pt, d, mut = sys.argv[1:]
live = open(pt + '/libs/video/rsx_live_draw.c').read()
lay = open(pt + '/libs/video/rsx_texture_layout.c').read()
def sub(s, a, b):
    assert s.count(a) == 1, (mut, a)
    return s.replace(a, b)
OLD_SEL = "{ 0, 1, 0, 1 };"; NEW_SEL_OLD = "{ 1, 0, 1, 0 };"
SH_NEW = "float2(f16tof32((b.w << 8) | b.x), f16tof32((b.y << 8) | b.z))"
SH_OLD = "float2(f16tof32((b.y << 8) | b.z), f16tof32((b.w << 8) | b.x))"
if mut in ('SEL_ONLY', 'BOTH_OLD'):    # sel_rg back to the old order
    live = sub(live, "static const u32 sel_rg[4]   = " + OLD_SEL, "static const u32 sel_rg[4]   = " + NEW_SEL_OLD)
if mut in ('SHADER_ONLY', 'BOTH_OLD'): # the pass back to component 0 = X
    live = sub(live, SH_NEW, SH_OLD)
if mut == 'NO_BACKEND_LINE':
    lay = sub(lay, "(base == 0x9Fu) ? lanes_y16x16f", "(base == 0x9Fu) ? lanes_argb + 0 * sizeof(lanes_y16x16f)")
i = live.index("static u32 ld_float_tex_mapping(u32 base_fmt, u32 remap)\n{")
k = live.index('{', i) + 1; depth = 1
while depth:
    depth += (live[k] == '{') - (live[k] == '}'); k += 1
func = live[i:k]
m = re.search(r"float2\(f16tof32\(\(b\.(\w) << 8\) \| b\.(\w)\), f16tof32\(\(b\.(\w) << 8\) \| b\.(\w)\)\)", live)
assert m, 'surf-rg16 write expression not found'
idx = {'x': 0, 'y': 1, 'z': 2, 'w': 3}
assert "d[k] = px[k + 1]; d[k + 1] = px[k];" in live, 'guest float decode byte swap not found'
dis = open(pt + '/libs/video/rsx_dispatch.h').read()
fmt = re.search(r"#define RSX_TEX_FMT_Y16X16_FLOAT\s+(0x[0-9A-Fa-f]+)", dis).group(1)
with open(d + '/y16x16f_extract.h', 'w') as f:
    f.write("typedef unsigned int u32;\n#define RSX_TEX_FMT_Y16X16_FLOAT %s\n" % fmt)
    f.write("#define SH_C0_HI %d\n#define SH_C0_LO %d\n#define SH_C1_HI %d\n#define SH_C1_LO %d\n" % tuple(idx[g] for g in m.groups()))
    f.write(func + "\n")
open(d + '/rsx_texture_layout.c', 'w').write(lay)
PY
  [[ $? -eq 0 ]] || return 99
  local fl=(-std=gnu11 -Wall -Wextra -Werror -O2 "-I$d" "-I$PT/libs/video" "-I$PT/include")
  $CCX "${fl[@]}" "$SRC" "$d/rsx_texture_layout.c" -o "$d/t" 2>"$d/build.log" || { echo "[test_y16x16f] build failed ($1)"; head -12 "$d/build.log"; return 98; }
  "$d/t"
}
fails=0
out="$(run base)"; rc=$?; echo "$out" | tail -3
[[ $rc -eq 0 ]] || { echo "[test_y16x16f] FAIL: unmutated sources"; exit 1; }
# control NAME 'check names that must fail' ['check names that must still pass']
control() {
  local out rc; out="$(run "$1" "$1")"; rc=$?
  if [[ $rc -eq 0 || $rc -ge 98 ]]; then echo "[test_y16x16f] FAIL: control $1 not caught (rc=$rc)"; echo "$out" | head -5; fails=$((fails+1)); return; fi
  local c
  for c in $2; do grep -q "FAIL $c " <<<"$out" || { echo "[test_y16x16f] FAIL: control $1 did not fail check $c"; echo "$out" | head -8; fails=$((fails+1)); return; }; done
  for c in ${3:-}; do grep -q "FAIL $c " <<<"$out" && { echo "[test_y16x16f] FAIL: control $1 failed $c"; fails=$((fails+1)); return; }; done
  echo "[test_y16x16f] control $1: caught ($(grep -c '^FAIL' <<<"$out") failing check(s): $(grep -o '^FAIL [a-zA-Z0-9-]*' <<<"$out" | cut -d' ' -f2 | tr '\n' ' '))"
}
control SEL_ONLY "gbuffer-R-is-X gbuffer-G-is-Y gbuffer-net-unchanged-vs-old-pair guest-tex-R-is-X backend-equals-live-draw-mapping"
control SHADER_ONLY "gbuffer-R-is-X gbuffer-G-is-Y gbuffer-net-unchanged-vs-old-pair surface-path-equals-texture-path"
# the old pair keeps the G-buffer result (net unchanged) but is wrong for a true guest texture and disagrees with the backend
control BOTH_OLD "guest-tex-R-is-X guest-tex-G-is-Y backend-equals-live-draw-mapping" "gbuffer-R-is-X gbuffer-G-is-Y gbuffer-net-unchanged-vs-old-pair"
control NO_BACKEND_LINE "backend-identity-0101 backend-equals-live-draw-mapping"
[[ $fails -eq 0 ]] && echo "[test_y16x16f] PASS" || { echo "[test_y16x16f] $fails FAILURE(S)"; exit 1; }
