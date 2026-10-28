#!/usr/bin/env bash
# Build tools/smoke/test_fp_d3d12.c for Windows (patches 2010-2012 under D3D12; run it with tools/proton-test.sh).
#   nix develop .#win --command tools/sandbox.sh -- tools/smoke/test_fp_d3d12.sh
#   tools/agents/lock.sh gpu -- nix develop --command tools/proton-test.sh --seconds 20 --name fp-d3d12 \
#       build/smoke-fp/test_fp_d3d12.exe
set -euo pipefail
S="${PS3RECOMP_SRC:-work/ps3recomp-patched}"
O=build/smoke-fp
mkdir -p "$O"
x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall -I"$S/include" -I"$S/libs/video" tools/smoke/test_fp_d3d12.c \
  "$S/libs/video/rsx_fp_decompiler.c" -o "$O/test_fp_d3d12.exe" -ld3d12 -ld3dcompiler -ldxguid -static
echo "built $O/test_fp_d3d12.exe"
