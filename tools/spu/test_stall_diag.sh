#!/usr/bin/env bash
# Patch 1081 / port/spu/kz3_stall_diag.c host test (SPU pass 63): KZ3_STALL_DIAG off leaves the hooks NULL; on, a
# policy-module run, a task run and a 0x220A78 PPU wait that have not returned for 3 s are described once per 5 s
# (the frame-arena record at LS r85 with the 0x5B9F0 free test), never after they end, and a run's end waits while
# the scan reads its context; a task parked at the taskset syscall (LS 0xA70) is not described
# until 3 s after the last scan that saw it parked (SPU pass 66). Also fails when the patched toolkit lacks the patch-1081 call sites.
# Self-contained: no game data.   nix develop --command tools/sandbox.sh -- tools/spu/test_stall_diag.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/runtime/spu/spu_context.h" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
for pair in "runtime/spu/spu_lifted_job.h:g_kz3_run_ctx_hook(&ctx, 1" "runtime/spu/spurs_policy.c:g_kz3_run_ctx_hook(ctx, 1, 1"\
            "runtime/spu/spu_channels.c:void (\*g_kz3_run_ctx_hook)" "runtime/spu/spu_workload.c:g_kz3_cur_taskset = j->taskset_ea"\
            "libs/spurs/cellSpurs.c:if (g_kz3_stall_scan) g_kz3_stall_scan"; do
  f="${pair%%:*}"; s="${pair#*:}"
  grep -qF -- "${s//\\/}" "$P/$f" || { echo "FAIL: patch 1081 call site missing in $f ($s)"; exit 1; }
done
${CC:-cc} -std=gnu11 -O1 -Wall -Wno-unused-function -I"$P/runtime/spu" -I"$P/runtime" -I"$P/include" -o "$O/t_stall_diag" \
  "$ROOT/tools/spu/test_stall_diag.c" "$ROOT/port/spu/kz3_stall_diag.c" -lpthread 2> "$O/t_stall_diag.log" ||
  { echo "FAIL: build"; head -20 "$O/t_stall_diag.log"; exit 1; }
"$O/t_stall_diag"
