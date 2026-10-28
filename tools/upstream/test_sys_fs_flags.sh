#!/usr/bin/env bash
# Host test for KZ3 patch 1139 (lv2 sys_fs_open, syscall 801, decodes the console's CELL_FS_O_* values; one header,
# include/ps3emu/cell_fs_flags.h, shared with cellFs; the Killzone 2 port's patch 6036 is the same change). Builds tools/upstream/test_sys_fs_flags.c
# against the patched toolkit's runtime/syscalls/sys_fs.c and runs it on a scratch tree: the defines, create with
# 0x41/0x42/0x241, no truncation with 0x41, append with 0x401/0x441, CELL_EEXIST with 0xC1 on an existing file,
# CELL_ENOENT without O_CREAT on a missing file, read-only unchanged. No game data, no GPU. Exit 1 on any failure.
#   nix develop --command tools/sandbox.sh -- tools/upstream/test_sys_fs_flags.sh
# NEW_TREE=<tree> names the toolkit directly (the negative control: a tree without the patch fails); else $KZ3_OUT,
# work/runtime and work are searched.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT" || exit 1
P=""
for d in ${NEW_TREE:+"$NEW_TREE"} work/ps3recomp-patched; do
  [[ -f "$d/runtime/syscalls/sys_fs.c" ]] && { P="$d"; break; }
done
[[ -n "$P" ]] || { echo "SKIP test_sys_fs_flags: no patched toolkit here (run tools/ps3recomp-patched.sh)"; exit 0; }
[[ -f "$P/include/ps3emu/cell_fs_flags.h" ]] \
  || echo "note: $P has no include/ps3emu/cell_fs_flags.h (no patch 1139; rerun tools/ps3recomp-patched.sh): expect FAIL"
CC="${CC:-cc}"
O="$ROOT/work/upstream-tests"; mkdir -p "$O"
T="$(mktemp -d "$O/sysfsflags.XXXXXX")"
trap '"$ROOT/tools/safe_rm.sh" "$T" >/dev/null 2>&1 || true' EXIT
inc=(); for d in runtime/syscalls include libs runtime runtime/ppu runtime/memory .; do inc+=(-I"$P/$d"); done
printf '%s\n' '#include "filesystem/cellFs.h"' \
  'const unsigned CFS_CREAT = CELL_FS_O_CREAT, CFS_EXCL = CELL_FS_O_EXCL, CFS_TRUNC = CELL_FS_O_TRUNC,' \
  '               CFS_APPEND = CELL_FS_O_APPEND;' > "$T/cellfs_vals.c"
"$CC" -std=gnu17 -O1 -w "${inc[@]}" -o "$T/t" tools/upstream/test_sys_fs_flags.c "$T/cellfs_vals.c" -pthread 2>"$T/err" \
  || { head -20 "$T/err"; echo "[FAIL] test_sys_fs_flags: build"; exit 1; }
mkdir -p "$T/root"
out="$("$T/t" "$T/root" 2>"$T/stderr")"; rc=$?
echo "$out" | grep "^\[FAIL\]" | head -20
echo "$out" | tail -1
if [[ $rc -ne 0 ]]; then echo "[FAIL] test_sys_fs_flags"; exit 1; fi
grep -q "^\[sys_fs\] open -> EEXIST (flags 0xc1)" "$T/stderr" || { echo "[FAIL] test_sys_fs_flags: no EEXIST log line"; exit 1; }
echo "[PASS] test_sys_fs_flags"
