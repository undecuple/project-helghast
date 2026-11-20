#!/usr/bin/env bash
# Host test for the SPU pass 161 helpers of the native MotionBlur row kernel (port/spu/kz3_native_post.c: mb_rec_scalar,
# mb_cflt_bytes) against the four-lane SPU helpers of the patched toolkit. SKIP without work/ps3recomp-patched (or SPU_TREE).
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_r161_mb_fast.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/host-tests/mb-fast"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -f "$T/runtime/spu/spu_helpers.h" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
SRC="$ROOT/port/spu/kz3_native_post.c"
python3 - "$SRC" "$O/mbfast.h" <<'P' || { echo FAIL: extract; exit 1; }
import sys
s = open(sys.argv[1]).read()
a = s.index("static inline uint32_t mb_rec_scalar"); c = s.index("static const int32_t mb_c[6]")
open(sys.argv[2], "w").write(s[a:c])
P
${CC:-gcc} -O2 -msse4.1 -mfma -ffp-contract=off -DMBFAST_H="\"$O/mbfast.h\"" -I "$T/runtime/spu" -I "$T/include" \
    -o "$O/t_mb_fast" "$ROOT/tools/spu/test_r161_mb_fast.c" 2>&1 | grep -m5 -E "error|undefined"
[[ -x "$O/t_mb_fast" ]] || { echo "FAIL: build"; exit 1; }
out="$(timeout 300 "$O/t_mb_fast" 2>&1)"; r=$?
echo "$out" | tail -6
exit $r
