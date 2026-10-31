#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/smoke-host/fsr31-motion"
mkdir -p "$OUT"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/port/kz3" \
  "$ROOT/tools/smoke/test_fsr31_motion.c" -lm \
  -o "$OUT/test_fsr31_motion"
"$OUT/test_fsr31_motion"
