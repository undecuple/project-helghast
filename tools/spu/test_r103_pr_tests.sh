#!/usr/bin/env bash
# Pass 103: the tests of the upstream PRs carried as patches 1117-1120, built from the patched toolkit
# (work/ps3recomp-patched, tools/ps3recomp-patched.sh; KZ3_PATCHED_DIR = another tree) the way the PRs' workflow lines
# build them: #450 runtime/spu/tests/test_spu_taskset_wake.c (two new cases), #449 tools/test_gcm_ea2io_range.py
# (its tools/test_gcm_io_mappings.py runs in tools/upstream/test_upstream_suite.sh). #445's test_spu_float_exact.c is not carried (patch 1118 header); #434 has none.
# Self-contained (no game data). Exit 1 if any test fails.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/spu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; fail=0
# KZ3 patch 1032's job accounting and wait-trace hooks, called from spu_channels.c paths the test does not reach (off).
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
if (cd "$T" && ${CC:-cc} -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$OUT/tw" \
      runtime/spu/tests/test_spu_taskset_wake.c runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
      runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
      runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c \
      runtime/spu/spu_interp.c runtime/platform/win32_compat.c "$OUT/stub.c" -lpthread -lm) >"$OUT/tw.log" 2>&1; then
  if "$OUT/tw" >"$OUT/tw.run" 2>&1; then echo "PASS test_spu_taskset_wake: $(tail -1 "$OUT/tw.run")"
  else echo "FAIL test_spu_taskset_wake: $(tail -1 "$OUT/tw.run" | cut -c1-200)"; fail=1; fi
else echo "FAIL test_spu_taskset_wake (build): $(grep -m1 -E 'error|undefined' "$OUT/tw.log" | cut -c1-200)"; fail=1; fi
# test_gcm_io_mappings.py: tools/upstream/test_upstream_suite.sh (stub header)
t=tools/test_gcm_ea2io_range.py
if [[ ! -f $T/$t ]]; then echo "FAIL $t: missing (patch 1119 not applied?)"; fail=1
elif python3 "$T/$t" >"$OUT/py.run" 2>&1; then echo "PASS $t: $(tail -1 "$OUT/py.run" | cut -c1-120)"
else echo "FAIL $t: $(tail -3 "$OUT/py.run" | tr '\n' ' ' | cut -c1-240)"; fail=1; fi
rm -rf "$OUT"
exit $fail
