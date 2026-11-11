#!/usr/bin/env bash
# XeSS deferred-input selection host test. No game data or GPU.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$ROOT/build/smoke-host/xess-inputs"
mkdir -p "$O"
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/port/kz3" "$ROOT/tools/smoke/test_xess_inputs.c" \
  -lm -o "$O/test_xess_inputs"
"$O/test_xess_inputs"
