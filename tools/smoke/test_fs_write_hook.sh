#!/usr/bin/env bash
# Host test for KZ3 patch 2078 (cellFsRead / cellFsAioRead report the guest bytes they write through
# fs_set_guest_write_hook). Builds tools/smoke/test_fs_write_hook.cpp against the patched toolkit (ppu_fs.cpp compiled
# in, stubs for the rest) and runs it. No game data.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fs_write_hook.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
if [[ ! -f "$PT/runtime/ppu/ppu_fs.cpp" ]]; then echo "[test_fs_write_hook] SKIP: no patched toolkit (run tools/ps3recomp-patched.sh)"; exit 0; fi
RH="$ROOT/work/recomp"; [[ -f "$RH/ppu_recomp.h" ]] || RH="$ROOT/tools/upstream/stub"
O="$ROOT/build/smoke-host/fs_write_hook"; mkdir -p "$O"
(cd "$PT" && g++ -std=gnu++17 -w -I . -I include -I runtime -I runtime/ppu -I runtime/memory -I "$RH" -I libs \
   -o "$O/test_fs_write_hook" "$ROOT/tools/smoke/test_fs_write_hook.cpp")
"$O/test_fs_write_hook"
