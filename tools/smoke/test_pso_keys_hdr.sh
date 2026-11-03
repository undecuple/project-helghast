#!/usr/bin/env bash
# Host test for the PSO key file header and its shader translator check (patch 2141, from the Killzone 2 port's 6047):
# builds tools/smoke/test_pso_keys_hdr.c against the patched toolkit's libs/video/rsx_pso_keys_hdr.h, once with a
# defined RSX_TRANSLATOR_ID and once without (id 0). No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pso_keys_hdr.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_pso_keys_hdr.h" ]]; then
  echo "[test_pso_keys_hdr] SKIP: no patched toolkit with patch 2141 ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/pso_keys_hdr"; mkdir -p "$O"
F=(-std=c11 -O2 -Wall -Wextra -Werror -I"$V")
${CC:-cc} "${F[@]}" -DRSX_TRANSLATOR_ID=0x0123456789ABCDEFull "$ROOT/tools/smoke/test_pso_keys_hdr.c" -o "$O/with_id"
${CC:-cc} "${F[@]}" -DEXPECT_ID0 "$ROOT/tools/smoke/test_pso_keys_hdr.c" -o "$O/id0"
"$O/with_id"; "$O/id0"
echo "test_pso_keys_hdr.sh: ok"
