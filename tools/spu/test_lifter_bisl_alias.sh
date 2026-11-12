#!/usr/bin/env bash
# Host test for the SPU lifter bisl/bisled fix (patch 1156 on ps3recomp v0.18, upstream since v0.19): bisl/bisled read the target before writing the link). Lifts a 5-instruction local-store
# image with the patched spu_lifter.py and checks the emitted C: `bisl $7,$7` and `bisled $7,$7,$3` (rt == ra) must capture the
# target from r7 before the link store to r7, and branch to the captured value; bisled must take ra, not rb ($3).
# NEGATIVE CONTROL: the same check on a lifter with patch 1156 reversed (git apply -R; on v0.19, where 1156 is gone or does not reverse, with the link write moved before the target read) must FAIL. Self-contained, no game
# data, python only (no linking, so no --unresolved-symbols flags).
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_lifter_bisl_alias.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/tools/spu_lifter.py" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
O="$ROOT/work/spu/t_bisl"; "$ROOT/tools/safe_rm.sh" "$O" 2>/dev/null; mkdir -p "$O/neg/tools"
python3 - "$O/t.bin" <<'PY'
import struct, sys
w = [0x35200000 | (7 << 7) | 7,                 # bisl   $7,$7
     0x35600000 | (3 << 14) | (7 << 7) | 7,     # bisled $7,$7 (rb field = 3, must not be the target)
     0x35000000]                                # bi $0
open(sys.argv[1], "wb").write(struct.pack(">3I", *w))
PY
check() {  # check LIFTER -> 0 when both sites capture the target first
  python3 "$1" "$O/t.bin" -o "$2" --base 0x100 >/dev/null 2>&1 || { echo "lift failed"; return 2; }
  python3 - "$2/spu_recomp.c" <<'PY'
import re, sys
ok = True; seen = 0
for l in open(sys.argv[1]):
    if "spu_link(0x104)" in l and "spu_indirect_branch" in l:
        m = re.search(r'(\w+) = ctx->gpr\[7\]\._u32\[0\];.*gpr\[7\] = spu_link.*ctx->pc = \1;', l)
        print("bisl  :", "captured before link" if m else "TARGET READ AFTER LINK"); ok &= bool(m)
    if "spu_splat_u32(0x108)" in l or "spu_link(0x108)" in l:   # v0.18 emitted spu_splat_u32, v0.19 spu_link
        m = re.search(r'(\w+) = ctx->gpr\[7\]\._u32\[0\];.*gpr\[7\] = spu_(?:splat_u32|link).*ctx->pc = \1;', l)
        print("bisled:", "captured ra before link" if m else "TARGET NOT ra-before-link"); ok &= bool(m)
        seen += 1
    if "spu_link(0x104)" in l and "spu_indirect_branch" in l: seen += 1
if seen != 2: print("bisl/bisled site not found in the lifted C (seen=%d)" % seen); ok = False
sys.exit(0 if ok else 1)
PY
}
check "$P/tools/spu_lifter.py" "$O/pos" || { echo "FAIL: patched lifter"; exit 1; }
cp "$P"/tools/*.py "$O/neg/tools/"
if [[ -f "$ROOT/port/patches/1156-spu-bisl-target-before-link.patch" ]] && \
   git apply -R --directory="${O#"$ROOT"/}/neg" "$ROOT/port/patches/1156-spu-bisl-target-before-link.patch" 2>"$O/rev.err"; then
  # ps3recomp v0.18: patch 1156 carries the fix, the negative control is the reversed patch
  what="reversed patch 1156"
else
  # ps3recomp v0.19: the fix is upstream (1156 dropped). Negative control: put the link write back before the
  # target read at both sites (the bug 1000-spu once reintroduced), by rewriting the lifter source.
  python3 - "$O/neg/tools/spu_lifter.py" <<'PY' || { echo "FAIL: cannot build the negative control (lifter layout changed)"; exit 1; }
import re, sys
p = sys.argv[1]; t = open(p).read()
pat = r'\{\{ uint32_t _tg = \{g\(tgt_reg\)\}\._u32\[0\]; (\{g\(link_rt\)\} = spu_link\([^;]*\);) '
t2, n = re.subn(pat, r'\1 {{ uint32_t _tg = {g(tgt_reg)}._u32[0]; ', t)
if n != 2: sys.exit(1)
open(p, "w").write(t2)
PY
  what="link-before-target mutation"
fi
if check "$O/neg/tools/spu_lifter.py" "$O/negout" >"$O/neg.txt" 2>&1; then echo "FAIL: negative control passed - test proves nothing"; exit 1; fi
echo "PASS: bisl/bisled lifter test passes; $what fails ($(tr '\n' ' ' <"$O/neg.txt"))"
