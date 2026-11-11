#!/usr/bin/env bash
# XeSS camera bridge, flip pairing and freshness host test. No game data or GPU.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$ROOT/build/smoke-host/xess-camera"
mkdir -p "$O"
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/port/kz3" "$ROOT/tools/smoke/test_xess_camera.c" \
  -lm -o "$O/test_xess_camera"
"$O/test_xess_camera"
