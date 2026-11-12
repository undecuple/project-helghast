#!/usr/bin/env bash
# Host test for KZ3 patch 2163 (program identity cache in get_pso): cached == reference for every derived value and the
# final key over scripted sequences and a fuzz, a negative control (memcmp skipped must FAIL), an ASan/UBSan pass and a
# micro-benchmark against a model of the old one-slot memos. Needs the patched toolkit (SKIP without patch 2163).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pso_memo.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_pso_memo.h" ]]; then echo "[test_pso_memo] SKIP: toolkit without patch 2163 (run tools/ps3recomp-patched.sh)"; exit 0; fi
O="$ROOT/build/smoke-host/pso_memo"; mkdir -p "$O"
CC="${CC:-cc}"
W="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -I$V -I$PT/include"
# the toolkit decompilers are compiled as they are (their warnings are not this test's business)
for f in rsx_fp_decompiler rsx_vp_decompiler; do $CC -std=gnu11 -w -O2 -g -I"$V" -I"$PT/include" -c "$V/$f.c" -o "$O/$f.o"; done
SRCS="$ROOT/tools/smoke/test_pso_memo.c $O/rsx_fp_decompiler.o $O/rsx_vp_decompiler.o"
$CC $W -O2 $SRCS -o "$O/test_pso_memo" -lm
"$O/test_pso_memo"
$CC $W -O2 -DRSX_PM_TEST_FLAW $SRCS -o "$O/test_pso_memo_flaw" -lm
if "$O/test_pso_memo_flaw" >/dev/null; then echo "[test_pso_memo] FAIL: negative control (no byte validation) not detected"; exit 1; fi
echo "[test_pso_memo] negative control: unvalidated reuse detected (PASS)"
if echo "int main(void){return 0;}" | $CC -fsanitize=address,undefined -x c - -o "$O/probe" 2>/dev/null && "$O/probe"; then
  $CC $W -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $SRCS -o "$O/test_asan" -lm
  "$O/test_asan" | grep -v "^\[time\]" | tail -2 && echo "[PASS] ASan/UBSan pass"
fi
