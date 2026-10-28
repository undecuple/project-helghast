#!/usr/bin/env bash
# Lock-line host tests (patch 1056 and the existing coherency tests): the upstream runtime/spu/tests
# test_spu_coherency.c and test_spu_lockline_peer.c, test_spu_mfc_slots.c, plus tools/spu/test_lockline_stress.c
# (reservations under contention: lost-reservation, ABA, counter, cross-stripe span cases, both lock modes).
# Builds against work/ps3recomp-patched (or LL_TREE=DIR); SKIP without it.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_lockline.sh [stress-args]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/ll-tests"; mkdir -p "$O"
T="${LL_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_coherency.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c runtime/platform/win32_compat.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rc=0
run() { # name, sources...
  local n="$1"; shift
  (cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/$n" "$@" -lm 2>&1 \
     | grep -m5 "undefined reference\|error:" | sort -u)
  [[ -x "$O/$n" ]] || { echo "== $n: BUILD FAIL"; rc=1; return; }
  local out; out="$("$O/$n" ${ARGS:-} 2>&1)"; local r=$?
  echo "== $n (exit $r): $(grep -v '^\[' <<<"$out" | tail -n1)"
  if [[ $r != 0 && $n == t_lockline_peer ]] && ll_expected_only <<<"$out"; then
    echo "   (the 2 failures are upstream c9a450d8's lock-free GETLLAR checks: patch 1056 replaces that path, expected)"; return; fi
  [[ $r == 0 ]] || { grep -m8 "FAIL" <<<"$out"; rc=1; }
}
# Patch 1056 (striped seqlock lock line, own slot registry) replaces upstream c9a450d8's lock-free GETLLAR
# (spu_coh_reserve_fast, never called here) and its list registration, the only writer of ctx->coh_listed. Two upstream
# checks read that path: a registered holder's GETLLAR completing under the held global lock (1056's GETLLAR takes the
# lock), and coh_listed == 1 after an unregistered context's GETLLAR (1056 re-registers it in a slot, coh_listed stays 0;
# the lock-wait half of that check passes). Exactly these two FAILs are expected; any other FAIL is not.
LL_EXPECTED_FAILS="a registered holder's GETLLAR of a marked line does not wait for the lock
an unregistered context's GETLLAR of a marked line still takes the lock"
ll_expected_only() { # stdin: test output
  local f; f="$(grep '^FAIL: ' | sed 's/^FAIL: //' | sort -u)"
  [[ "$f" == "$(sort -u <<<"$LL_EXPECTED_FAILS")" ]]
}
rm -f "$O"/t_*
run t_coherency runtime/spu/tests/test_spu_coherency.c
run t_coherency_striped -DPS3RECOMP_LL_STRIPED=1 runtime/spu/tests/test_spu_coherency.c
run t_lockline_peer runtime/spu/tests/test_spu_lockline_peer.c $B $extra
run t_mfc_slots runtime/spu/tests/test_spu_mfc_slots.c $B $extra
if [[ -f "$ROOT/tools/spu/test_lockline_stress.c" ]]; then
  ARGS="$*" run t_ll_stress "$ROOT/tools/spu/test_lockline_stress.c" $B $extra
  KZ3_LL_OLD=1 ARGS="$*" run t_ll_stress "$ROOT/tools/spu/test_lockline_stress.c" $B $extra
fi
exit $rc
