# Shared by test_r159_native_k7a.sh and test_r159_native_k3.sh (SPU pass 159): builds tools/spu/test_r159_snd_model.c with
# port/spu/kz3_native_snd.c twice (-O1 and -O2), runs the independent model, the negative control, then native_diff.py
# against the lifted code (game data only). Args: KIND(k7a|k3) SLOT TWINSLOT.
r159_run() {
    local kind=$1 slot=$2 twin=$3
    ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    local T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
    [[ -f $T/runtime/spu/spu_context.h ]] || { echo "SKIP: no patched toolkit at $T"; echo "RESULT: SKIP"; return 0; }
    OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
    local SRC=("$ROOT/tools/spu/test_r159_snd_model.c" "$ROOT/port/spu/kz3_native_snd.c" "$ROOT/tools/spu/native_xr_stubs.c")
    local INC=(-I"$ROOT/port/spu" -I"$T/runtime/spu" -I"$T/include" -I"$T/runtime")
    local BASE=(-std=gnu17 -w -fno-strict-aliasing -ffp-contract=off) fail=0
    build() { local o=$1; shift; cc "${BASE[@]}" "$@" "${INC[@]}" "${SRC[@]}" -lm -o "$OUT/$o" || { echo "FAIL build $o"; echo "RESULT: FAIL"; exit 1; }; }
    build t1 -O1 -msse4.1
    for m in default ftz; do "$OUT/t1" $kind $m > "$OUT/t1.$m" 2>&1 && echo "PASS model -O1 ($m): $(tail -1 "$OUT/t1.$m")" || { echo "FAIL model -O1 ($m): $(tail -3 "$OUT/t1.$m")"; fail=1; }; done
    if grep -qw fma /proc/cpuinfo 2>/dev/null; then
        build t2 -O2 -msse4.1 -mfma -DKZ3_SPU_NEEDS_FMA
        for m in default ftz; do "$OUT/t2" $kind $m > "$OUT/t2.$m" 2>&1 && echo "PASS model -O2 -mfma ($m): $(tail -1 "$OUT/t2.$m")" || { echo "FAIL model -O2 ($m): $(tail -3 "$OUT/t2.$m")"; fail=1; }; done
        build tn -O2 -msse4.1 -mfma -DKZ3_SPU_NEEDS_FMA -DKZ3_NATIVE_NEGCTL
    else
        echo "SKIP -mfma builds (no FMA on this CPU)"; build tn -O1 -msse4.1 -DKZ3_NATIVE_NEGCTL
    fi
    if "$OUT/tn" $kind > "$OUT/neg.log" 2>&1; then echo "FAIL negative control not caught"; fail=1; else echo "PASS negative control caught ($(tail -1 "$OUT/neg.log"))"; fi
    if [[ -f $ROOT/work/spu/spu_lift.json && -f $ROOT/work/spu/lift/job_0125e300/spu_recomp.c ]]; then
        local G="-O2 -mfma -DKZ3_SPU_NEEDS_FMA" v out fl ftz
        for v in "|" "$G|" "$G|1"; do
            fl=${v%|*}; ftz=${v#*|}
            out=$(cd "$ROOT" && NATIVE_DIFF_CFLAGS="$fl" HARNESS_FTZ="$ftz" python3 tools/spu/native_diff.py --slot $slot --trials 3000 2>&1 | tail -3)
            echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot $slot [${fl:--O1}${ftz:+ FTZ}]: $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot $slot [$fl $ftz]: $out"; fail=1; }
        done
        out=$(cd "$ROOT" && NATIVE_DIFF_CFLAGS="$G -DKZ3_NATIVE_NEGCTL" python3 tools/spu/native_diff.py --slot $slot --trials 600 2>&1 | tail -2)
        echo "$out" | grep -q '^FAIL' && echo "PASS native_diff negative control FAILs as it must ($(echo "$out" | grep -o 'equal=[0-9]* differ=[0-9]*'))" || { echo "FAIL native_diff negative control passed: $out"; fail=1; }
        if [[ -f $ROOT/work/spu-100/spu_lift.json && -f $ROOT/work/spu-100/lift/job_011e1180/spu_recomp.c ]]; then
            out=$(cd "$ROOT" && KZ3_EBOOT_VER=100 NATIVE_DIFF_CFLAGS="$G" python3 tools/spu/native_diff.py --slot $twin --trials 3000 --ps3recomp "$T" 2>&1 | tail -3)
            echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot $twin (1.00 twin): $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot $twin: $out"; fail=1; }
        else echo "SKIP native_diff slot $twin: no 1.00 lift"; fi
    else echo "SKIP native_diff (no game data / lift)"; fi
    [[ $fail == 0 ]] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; return 1; }
}
