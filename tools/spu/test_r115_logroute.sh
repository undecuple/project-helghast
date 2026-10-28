#!/usr/bin/env bash
# Spu pass 115 host tests (no game data, no work/ inputs):
#  1. tools/spu/test_r115_logroute.c: a port/spu-style fprintf(stderr) reaches the log file only with the redirect;
#     without it, ps3_stdio's 64 KB stderr buffer and its no-op (redirected) fflush lose the line when the run is killed.
#  2. port/spu/CMakeLists.txt configured with stub inputs under a stub C/C++ top level: kz3_ppu_overrides.cpp and
#     guest_fiber.c get -DKZ3_LOG_REDIRECT_STDIO -include kz3_log.h; the probes, kz3_cpu_check.c, the lifted sources and
#     spu_dispatch_mt.c do not; KZ3_LOG_REDIRECT=OFF turns it off. SKIP without cmake/ninja/c++.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r115_logroute.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$(mktemp -d "${TMPDIR:-/tmp}/r115.XXXXXX")"; [[ -n "${R115_KEEP:-}" ]] || trap 'rm -rf "$O"' EXIT; echo "work dir $O"
CC="${CC:-cc}"; W="-std=gnu17 -O1 -Wall -Wno-unused-function -Wno-format-truncation"
fail=0
$CC $W -I"$ROOT/port/kz3" -DKZ3_LOG_IMPLEMENTATION -c "$ROOT/port/kz3/kz3_log.c" -o "$O/kz3_log.o" || exit 1
$CC $W -c "$ROOT/tools/spu/test_r115_logroute_tu.c" -o "$O/raw.o" || exit 1
$CC $W -DKZ3_LOG_REDIRECT_STDIO -include "$ROOT/port/kz3/kz3_log.h" -c "$ROOT/tools/spu/test_r115_logroute_tu.c" -o "$O/routed.o" || exit 1
$CC $W -I"$ROOT/port/kz3" "$ROOT/tools/spu/test_r115_logroute.c" "$O/raw.o" "$O/routed.o" "$O/kz3_log.o" -lpthread -o "$O/t" || exit 1
mkdir -p "$O/run"; "$O/t" "$O/run" || fail=1

if command -v cmake >/dev/null && command -v ninja >/dev/null && command -v "${CXX:-c++}" >/dev/null; then
  S="$O/stub"; mkdir -p "$S/spu/gen" "$S/spu/lift/img_a" "$S/ps3/runtime/spu" "$S/ps3/include" "$S/recomp"
  echo img_a > "$S/spu/gen/sources.txt"
  : > "$S/spu/gen/kz3_spu_register.c"; : > "$S/spu/lift/img_a/spu_recomp.c"
  : > "$S/ps3/runtime/spu/spu_context.h"; : > "$S/ps3/runtime/spu/spu_dispatch_mt.c"; : > "$S/recomp/ppu_recomp.h"
  mkdir -p "$S/top"; printf 'cmake_minimum_required(VERSION 3.20)\nproject(r115top C CXX)\nadd_subdirectory("%s" spu)\n' "$ROOT/port/spu" > "$S/top/CMakeLists.txt"
  if cmake -S "$S/top" -B "$O/b" -G Ninja -DKZ3_SPU_DIR="$S/spu" -DPS3RECOMP_DIR="$S/ps3" -DRECOMP_DIR="$S/recomp" >"$O/cmake.log" 2>&1; then
    N="$O/b/build.ninja"; [[ -f "$O/b/spu/build.ninja" ]] && N="$O/b/spu/build.ninja"; cat "$O/b"/*.ninja "$O/b"/CMakeFiles/*.ninja > "$O/all.ninja" 2>/dev/null; N="$O/all.ninja"
    flags_of() { awk -v s="$1" '$1=="build" && index($0, s) && $0 ~ /\.obj:|\.o:/ {f=1; next} f && /^  FLAGS = /{print; exit} f && /^build /{exit}' "$N"; }
    for s in kz3_ppu_overrides.cpp guest_fiber.c; do
      if flags_of "$s" | grep -q 'kz3_log.h'; then echo "[PASS] $s gets the redirect"; else echo "[FAIL] $s has no redirect"; fail=1; fi
    done
    for s in kz3_stall_diag.c kz3_frame_trace.c kz3_cpu_check.c img_a/spu_recomp.c spu_dispatch_mt.c; do
      if [[ -z "$(flags_of "$s")" ]]; then echo "[FAIL] $s: no compile rule found"; fail=1
      elif flags_of "$s" | grep -q 'kz3_log.h'; then echo "[FAIL] $s got the redirect"; fail=1; else echo "[PASS] $s keeps its command"; fi
    done
    if grep -q 'DEFINES = .*KZ3_LOG_REDIRECT_STDIO' "$N"; then echo "[PASS] KZ3_LOG_REDIRECT_STDIO defined"; else echo "[FAIL] define missing"; fail=1; fi
    cmake -S "$S/top" -B "$O/b2" -G Ninja -DKZ3_SPU_DIR="$S/spu" -DPS3RECOMP_DIR="$S/ps3" -DRECOMP_DIR="$S/recomp" \
      -DKZ3_LOG_REDIRECT=OFF >/dev/null 2>&1 && ! cat "$O/b2"/*.ninja | grep -q 'kz3_log.h'  \
      && echo "[PASS] KZ3_LOG_REDIRECT=OFF leaves port/spu unredirected" || { echo "[FAIL] KZ3_LOG_REDIRECT=OFF"; fail=1; }
  else
    echo "[FAIL] standalone configure of port/spu"; tail -5 "$O/cmake.log"; fail=1
  fi
else
  echo "SKIP: cmake/ninja not on PATH (CMake part)"
fi
(( fail )) && { echo "FAIL: test_r115_logroute.sh"; exit 1; }
echo "PASS: test_r115_logroute.sh"
