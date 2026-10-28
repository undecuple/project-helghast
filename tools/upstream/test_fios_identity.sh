#!/usr/bin/env bash
# FIOS identity model (RE pass 165): nix develop .#re --command tools/sandbox.sh -- tools/upstream/test_fios_identity.sh
# Self-contained: needs no toolkit and no game data. Exit 1 on any FAIL.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$(mktemp -d)"; trap 'rm -rf "$O"' EXIT
g++ -std=gnu++17 -O1 -Wall -o "$O/t" "$ROOT/tools/upstream/test_fios_identity.cpp" 2>"$O/err" || { echo "FAIL build"; head -15 "$O/err"; exit 1; }
"$O/t" | tee "$O/out" | grep "^\[FAIL\]\|^A \|^B \|^PASS\|^FAIL"; grep -q "^PASS" "$O/out"
