#!/usr/bin/env bash
# Compile the XeSS adapter (port/kz3/xess.cpp) and the two Windows XeSS test programs, and enforce the generic
# renderer boundary. No game data or GPU; nothing is linked or run (tools/smoke/test_xess.ps1 runs them by hand, with
# Intel's DLL and a D3D12 GPU).
#
# The Linux CI shell has no Windows-target compiler, so there the compile part SKIPs; CI's release-tools-windows job
# runs this with the release's own pinned llvm-mingw and KZ3_XESS_COMPILE_REQUIRED=1 (default unset: a missing
# patched tree or compiler is SKIP; set to 1: either one is FAIL, so a CI step can never pass by skipping).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/pyresolve.sh"
O="$ROOT/build/smoke-host/xess-compile"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
R="$PT/libs/video/rsx_live_draw.c"
H="$PT/libs/video/rsx_live_draw.h"
U="$PT/libs/video/rsx_upscale.h"
skip() {
  if [[ "${KZ3_XESS_COMPILE_REQUIRED:-}" == 1 ]]; then
    echo "[test_xess_compile] FAIL: $1 (KZ3_XESS_COMPILE_REQUIRED=1)" >&2
    exit 1
  fi
  echo "[test_xess_compile] SKIP: $1"
  exit 0
}
if [[ ! -f "$R" || ! -f "$H" || ! -f "$U" ]]; then
  skip "patched renderer not found ($PT)"
fi

$PY - "$R" "$H" <<'PY'
import sys

source = open(sys.argv[1], encoding="utf-8").read()
header = open(sys.argv[2], encoding="utf-8").read()
combined = source + header


def ordered(*needles):
    at = -1
    for needle in needles:
        at = source.find(needle, at + 1)
        if at < 0:
            return False
    return True


checks = (
    ("per-backend hook storage",
     "g_up_hooks[RSX_UPSCALE_BACKEND_COUNT]" in source
     and "g_up_hooks[hooks->backend] = *hooks;" in source),
    ("active backend hook lookup",
     "ld_up_active_hooks" in source
     and "g_up_hooks[backend].backend == backend" in source),
    ("draw inspection before biased sampler table construction",
     ordered("up_hooks->inspect_draw(", "ld_up_bias_samplers(",
             "sampler_table(smp_slots)")),
    ("generic signed mip-bias action",
     "int32_t mip_bias_fp;" in header
     and "ld_up_bias_samplers" in source
     and "mip_present" in source),
    ("generic display composition metadata",
     "int display_target;" in header
     and "int first_display_draw;" in header
     and "int depth_draw_seen;" in header),
    ("refreshed post-draw command-list handles",
     ordered("if (oq_pred >= 0 && up_capture_after_draw", "up_draw.command_list = g.list;",
             "up_hooks->after_draw(")),
    ("no KZ3 or XeSS implementation in generic renderer",
     "KZ3_XESS" not in combined and "kz3_xess" not in combined),
)
bad = [name for name, ok in checks if not ok]
if bad:
    print("[test_xess_compile] FAIL: " + ", ".join(bad), file=sys.stderr)
    raise SystemExit(1)
PY

CXX=""
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  CXX=x86_64-w64-mingw32-g++
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && command -v g++ >/dev/null 2>&1; then
  CXX=g++
fi
if [[ -z "$CXX" ]]; then
  skip "no Windows-target C++ compiler (compile part)"
fi
echo "[test_xess_compile] compiler: $("$CXX" --version | head -n 1)"
mkdir -p "$O"
# Same flags as test_xess.ps1, compile only.
for src in port/kz3/xess.cpp tools/smoke/test_xess.cpp tools/smoke/test_xess_temporal.cpp; do
  "$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cast-function-type \
    -I"$ROOT/port/kz3" -I"$PT/libs/video" -I"$PT/include" \
    -c "$ROOT/$src" -o "$O/$(basename "$src" .cpp).o"
  echo "[test_xess_compile] compiled $src"
done
echo "[test_xess_compile] PASS"
