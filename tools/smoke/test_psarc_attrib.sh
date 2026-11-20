#!/usr/bin/env bash
# Host test for the RSX_ATTRIB=1 [psarc] lines (port/kz3/kz3_psarc_attrib.h) plus static checks that psarc_vfs.cpp reads the
# switch like patch 2187 does and prints nothing without it. No game data.
#   nix develop .#ci --command tools/smoke/test_psarc_attrib.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$ROOT/build/smoke-host/psarc-attrib"; mkdir -p "$O"
CXX="${CXX:-c++}"
$CXX -std=c++17 -Wall -Wextra -Werror -O2 -I"$ROOT/port/kz3" "$ROOT/tools/smoke/test_psarc_attrib.cpp" -o "$O/t"
"$O/t"
V="$ROOT/port/kz3/psarc_vfs.cpp"
grep -qF 'getenv("RSX_ATTRIB")' "$V" || { echo "[FAIL] psarc_vfs.cpp does not read RSX_ATTRIB"; exit 1; }
grep -qF 'const bool att = attrib_on();' "$V" && grep -qF 'if (att) {' "$V" || { echo "[FAIL] the open line is not behind the switch"; exit 1; }
# every clock read in the miss path is guarded by the switch
n=$(grep -c 'att ? attrib_qpc() : 0' "$V"); [[ "$n" == 2 ]] || { echo "[FAIL] guarded clock reads: $n"; exit 1; }
echo "[test_psarc_attrib] static checks ok"
