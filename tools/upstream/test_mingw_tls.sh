#!/usr/bin/env bash
# Focused test for patches 1012 + 1015 (MinGW ignores __declspec(thread)).
#   nix develop .#win --command tools/sandbox.sh -- tools/upstream/test_mingw_tls.sh
# Compiles a two-variable file with the project's MinGW GCC: one variable spelled as v0.13.0 does under _WIN32
# (__declspec(thread)), one as 1012/1015 do for non-MSVC (__thread). Thread-local means: GCC emits a TLS access
# (emutls control object or a .tls section entry) instead of a plain .data/.bss symbol.
# Expected: __declspec(thread) -> warning "'thread' attribute directive ignored", plain global (FAIL for v0.13.0);
#           __thread -> thread-local (PASS for the patched spelling).
set -u
O="$(cd "$(dirname "$0")/../.." && pwd)/work/upstream-tests"; mkdir -p "$O"
CC="$(command -v x86_64-w64-mingw32-gcc || command -v x86_64-w64-mingw32ucrt-gcc)"
[[ -n "$CC" ]] || { echo "no MinGW gcc on PATH (use nix develop .#win)"; exit 2; }
rc=0
for spell in "__declspec(thread):v0.13.0" "__thread:patched"; do
  kw="${spell%%:*}" name="${spell#*:}"
  printf 'static %s int t_var;\nint* get(void) { return &t_var; }\n' "$kw" > "$O/tls_$name.c"
  warn="$("$CC" -O1 -S -o "$O/tls_$name.s" "$O/tls_$name.c" 2>&1)"
  if grep -q '__emutls\|\.tls' "$O/tls_$name.s"; then res="thread-local"; else res="process-wide (plain global)"; fi
  echo "== $name ($kw): $res${warn:+; compiler: $(echo "$warn" | grep -o "'thread' attribute directive ignored" | head -1)}"
  [[ $name == patched && $res != thread-local ]] && rc=1
done
"$CC" --version | head -1
exit $rc
