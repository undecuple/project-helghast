#!/usr/bin/env bash
# Pass 127: Gfx.LightProbes native gates are default on for the 1.14 job (slots 8 9 12 13 16), opt-in for the 1.00 twins
# (10 11 14 15 17); KZ3_NATIVE_JOB_LIGHTPROBES=0 or a single _LP_*=0 turns them off, a single =1 beats LIGHTPROBES=0; every
# non-LightProbes gate stays off. Also checks that re/spu/native_gates.tsv "default" column equals the C table. SKIP
# without the patched toolkit.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_drain.c ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
CC="${CC:-cc}"; S="$ROOT/tools/spu"
$CC -std=gnu17 -O1 -w -msse4.1 -ffp-contract=off -I "$T/runtime/spu" -I "$T/include" -I "$T/runtime" -I "$ROOT/port/spu" -I "$S" \
  -o "$OUT/p" "$S/native_default_probe.c" "$ROOT/port/spu/kz3_native_jobs.c" "$ROOT/port/spu/kz3_native_post.c" \
  "$ROOT/port/spu/kz3_native_dof.c" "$ROOT/port/spu/kz3_native_lp.c" "$S/native_xr_stubs.c" -lm >"$OUT/b.log" 2>&1 ||
  { echo "FAIL build: $(grep -m3 -E 'error|undefined' "$OUT/b.log" | cut -c1-200)"; exit 1; }
fail=0
chk() {  # name expected-lines... ; env passed via $ENVS
  local name=$1 want=$2
  local got; got=$(env -u KZ3_NATIVE_JOB_LIGHTPROBES -u KZ3_NATIVE_JOB_LP_KD -u KZ3_NATIVE_JOB_LP_LIGHT -u KZ3_NATIVE_JOB_POST $ENVS "$OUT/p" 2>/dev/null | tr '\n' ' ')
  [[ "$got" == "$want" ]] && echo "PASS $name" || { echo "FAIL $name: got '$got' want '$want'"; fail=1; }
}
ON114="slot 8 native slot 9 native slot 10 lifted slot 11 lifted slot 12 native slot 13 native slot 14 lifted slot 15 lifted slot 16 native slot 17 lifted other native 0 "
ENVS="" chk "no env: 1.14 on, 1.00 off, others off" "$ON114"
ENVS="KZ3_NATIVE_JOB_LIGHTPROBES=0" chk "LIGHTPROBES=0: all off" "slot 8 lifted slot 9 lifted slot 10 lifted slot 11 lifted slot 12 lifted slot 13 lifted slot 14 lifted slot 15 lifted slot 16 lifted slot 17 lifted other native 0 "
ENVS="KZ3_NATIVE_JOB_LP_KD=0" chk "LP_KD=0: only kd (slot 8) off" "${ON114/slot 8 native/slot 8 lifted}"
ENVS="KZ3_NATIVE_JOB_LP_LIGHT=0" chk "LP_LIGHT=0: only lights (16) off" "${ON114/slot 16 native/slot 16 lifted}"
ENVS="KZ3_NATIVE_JOB_LIGHTPROBES=1" chk "LIGHTPROBES=1: all ten on" "slot 8 native slot 9 native slot 10 native slot 11 native slot 12 native slot 13 native slot 14 native slot 15 native slot 16 native slot 17 native other native 0 "
ENVS="KZ3_NATIVE_JOB_LIGHTPROBES=0 KZ3_NATIVE_JOB_LP_KD=1" chk "LIGHTPROBES=0 + LP_KD=1: only slots 8 and 10" "slot 8 native slot 9 lifted slot 10 native slot 11 lifted slot 12 lifted slot 13 lifted slot 14 lifted slot 15 lifted slot 16 lifted slot 17 lifted other native 0 "
ENVS="KZ3_NATIVE_JOB_POST=1" chk "POST=1 turns on the Post gates only (8 of them), LightProbes unchanged" "${ON114/other native 0/other native 8}"
# tsv default column == C table
tsvon=$(awk -F'\t' '!/^#/ && NF>=7 && $7==1 {print $3}' "$ROOT/re/spu/native_gates.tsv" | sort -n | tr '\n' ' ')
[[ "$tsvon" == "8 9 12 13 16 " ]] && echo "PASS tsv default column ($tsvon)" || { echo "FAIL tsv default column: '$tsvon'"; fail=1; }
[[ $fail -eq 0 ]] && echo PASS || { echo FAIL; exit 1; }
