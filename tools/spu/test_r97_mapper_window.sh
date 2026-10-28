#!/usr/bin/env bash
# Patch 1113 host test (SPU pass 97, tools/spu/test_r97_mapper_window.c): runtime-owned windows (patch 5005's font
# pool at 0xB8000000 sits inside v0.17.0's mapper range 0x40000000-0xC0000000) are never handed out by the mapper or
# the user allocator. Needs work/ps3recomp-patched (SPU_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r97_mapper_window.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/syscalls/sys_memory.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "sys_memory_reserve_window" "$T/runtime/syscalls/sys_memory.c" || { echo "FAIL: $T has no patch 1113"; exit 1; }
rm -f "$O/t_mapper_window"
(cd "$T" && ${CC:-gcc} -std=gnu17 -O1 -w -pthread -I . -I include -o "$O/t_mapper_window" \
    "$ROOT/tools/spu/test_r97_mapper_window.c" runtime/syscalls/sys_memory.c 2>&1 | grep -m8 error)
[[ -x "$O/t_mapper_window" ]] || { echo "FAIL: build"; exit 1; }
timeout 20 "$O/t_mapper_window"; r=$?
[[ $r == 0 ]] || { echo "FAIL: rc=$r"; exit 1; }
