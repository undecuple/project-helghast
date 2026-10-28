#!/usr/bin/env bash
# Patch 2010 host test: build tools/smoke/test_fp_pack.c natively against the patched decompiler
# (work/ps3recomp-patched, made by tools/ps3recomp-patched.sh), run it, then compile the HLSL it wrote with
# glslangValidator's HLSL front end (-D) to SPIR-V as a syntax/type check.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_pack.sh   (glslangValidator: default shell)
set -euo pipefail
S="${PS3RECOMP_SRC:-work/ps3recomp-patched}"
O=build/smoke-host
mkdir -p "$O"
cc -std=c11 -Wall -Wextra -O1 -I"$S/include" -I"$S/libs/video" tools/smoke/test_fp_pack.c \
  "$S/libs/video/rsx_fp_decompiler.c" -o "$O/test_fp_pack"
"$O/test_fp_pack" "$O"
if command -v glslangValidator >/dev/null; then
  for f in "$O"/fp_pack_*.hlsl; do
    glslangValidator -D -S frag -e main -V "$f" -o "${f%.hlsl}.spv" >"${f%.hlsl}.log" 2>&1 \
      && echo "[PASS] glslang HLSL compile $(basename "$f")" \
      || { echo "[FAIL] glslang HLSL compile $(basename "$f")"; tail -5 "${f%.hlsl}.log"; exit 1; }
  done
else
  echo "glslangValidator not on PATH (use the default shell for the compile check)"
fi
