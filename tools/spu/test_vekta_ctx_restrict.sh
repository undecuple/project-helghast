#!/usr/bin/env bash
# Host test for patch 1165 (KZ3_PPU_CTX_RESTRICT, lift time, default OFF): lifts synthetic functions (plain, host-overridden,
# setjmp-calling, longjmp) with the patched ppu_lifter.py three ways and checks
#   1. unset, "0" and "" give byte-identical output (the default lift does not change), with no RESTRICT text anywhere;
#   2. "1" differs, and removing exactly the header block and the "PPU_CTX_RESTRICT " token from it gives the OFF output back
#      (the switch changes nothing else), setjmp/longjmp functions stay unqualified, the others carry it;
#   3. codegen: a tiny C function that stores to guest memory between ctx field uses is compiled with and without
#      -DPPU_CTX_RESTRICT=__restrict (gcc -O2): prints the load counts (the reloads the qualifier removes).
# NEGATIVE CONTROL: a lifter forced always-on must FAIL check 1.
# Self-contained (patched toolkit only). SKIP without it or without patch 1165.
#   nix develop .#ci --command bash -c 'nice -n 19 tools/spu/test_vekta_ctx_restrict.sh'
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f "$P/tools/ppu_lifter.py" ]] || { echo "SKIP: no patched toolkit"; exit 77; }
grep -q "KZ3_PPU_CTX_RESTRICT" "$P/tools/ppu_lifter.py" || { echo "SKIP: patch 1165 not in $P"; exit 77; }
O="$ROOT/work/spu/t_ctxr"; "$ROOT/tools/safe_rm.sh" "$O" 2>/dev/null; mkdir -p "$O/neg/tools"
cat > "$O/lift.py" <<'PY'
import os, sys
sys.path.insert(0, sys.argv[1])
from ppu_lifter import LiftedFunction, PPULifter
out = sys.argv[2]
lf = PPULifter()
def add(a, body, **kw):
    lf.functions.append(LiftedFunction(name=f"func_{a:08X}", start_addr=a, end_addr=a + 0x40, body_lines=body + ["return;"], **kw))
add(0x10000, ["ctx->gpr[3] = 1;", "ctx->fpr[1] = ctx->fpr[2] + 1.0;"])
add(0x10040, ["ctx->gpr[3] = 2;"])                       # host-overridden
add(0x10080, ["PPU_SJ_CALL(func_00010000);"], uses_setjmp=True)
add(0x100C0, ["ctx->gpr[3] = 3;"])                       # longjmp
lf.overrides.add(0x10040); lf.longjmp_marks.add(0x100C0)
open(os.path.join(out, "h.h"), "w").write(lf.emit_header())
lf.write_source_files(out, base="ppu_recomp", chunks=2)
PY
lift() {  # lift ENV_VALUE|- LIFTERDIR OUT
  mkdir -p "$3"; if [[ "$1" == "-" ]]; then env -u KZ3_PPU_CTX_RESTRICT python3 "$O/lift.py" "$2" "$3" >/dev/null 2>"$3/err" ; else KZ3_PPU_CTX_RESTRICT="$1" python3 "$O/lift.py" "$2" "$3" >/dev/null 2>"$3/err"; fi || { echo "lift failed: $(head -3 "$3/err")"; return 1; }
}
rc=0
check1() {  # LIFTERDIR TAG -> 0 when unset == 0 == empty and no RESTRICT text
  local L="$1" t="$2"
  lift - "$L" "$O/$t/u" && lift 0 "$L" "$O/$t/z" && lift "" "$L" "$O/$t/e" || return 2
  diff -r "$O/$t/u" "$O/$t/z" >/dev/null && diff -r "$O/$t/u" "$O/$t/e" >/dev/null || { echo "  unset/0/empty differ"; return 1; }
  ! grep -rq "RESTRICT" "$O/$t/u" || { echo "  RESTRICT text in the default lift"; return 1; }
}
check1 "$P/tools" a || { echo "FAIL: default lift changed"; rc=1; }
[[ $rc == 0 ]] && echo "ok: unset/0/empty lifts are byte-identical and carry no RESTRICT text"
lift 1 "$P/tools" "$O/on" || rc=1
python3 - "$O" <<'PY' || rc=1
import os, re, sys
O = sys.argv[1]
def read(d): return {f: open(os.path.join(d, f)).read() for f in sorted(os.listdir(d)) if f != "err"}
off, on = read(O + "/a/u"), read(O + "/on")
assert off.keys() == on.keys(), "file sets differ"
assert on != off, "FAIL: switch 1 changed nothing"
n_tok = 0
for f in off:
    t = on[f]
    n_tok += t.count("PPU_CTX_RESTRICT ctx")
    t = re.sub(r"\n/\* PPU_CTX_RESTRICT: .*?#endif\n", "", t, flags=re.S) if f.endswith(".h") else t
    t = t.replace("PPU_CTX_RESTRICT ctx", "ctx")
    assert t == off[f], f"FAIL: {f}: on minus the qualifier differs from off"
src = "".join(v for k, v in on.items() if k.endswith(".cpp"))
assert "ppu_context* PPU_CTX_RESTRICT ctx) {" in src and n_tok >= 2, "FAIL: plain/overridden functions not qualified"
for nm in ("func_00010080", "func_000100C0__ljbody", "func_000100C0("):
    for l in src.splitlines():
        if nm in l and "(ppu_context*" in l and l.rstrip().endswith("{"): assert "RESTRICT" not in l, "FAIL: setjmp/longjmp function qualified: " + l
print(f"ok: switch 1 = off output + header block + {n_tok} qualified signatures, setjmp/longjmp functions untouched")
PY
# negative control: an always-on lifter must fail check 1
cp "$P/tools/"*.py "$O/neg/tools/" 2>/dev/null
sed -i 's/^_CTX_RESTRICT = .*/_CTX_RESTRICT = True/' "$O/neg/tools/ppu_lifter.py"
if check1 "$O/neg/tools" n >/dev/null 2>&1; then echo "FAIL: negative control (always-on lifter) passed"; rc=1; else echo "ok: negative control (always-on lifter) FAILED check 1 as it must"; fi
# codegen demo
cat > "$O/cg.c" <<'C'
#include <stdint.h>
typedef struct { uint64_t gpr[32]; double fpr[32]; uint64_t lr; } ctx_t;
#ifndef PPU_CTX_RESTRICT
#define PPU_CTX_RESTRICT
#endif
extern uint8_t* g_mem;
void f(ctx_t* PPU_CTX_RESTRICT ctx) {
    g_mem[ctx->gpr[3]] = 1; ctx->fpr[1] += ctx->fpr[2];
    g_mem[ctx->gpr[3] + 1] = 2; ctx->fpr[1] += ctx->fpr[2];
    g_mem[ctx->gpr[3] + 2] = 3; ctx->fpr[1] += ctx->fpr[2];
    g_mem[ctx->gpr[3] + 3] = 4; ctx->fpr[3] = ctx->fpr[1] + ctx->lr;
}
C
if command -v gcc >/dev/null; then
  a="$(gcc -O2 -fno-strict-aliasing -S -o - "$O/cg.c" | grep -c 'mov.*(%r')"; b="$(gcc -O2 -fno-strict-aliasing -DPPU_CTX_RESTRICT=__restrict -S -o - "$O/cg.c" | grep -c 'mov.*(%r')"
  echo "info: memory-operand moves in a 4-store demo, plain / __restrict: $a / $b"
fi
[[ $rc == 0 ]] && echo "PASS test_vekta_ctx_restrict" || echo "FAIL test_vekta_ctx_restrict"
exit $rc
