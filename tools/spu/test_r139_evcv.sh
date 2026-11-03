#!/usr/bin/env bash
# Spu pass 139: ps3_evcv (patch 1148) host test on the POSIX Win32 shim. Needs the patched tree (patch 1148 applied):
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_r139_evcv.sh
# SKIP (exit 0) when work/ps3recomp-patched has no runtime/platform/ps3_evcv.c; PT=dir overrides the tree.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PT="${PT:-$ROOT/work/ps3recomp-patched}"
[[ -f "$PT/runtime/platform/ps3_evcv.c" ]] || { echo "SKIP: $PT has no ps3_evcv.c (regenerate with tools/ps3recomp-patched.sh)"; exit 0; }
O="$(mktemp -d "${TMPDIR:-/tmp}/r139.XXXXXX")"; trap 'rm -rf "$O"' EXIT
CC="${CC:-cc}"
$CC -std=gnu17 -O1 -Wall -Wno-unused-function -DPS3_EVCV_SHIM -I"$PT/runtime/platform" \
  "$ROOT/tools/spu/test_r139_evcv.c" "$PT/runtime/platform/ps3_evcv.c" "$PT/runtime/platform/win32_compat.c" -lpthread -o "$O/t" 2>"$O/cc.log" \
  || { head -20 "$O/cc.log"; echo FAIL: build; exit 1; }
timeout 120 "$O/t"
