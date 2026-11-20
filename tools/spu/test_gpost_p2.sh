#!/usr/bin/env bash
# Host test for the GPU post plan P2 snapshot (port/spu/kz3_gpost.c, E3 round P2): command-list / D / parameter-block parser over fake
# guest memory for 1.14 and 1.00, negative controls per rejection, unset = byte-identical. Honours $CC and prints the compiler it used.
# Run under both compilers with the INNER form: the nix shell sets CC=gcc itself, so an outer `CC=clang nix develop ...` is
# overridden and silently runs gcc:
#   nix develop .#ci --command bash -c 'CC=gcc tools/spu/test_gpost_p2.sh'
#   nix develop .#ci --command bash -c 'CC=clang tools/spu/test_gpost_p2.sh'
# Mutation check of this test: tools/spu/mut_gpost_p2.sh (same inner form).
set -u
cd "$(dirname "$0")/../.." || exit 1
OUT=$(mktemp -d "${TMPDIR:-/tmp}/kz3_gpostp2_XXXXXX") || exit 1
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
echo "test_gpost_p2: compiler: $CC = $($CC --version 2>&1 | head -1)"
$CC -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  tools/spu/test_gpost_p2.c port/spu/kz3_gpost.c -o "$OUT/t" || { echo "FAIL: build"; exit 1; }
if "$OUT/t"; then
  echo "test_gpost_p2 ($CC): PASS"
else
  echo "test_gpost_p2 ($CC): FAIL"
  exit 1
fi
