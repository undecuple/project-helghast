#!/usr/bin/env bash
# Pass 89: upstream PR #366 (9366) on the patched toolkit (tools/ps3recomp-patched.sh): test_spurs_job_stack must
# pass with the fix and fail with spurs_job.c's $sp put back to stack_top - 16 (before/after); the neighbouring SPURS
# runtime tests (taskset wake, lock-line peer, mfc slots) must still pass. Exit 1 on any unexpected result.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${PS3RECOMP_PATCHED:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/spu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"
fail=0
SRC="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c
 runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c
 runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
build() { # name test.c [source tree]
  (cd "${3:-$T}" && ${CC:-cc} -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$OUT/$1" \
     "$T/runtime/spu/tests/$2" $SRC $extra -lpthread -lm) >"$OUT/$1.log" 2>&1; }
# The two upstream lock-line checks patch 1056 answers differently by design (reason in test_lockline.sh).
LL_EXPECTED_FAILS="a registered holder's GETLLAR of a marked line does not wait for the lock
an unregistered context's GETLLAR of a marked line still takes the lock"
ll_expected_only() { local f; f="$(grep '^FAIL: ' | sed 's/^FAIL: //' | sort -u)"; [[ "$f" == "$(sort -u <<<"$LL_EXPECTED_FAILS")" ]]; }
for t in test_spurs_job_stack test_spu_taskset_wake test_spu_lockline_peer test_spu_mfc_slots; do
  if ! build $t $t.c; then echo "FAIL $t (build): $(grep -m3 -E 'undefined|error' "$OUT/$t.log" | cut -c1-200)"; fail=1; continue; fi
  if "$OUT/$t" >"$OUT/$t.run" 2>&1; then echo "PASS $t: $(tail -1 "$OUT/$t.run" | cut -c1-120)"
  elif [[ $t == test_spu_lockline_peer ]] && ll_expected_only <"$OUT/$t.run"; then
    echo "PASS $t: $(tail -1 "$OUT/$t.run" | cut -c1-80) (the 2 are patch 1056's expected ones, see test_lockline.sh)"
  else echo "FAIL $t: $(tail -1 "$OUT/$t.run" | cut -c1-200)"; fail=1; fi
done
# before: the same tree with the old frame
OLD="$OUT/old"; mkdir -p "$OLD"; cp -r "$T/include" "$T/runtime" "$T/libs" "$OLD/"
sed -i 's/(stack_top - 32u) & ~15u/(stack_top - 16u) \& ~15u/' "$OLD/runtime/spu/spurs_job.c"
if ! build old_stack test_spurs_job_stack.c "$OLD"; then echo "FAIL old build: $(grep -m3 -E 'undefined|error' "$OUT/old_stack.log" | cut -c1-200)"; fail=1
elif "$OUT/old_stack" >"$OUT/old.run" 2>&1; then echo "FAIL: stack test passes with the old frame (no overlap shown)"; fail=1
else echo "PASS before/after: old frame fails ($(tail -1 "$OUT/old.run" | cut -c1-100))"; fi
exit $fail
