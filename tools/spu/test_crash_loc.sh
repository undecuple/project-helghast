#!/usr/bin/env bash
# Host test of the crash-line formatting plus a MinGW compile check of the Windows helpers (SPU pass 137).
# Self-contained: the header include/ps3emu/crash_loc.h is extracted from port/patches/1011-*.patch.
#   nix develop .#win --command tools/sandbox.sh -- tools/spu/test_crash_loc.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="${TMPDIR:-/tmp}/crash_loc.$$"; mkdir -p "$T/include/ps3emu"; trap 'rm -rf "$T"' EXIT
awk '/^\+\+\+ b\/include\/ps3emu\/crash_loc.h/{on=1;getline;next} /^diff --git/{on=0} on&&/^\+/{print substr($0,2)}' \
  "$ROOT"/port/patches/1011-*.patch > "$T/include/ps3emu/crash_loc.h"
[[ -s "$T/include/ps3emu/crash_loc.h" ]] || { echo "SKIP: header not found in patch 1011"; exit 0; }
CC_HOST=""; for c in gcc cc; do command -v $c >/dev/null && { CC_HOST=$c; break; }; done
if [[ -n "$CC_HOST" ]]; then
  "$CC_HOST" -std=c11 -Wall -Wextra -I"$T/include" "$ROOT/tools/spu/test_crash_loc.c" -o "$T/t" && "$T/t"
else echo "SKIP host formatting test (no host gcc/cc in this shell; use .#re)"; fi
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  printf '#include "ps3emu/crash_loc.h"\nint main(void){char a[300],b[64];ps3_crash_where((void*)main,a,sizeof a);ps3_crash_thread_label(b,sizeof b);return a[0]+b[0];}\n' > "$T/w.c"
  x86_64-w64-mingw32-gcc -Wall -Wextra -I"$T/include" "$T/w.c" -o "$T/w.exe" && echo "PASS mingw compile (header Windows helpers)"
else echo "SKIP mingw compile (no x86_64-w64-mingw32-gcc)"; fi
