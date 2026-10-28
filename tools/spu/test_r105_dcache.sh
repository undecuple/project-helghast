#!/usr/bin/env bash
# Pass 105: patches 1123 (upstream #420, resident-code ownership shared by indirect dispatch and interpreter rejoin)
# and 1124 (upstream #481's dispatch cache, kept in the spu_context). Builds from the patched toolkit
# (work/ps3recomp-patched, tools/ps3recomp-patched.sh; KZ3_PATCHED_DIR = another tree):
#  - the PRs' runtime/spu/tests/test_spu_lifted_start.c (its header's build line) with SPU_DISPATCH_CACHE unset, 0, 1, 2;
#  - tools/spu/test_r105_dcache.c: the cache is per context (filled on one host thread, hit on another), default off,
#    shadow mode counts a stale entry and returns the resolver's answer.
# Self-contained (no game data). Exit 1 if any test fails.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/spu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; fail=0
cat > "$OUT/stub.c" <<'C'
#include <stdint.h>
int g_kz3_wacct = 0;
int g_kz3_cpuprobe = 0;   // patch 1135 (runtime/diag/kz3_wait_acct.c), read from spu_channels.c
void kz3_wacct_job(int img) { (void)img; }
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t) = 0;
uint64_t kz3_wacct_now(void) { return 0; }
void kz3_wacct_cpu_probe(void) {}
void kz3_wacct_trace(int a, uint32_t b, uint64_t c, uint64_t d) { (void)a; (void)b; (void)c; (void)d; }
C
RT=(runtime/spu/spu_channels.c runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c
    runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c
    runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c
    runtime/spu/spu_lifted_thread.c runtime/syscalls/spu_fallback.c runtime/syscalls/sdk_version_weak.c
    runtime/platform/win32_compat.c)
build() {   # NAME MAIN.c
  (cd "$T" && ${CC:-cc} -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$OUT/$1" "$2" "${RT[@]}" \
     "$OUT/stub.c" -lpthread -lm) >"$OUT/$1.log" 2>&1 \
  || { echo "FAIL $1 (build): $(grep -m1 -E 'error|undefined' "$OUT/$1.log" | cut -c1-200)"; fail=1; return 1; }
}
run() {   # NAME MODE (empty = unset)
  local m="$2" r
  if [[ -n $m ]]; then r="$(SPU_DISPATCH_CACHE=$m "$OUT/$1" 2>&1)"; else r="$(env -u SPU_DISPATCH_CACHE "$OUT/$1" 2>&1)"; fi
  local rc=$?
  if [[ $rc == 0 ]] && ! grep -q 'shadow mismatch' <<<"$r"; then echo "PASS $1 SPU_DISPATCH_CACHE=${m:-unset}: $(tail -1 <<<"$r")"
  else echo "FAIL $1 SPU_DISPATCH_CACHE=${m:-unset}: $(grep -m3 -E 'FAIL|mismatch' <<<"$r" | tr '\n' ' ' | cut -c1-240) $(tail -1 <<<"$r")"; fail=1; fi
}
if build lifted_start runtime/spu/tests/test_spu_lifted_start.c; then for m in "" 0 1 2; do run lifted_start "$m"; done; fi
if build r105 "$ROOT/tools/spu/test_r105_dcache.c"; then
  r="$(env -u SPU_DISPATCH_CACHE "$OUT/r105" 2>&1)"; rc=$?
  if [[ $rc == 0 ]]; then echo "PASS test_r105_dcache: $(tail -1 <<<"$r")"
  else echo "FAIL test_r105_dcache: $(grep -m4 FAIL <<<"$r" | tr '\n' ' ' | cut -c1-300)"; fail=1; fi
fi
rm -rf "$OUT"
exit $fail
