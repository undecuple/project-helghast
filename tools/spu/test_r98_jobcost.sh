#!/usr/bin/env bash
# Patch 1114 host test (tools/spu/test_r98_jobcost.c): KZ3_SPU_JOBCOST parsing and the job-call spin (a listed image's
# call costs F x its time; F 1, unlisted images, a close at another pc/depth and a malformed value spin nothing).
# Builds against work/ps3recomp-patched (or JC_TREE=DIR); SKIP without it or without patch 1114.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r98_jobcost.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/jc-tests"; mkdir -p "$O"
T="${JC_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_channels.c" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q kz3_jobcost_init_once "$T/runtime/spu/spu_channels.c" || { echo "SKIP: $T has no patch 1114"; exit 0; }
B="$(grep -o 'B="[^"]*"' "$ROOT/tools/spu/test_lr_lastcheck.sh" | sed 's/^B="//; s/"$//') runtime/spu/spu_channels.c"
extra=""; [[ -f "$T/runtime/diag/kz3_wait_acct.c" ]] && extra="runtime/diag/kz3_wait_acct.c -I runtime/diag"
rm -f "$O/t_jobcost"
(cd "$T" && gcc -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$O/t_jobcost" \
   "$ROOT/tools/spu/test_r98_jobcost.c" $B $extra -lm 2>&1 | grep -m5 "undefined reference\|error:\|multiple definition" | sort -u)
[[ -x "$O/t_jobcost" ]] || { echo "== t_jobcost: BUILD FAIL"; exit 1; }
rc=0
"$O/t_jobcost" 2>/dev/null || rc=1
"$O/t_jobcost" bad 2>/dev/null || rc=1
echo "== test_r98_jobcost: $([[ $rc == 0 ]] && echo PASS || echo FAIL)"
exit $rc
