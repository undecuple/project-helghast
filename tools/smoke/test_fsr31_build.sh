#!/usr/bin/env bash
# Build check for the FSR 3.1 adapter (port/kz3/fsr31.cpp) and its D3D12 smoke test (tools/smoke/test_fsr31.cpp),
# with the flags tools/smoke/test_fsr31.ps1 uses, so a break in either fails CI (review of PR #27, point 5). This only
# compiles and links the Windows exe: running it needs Windows, a D3D12 GPU and the pinned FidelityFX DLL, which is
# what test_fsr31.ps1 does. No game data. SKIP without a MinGW-w64 C++ compiler or without the patched toolkit.
#   nix develop .#win --command tools/smoke/test_fsr31_build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
CXX_WIN=""
for c in "${CXX_WIN64:-}" x86_64-w64-mingw32-g++ x86_64-w64-mingw32ucrt-g++; do
  if [[ -n "$c" ]] && command -v "$c" >/dev/null 2>&1; then CXX_WIN="$c"; break; fi
done
if [[ -z "$CXX_WIN" && "$(c++ -dumpmachine 2>/dev/null)" == *mingw* ]]; then CXX_WIN=c++; fi   # .#win shell: c++ is the cross compiler
if [[ -z "$CXX_WIN" ]]; then
  echo "[test_fsr31_build] SKIP: no MinGW-w64 C++ compiler (run in nix develop .#win)"; exit 0; fi
if ! grep -qs rsx_upscale_register_backend "$PT/libs/video/rsx_upscale.h" || ! grep -qs rsx_live_upscale_hooks "$PT/libs/video/rsx_live_draw.h"; then
  echo "[test_fsr31_build] SKIP: toolkit without patches 2220-2221 ($PT; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/fsr31-build"; mkdir -p "$O"
flags=(-std=c++20 -O2 -Wall -Wextra -Wno-cast-function-type -Wno-switch
       -I"$ROOT/port/kz3" -I"$PT/libs/video" -I"$PT/include" -I"$ROOT/third_party/fidelityfx-sdk/ffx-api/include")
if ! "$CXX_WIN" "${flags[@]}" "$ROOT/port/kz3/fsr31.cpp" "$ROOT/tools/smoke/test_fsr31.cpp" \
     -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 -o "$O/test_fsr31.exe" 2>"$O/build.log"; then
  echo "[test_fsr31_build] FAIL: $CXX_WIN could not build fsr31.cpp + test_fsr31.cpp"; head -20 "$O/build.log"; exit 1; fi
echo "[test_fsr31_build] PASS: fsr31.cpp and test_fsr31.cpp build for Windows ($CXX_WIN)"
