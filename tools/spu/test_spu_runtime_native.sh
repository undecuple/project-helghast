#!/usr/bin/env bash
# Native build + run of the upstream SPU runtime tests that patches 1013 and 1017 touch (MFC slots, lifted start).
# Usage: nix develop --command tools/sandbox.sh -- tools/spu/test_spu_runtime_native.sh  (needs work/ps3recomp-patched)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
cd "$ROOT/work/ps3recomp-patched" || exit 1
B="runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c"
gcc -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$O/t_mfc" runtime/spu/tests/test_spu_mfc_slots.c $B runtime/diag/kz3_wait_acct.c runtime/platform/win32_compat.c -lpthread -lm 2>&1 | grep -m5 error
"$O/t_mfc" 2>&1 | tail -1
EXTRA=$(sed -n '/^ \* Build:/,/^ \*$/p' runtime/spu/tests/test_spu_lifted_start.c | grep -o 'runtime/[a-z_/0-9]*\.c' | grep -v tests/ | tr '\n' ' ')
gcc -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$O/t_ls" runtime/spu/tests/test_spu_lifted_start.c $EXTRA runtime/diag/kz3_wait_acct.c -lpthread -lm 2>&1 | grep -m8 error
"$O/t_ls" 2>&1 | tail -1
# Patch 1052 (atomic event_status raise/ack): the upstream SPU-to-SPU lock-line test still passes.
gcc -std=gnu17 -w -I include -I runtime/spu -I runtime/platform -o "$O/t_lp" runtime/spu/tests/test_spu_lockline_peer.c $B runtime/diag/kz3_wait_acct.c runtime/platform/win32_compat.c -lpthread -lm 2>&1 | grep -m8 error
"$O/t_lp" 2>&1 | tail -1
