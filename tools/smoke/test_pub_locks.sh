#!/usr/bin/env bash
# Host test for KZ3 patch 2197: the ordered publisher P (libs/video/rsx_pub.c over libs/kzr) with real threads, and the lock-order test
# (C7 / H7, tools/smoke/test_pub_locks.c). Builds the unity test with Linux cc, runs it plain and under TSan when the compiler can, then rebuilds it once
# per mutant (-DRSX_PUB_MUTANT_x) and requires the named property to FAIL (negative controls):
#   NOBARRIER -> barrier (a label held back by the lag)         NOLAG -> lag (the lag ignored)         NODRAIN -> drain (the pass end does not drain)
#   TOUCH -> touch (the pending-write touch retires nothing)    OWNER -> owner (no thread rule)       MOVIE -> movie (the log-empty check never fires)
#   VAL / REC -> verify (a wrong stored word / a truncated record value)                               LOCK -> locks (a quiesce under the fifo lock: ABBA)
# Also compiles rsx_pub.c with -Wall -Wextra -Werror and, when x86_64-w64-mingw32-gcc exists, with MinGW (compile only: no Windows runtime here).
# No game data, no GPU; FAIL without the patched toolkit or patch 2197 unless KZ3_ALLOW_SKIP=1 (then SKIP).
#   nix develop .#re --command tools/smoke/test_pub_locks.sh
# TSan runs without the 4 GB ulimit (TSan maps terabytes of shadow); the plain runs keep it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"; K="$PT/libs/kzr"
# a missing toolkit is a FAIL (the test proves nothing), unless KZ3_ALLOW_SKIP=1; the final line then reads SKIP
nokit() { if [[ "${KZ3_ALLOW_SKIP:-0}" == 1 ]]; then echo "[test_pub_locks] SKIP: $1 (KZ3_ALLOW_SKIP=1)"; exit 0; fi; echo "[test_pub_locks] FAIL: $1 (run tools/ps3recomp-patched.sh, or KZ3_ALLOW_SKIP=1 to skip)"; exit 1; }
[[ -d "$V" ]] || nokit "no patched toolkit ($PT)"
[[ -f "$V/rsx_pub.c" ]] || nokit "toolkit without patch 2197"
O="$ROOT/build/smoke-host/pub-locks"; mkdir -p "$O"
CCX="${CC:-cc}"
INC=(-I"$V" -I"$K/include" -I"$K")
FL=(-std=gnu11 -Wall -Wextra -O1 -g)
SAN=""
if echo 'int main(void){return 0;}' | $CCX -x c -fsanitize=thread -o "$O/tsan_probe" - 2>/dev/null && "$O/tsan_probe" 2>/dev/null; then SAN="-fsanitize=thread"; else echo "[test_pub_locks] note: TSan unavailable, plain runs only"; fi
fails=0
build() { $CCX "${FL[@]}" "${@:2}" "${INC[@]}" "$ROOT/tools/smoke/test_pub_locks.c" -o "$O/$1" -lpthread 2>"$O/$1.log" || { echo "[test_pub_locks] FAIL: build $1"; head -8 "$O/$1.log"; exit 1; }; }
run_plain() { ( ulimit -v 4194304 2>/dev/null; timeout 120 "$O/$1" "${@:2}" 2>&1 ); }

# the library file itself, warnings as errors
if ! $CCX -std=gnu11 -Wall -Wextra -Werror -O1 -c "${INC[@]}" "$V/rsx_pub.c" -o "$O/rsx_pub.o" 2>"$O/pub_c.log"; then echo "[test_pub_locks] FAIL: rsx_pub.c warnings"; head -6 "$O/pub_c.log"; fails=$((fails+1)); fi

build t_plain -Werror
out="$(run_plain t_plain)" && rc=0 || rc=$?
echo "$out" | grep -E 'ALL OK|FAIL' | cut -c1-200
[[ $rc -eq 0 ]] || { echo "[test_pub_locks] FAIL: plain run rc $rc"; fails=$((fails+1)); }
for prop in parse lag barrier drain touch verify owner movie counters locks; do echo "$out" | grep -q "done $prop" || { echo "[test_pub_locks] FAIL: property $prop did not complete"; fails=$((fails+1)); }; done

if [[ -n "$SAN" ]]; then
  build t_tsan $SAN -Wno-tsan
  tout="$("$O/t_tsan" 2>&1)" && trc=0 || trc=$?
  echo "$tout" | grep -E 'ALL OK|FAIL' | cut -c1-200
  if [[ $trc -ne 0 ]] || echo "$tout" | grep -q 'WARNING: ThreadSanitizer'; then echo "[test_pub_locks] FAIL: TSan run (rc $trc)"; echo "$tout" | grep -m5 -A8 'WARNING: ThreadSanitizer' | cut -c1-160; fails=$((fails+1)); else echo "[test_pub_locks] TSan clean (the log-empty check, the counters and the lock-order threads)"; fi
fi

# negative controls: mutant -> property that must FAIL
for m in NOBARRIER:barrier NOLAG:lag NODRAIN:drain TOUCH:touch OWNER:owner MOVIE:movie VAL:verify REC:verify LOCK:locks; do
  mut="${m%%:*}"; prop="${m##*:}"
  build "t_$mut" "-DRSX_PUB_MUTANT_$mut"
  mo="$(run_plain "t_$mut" "$prop")" && mrc=0 || mrc=$?
  if [[ $mrc -ne 0 ]] && echo "$mo" | grep -q "FAIL $prop:"; then echo "[test_pub_locks] control $mut: $prop fails as it must ($(echo "$mo" | grep -m1 "FAIL $prop:" | cut -c1-110))"; else echo "[test_pub_locks] FAIL: mutant $mut did not break $prop (rc $mrc)"; echo "$mo" | tail -3 | cut -c1-160; fails=$((fails+1)); fi
done

MG=x86_64-w64-mingw32-gcc
if command -v $MG >/dev/null 2>&1; then
  if $MG -std=gnu11 -Wall -Wextra -Werror -O1 -c "${INC[@]}" "$V/rsx_pub.c" -o "$O/rsx_pub_mg.o" 2>"$O/mingw.log"; then echo "[test_pub_locks] MinGW compile of rsx_pub.c ok"
  else echo "[test_pub_locks] FAIL: MinGW compile"; head -5 "$O/mingw.log"; fails=$((fails+1)); fi
else echo "[test_pub_locks] note: no MinGW in this shell (use nix develop .#win for the cross compile)"; fi
[[ $fails -eq 0 ]] || { echo "[test_pub_locks] FAIL ($fails)"; exit 1; }
echo "[test_pub_locks] PASS ${SAN:+(tsan)}"
