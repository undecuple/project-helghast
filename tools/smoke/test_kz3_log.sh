#!/usr/bin/env bash
# host tests for port/kz3/kz3_log.{h,c}: builds and runs tools/smoke/test_kz3_log.c (+ the redirect TUs) with the
# host gcc, with -fsanitize=thread for the thread test as a second pass. Linux only (the Windows paths are covered by a
# MinGW compile check when x86_64-w64-mingw32-gcc is on PATH, i.e. in the .#win shell).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_kz3_log.sh
set -euo pipefail
O=build/smoke-host/kz3_log
rm -rf "$O"; mkdir -p "$O"
CC="${CC:-cc}"; CXX="${CXX:-c++}"
W="-std=c11 -Wall -Wextra -Werror -Wno-format-truncation -O2 -g -Iport/kz3"
$CC $W -c port/kz3/kz3_log.c -o "$O/kz3_log.o"
$CC $W -DKZ3_LOG_REDIRECT_STDIO -include port/kz3/kz3_log.h -c tools/smoke/test_kz3_log_redirect.c -o "$O/redir.o"
$CC $W tools/smoke/test_kz3_log.c "$O/kz3_log.o" "$O/redir.o" -lpthread -o "$O/test_kz3_log"
"$O/test_kz3_log" "$O/run1"
if command -v "$CXX" >/dev/null; then
  $CXX -std=c++17 -Wall -Werror -O1 -Iport/kz3 -DKZ3_LOG_REDIRECT_STDIO -include port/kz3/kz3_log.h \
    tools/smoke/test_kz3_log_redirect_cxx.cpp "$O/kz3_log.o" -o "$O/redir_cxx"
  "$O/redir_cxx" && echo "[PASS] C++ redirect survives <cstdio>" || { echo "[FAIL] C++ redirect"; exit 1; }
  # NEG: without the macros the real printf runs and the program exits 1
  $CXX -std=c++17 -O1 -Iport/kz3 tools/smoke/test_kz3_log_redirect_cxx.cpp -o "$O/redir_cxx_neg"
  if "$O/redir_cxx_neg" >/dev/null; then echo "[FAIL] NEG C++ without redirect"; exit 1; else echo "[PASS] NEG C++ without redirect"; fi
fi
# Second pass under ThreadSanitizer (data races in the ring / config / once set / fd).
if $CC -fsanitize=thread -x c /dev/null -c -o /dev/null 2>/dev/null; then
  T="-fsanitize=thread -O1 -Wno-tsan"
  $CC $W $T -c port/kz3/kz3_log.c -o "$O/kz3_log_tsan.o"
  $CC $W $T -DKZ3_LOG_REDIRECT_STDIO -include port/kz3/kz3_log.h -c tools/smoke/test_kz3_log_redirect.c -o "$O/redir_tsan.o"
  $CC $W $T tools/smoke/test_kz3_log.c "$O/kz3_log_tsan.o" "$O/redir_tsan.o" -lpthread -o "$O/test_kz3_log_tsan"
  TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$O/test_kz3_log_tsan" "$O/run2" | tail -3
fi
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  x86_64-w64-mingw32-gcc $W -c port/kz3/kz3_log.c -o "$O/kz3_log_win.o" && echo "[PASS] MinGW compile"
fi
echo "test_kz3_log.sh: OK"
