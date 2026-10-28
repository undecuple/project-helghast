#!/usr/bin/env bash
# Pass 104 (patch 1122): lv2 sync semantics host tests, built from the patched toolkit (work/ps3recomp-patched;
# KZ3_PATCHED_DIR = another tree, e.g. one without patch 1122 to see them fail):
#   test_r104_lv2_mutex.c  sys_mutex / sys_cond (runtime/syscalls) + the toolkit's tests/sync_stress/test_shims.c
#   test_r104_lwsync.cpp   HLE sys_lwmutex / sys_lwcond (runtime/ppu/ppu_sysprx.cpp) through its registered handlers
# plus the toolkit's own tests/sync_stress suite (sync_stress, test_cond_semantics) on the same sources.
# Self-contained (no game data). ITERS = iterations of the racing cases (default 50). Exit 1 if any test fails.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/syscalls ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
T="$(cd "$T" && pwd)"
ITERS="${ITERS:-50}"; OUT="$(mktemp -d)"; fail=0
CC="${CC:-cc}"; CXX="${CXX:-c++}"
run() {  # name exe args...
  local n=$1; shift
  local rc=0; timeout 60 "$@" >"$OUT/$n.run" 2>&1 || rc=$?
  grep -E '^(PASS|FAIL|condition|  [a-z])' "$OUT/$n.run" | sed 's/^/  /' | head -24
  if [[ $rc == 0 ]]; then echo "PASS $n"; else echo "FAIL $n (exit $rc)"; fail=1; fi
}
SY="$T/runtime/syscalls"
if (cd "$T" && $CC -std=gnu17 -O2 -w -pthread -I . -I runtime -I include -I runtime/syscalls -o "$OUT/mx" \
      "$ROOT/tools/spu/test_r104_lv2_mutex.c" tests/sync_stress/test_shims.c "$SY/sys_mutex.c" "$SY/sys_cond.c" \
      runtime/platform/win32_compat.c) >"$OUT/mx.log" 2>&1; then run lv2_mutex "$OUT/mx" "$ITERS"
else echo "FAIL lv2_mutex (build): $(grep -m1 -E 'error|undefined' "$OUT/mx.log" | cut -c1-200)"; fail=1; fi
if (cd "$T" && $CXX -std=gnu++17 -O2 -w -pthread -I "$ROOT/tools/upstream/stub" -I include -I runtime -I runtime/ppu \
      -I runtime/platform -o "$OUT/lw" "$ROOT/tools/spu/test_r104_lwsync.cpp" runtime/ppu/ppu_sysprx.cpp \
      -x c runtime/platform/win32_compat.c) >"$OUT/lw.log" 2>&1; then run lwsync "$OUT/lw" "$ITERS"
else echo "FAIL lwsync (build): $(grep -m1 -E 'error|undefined' "$OUT/lw.log" | cut -c1-200)"; fail=1; fi
# The toolkit's sync suite on the same sources (-include win32_compat.h: KZ3 patches use LONG in sys_event.c on POSIX) (what its CI runs through tests/sync_stress/CMakeLists.txt).
SS="$T/tests/sync_stress"
# KZ3 diagnostics sys_event.c reaches (patches 10xx); never called by the suite.
printf '%s\n' 'unsigned long long ps3_ms_now(void) { return 0; }' 'void spu_taskset_dump_tracked(const char* w) { (void)w; }' > "$OUT/ssstub.c"
if (cd "$T" && $CC -std=gnu17 -O2 -w -pthread -c -o "$OUT/w32.o" runtime/platform/win32_compat.c && \
      $CC -std=gnu17 -O2 -w -pthread -include runtime/platform/win32_compat.h -I "$SY" -I runtime -I include -o "$OUT/ss" \
      "$SS/main.c" "$SS/test_shims.c" "$SY/sys_mutex.c" "$SY/sys_cond.c" "$SY/sys_semaphore.c" "$SY/sys_event.c" \
      "$SY/sys_rwlock.c" "$OUT/w32.o" "$OUT/ssstub.c") >"$OUT/ss.log" 2>&1; then run sync_stress "$OUT/ss"
else echo "FAIL sync_stress (build): $(grep -m1 -E 'error|undefined' "$OUT/ss.log" | cut -c1-200)"; fail=1; fi
if (cd "$T" && $CC -std=gnu17 -O2 -w -pthread -I . -I "$SY" -I runtime -I include -o "$OUT/cs" "$SS/test_cond_semantics.c" \
      "$SS/test_shims.c" "$SY/sys_mutex.c" "$SY/sys_cond.c" runtime/platform/win32_compat.c) >"$OUT/cs.log" 2>&1; then
  for m in recursive spurious owner; do run "cond_semantics_$m" "$OUT/cs" $m; done
else echo "FAIL test_cond_semantics (build): $(grep -m1 -E 'error|undefined' "$OUT/cs.log" | cut -c1-200)"; fail=1; fi
rm -rf "$OUT"
exit $fail
