#!/usr/bin/env bash
# Host test for KZ3 patch 2191: kzr/core/sync + kzr_platform events (libs/kzr in the patched toolkit).
# Builds tools/smoke/test_pub.c (unity build of the library) with Linux cc, runs it (plain, and under TSan when
# the compiler can), then rebuilds it once per mutant (-DKZR_SYNC_MUTANT_x) and requires the named property to FAIL
# (negative controls). Also compiles with MinGW (-fsyntax-only style full compile) when x86_64-w64-mingw32-gcc exists.
# No game data, no GPU; SKIP without the patched toolkit or patch 2191.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pub.sh
# TSan runs without the 4 GB ulimit (TSan maps terabytes of shadow); the plain runs keep it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
K="$PT/libs/kzr"
[[ -d "$PT/libs/video" ]] || { echo "[test_pub] SKIP: no patched toolkit ($PT)"; exit 0; }
[[ -f "$K/core/sync/kzr_sync.h" ]] || { echo "[test_pub] SKIP: toolkit without patch 2191"; exit 0; }
O="$ROOT/build/smoke-host/pub"; mkdir -p "$O"
CCX="${CC:-cc}"
FL=(-std=gnu11 -Wall -Wextra -Werror -O1 -g -I"$K/include" -I"$K")
SAN=""
if echo 'int main(void){return 0;}' | $CCX -x c -fsanitize=thread -o "$O/tsan_probe" - 2>/dev/null && "$O/tsan_probe" 2>/dev/null; then SAN="-fsanitize=thread -Wno-tsan"; else echo "[test_pub] note: TSan unavailable, plain runs only"; fi
fails=0
if [[ "$($CCX -dumpmachine 2>/dev/null)" == *mingw* ]]; then   # .#win shell: cc is the cross compiler, build only
  if $CCX -std=gnu11 -Wall -Wextra -Werror -O1 -I"$K/include" -I"$K" "$ROOT/tools/smoke/test_pub.c" -o "$O/test_pub.exe" 2>"$O/mingw.log"; then
    for m in "" NOEPOCH NOFENCE PUBLISH PINNED ORDER DROP; do $CCX -std=gnu11 -Wall -Wextra -Werror -O1 ${m:+-DKZR_SYNC_MUTANT_$m} -I"$K/include" -I"$K" "$ROOT/tools/smoke/test_pub.c" -o "$O/test_pub_m.exe" 2>>"$O/mingw.log" || fails=$((fails+1)); done
    [[ $fails -eq 0 ]] && echo "[test_pub] MinGW build ok (plain + 6 mutants; not run: no Windows runtime in this shell)" && exit 0
  fi
  echo "[test_pub] FAIL: MinGW build"; head -8 "$O/mingw.log"; exit 1
fi
build() { $CCX "${FL[@]}" $2 "$ROOT/tools/smoke/test_pub.c" -o "$O/$1" -lpthread || { echo "[test_pub] FAIL: build $1"; exit 1; }; }
run_plain() { ( ulimit -v 4194304 2>/dev/null; "$O/$1" "${@:2}" 2>&1 ); }

build t_plain ""
out="$(run_plain t_plain)" && rc=0 || rc=$?
echo "$out" | grep -E 'done|ALL OK|FAIL' | cut -c1-200
[[ $rc -eq 0 ]] || { echo "[test_pub] FAIL: plain run rc $rc"; fails=$((fails+1)); }
for prop in order idle_reads hol full_log lost_wake dekker sfence; do echo "$out" | grep -q "done $prop" || { echo "[test_pub] FAIL: property $prop did not complete"; fails=$((fails+1)); }; done

if [[ -n "$SAN" ]]; then
  build t_tsan "$SAN"
  tout="$("$O/t_tsan" 2>&1)" && trc=0 || trc=$?
  echo "$tout" | grep -E 'ALL OK|FAIL' | cut -c1-200
  if [[ $trc -ne 0 ]] || echo "$tout" | grep -q 'WARNING: ThreadSanitizer'; then echo "[test_pub] FAIL: TSan run (rc $trc)"; echo "$tout" | grep -m5 -A6 'WARNING: ThreadSanitizer' | cut -c1-160; fails=$((fails+1)); else echo "[test_pub] TSan clean"; fi
fi

# negative controls: mutant -> property that must FAIL
for m in PINNED:idle_reads ORDER:hol DROP:full_log NOEPOCH:lost_wake NOFENCE:dekker PUBLISH:sfence; do
  mut="${m%%:*}"; prop="${m##*:}"
  build "t_$mut" "-DKZR_SYNC_MUTANT_$mut"
  mo="$(run_plain "t_$mut" "$prop")" && mrc=0 || mrc=$?
  if [[ $mrc -ne 0 ]] && echo "$mo" | grep -q "FAIL $prop:"; then echo "[test_pub] control $mut: $prop fails as it must"; else echo "[test_pub] FAIL: mutant $mut did not break $prop (rc $mrc)"; echo "$mo" | tail -3 | cut -c1-160; fails=$((fails+1)); fi
done
if [[ -n "$SAN" ]]; then   # the publish mutant (relaxed store, no fence) must also be a TSan data race on the payload
  build t_tsan_pub "$SAN -DKZR_SYNC_MUTANT_PUBLISH"
  pout="$("$O/t_tsan_pub" sfence 2>&1 || true)"
  if grep -q 'WARNING: ThreadSanitizer: data race' <<<"$pout"; then echo "[test_pub] control PUBLISH: TSan reports the payload race as it must"; else echo "[test_pub] FAIL: PUBLISH mutant is not a TSan race"; fails=$((fails+1)); fi
fi

MG=x86_64-w64-mingw32-gcc
if command -v $MG >/dev/null 2>&1; then
  if $MG -std=gnu11 -Wall -Wextra -Werror -O1 -I"$K/include" -I"$K" "$ROOT/tools/smoke/test_pub.c" -o "$O/test_pub.exe" 2>"$O/mingw.log"; then echo "[test_pub] MinGW build ok"
  else echo "[test_pub] FAIL: MinGW build"; head -5 "$O/mingw.log"; fails=$((fails+1)); fi
else echo "[test_pub] note: no MinGW in this shell (use nix develop .#win for the cross build)"; fi
[[ $fails -eq 0 ]] || { echo "[test_pub] FAIL ($fails)"; exit 1; }
echo "[test_pub] PASS ${SAN:+(tsan)}"
