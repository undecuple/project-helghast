#!/usr/bin/env bash
# Host test for KZ3 patch 2198 (S7.4): libs/kzr/core/vres (resident raw vertex streams, pure core) + libs/video/rsx_vtx_res.h.
# Builds tools/smoke/test_vres.c with Linux cc, runs it plain and under ASan+UBSan (when the compiler can), then runs
# every negative control (a test-side TEST_NEG mode, or a mutant: a scratch copy of the shipped headers with an exact-text
# substitution, see mutate() below; the shipped header carries no test-only code) and requires the named property to FAIL. Also compiles with MinGW when x86_64-w64-mingw32-gcc exists (build only).
# No game data, no GPU; SKIP without the patched toolkit or patch 2198.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_vres.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d "$PT/libs/video" ]] || { echo "[test_vres] SKIP: no patched toolkit ($PT)"; exit 0; }
[[ -f "$PT/libs/kzr/core/vres/kzr_vres.h" ]] || { echo "[test_vres] SKIP: toolkit without patch 2198"; exit 0; }
O="$ROOT/build/smoke-host/vres"; mkdir -p "$O"
CCX="${CC:-cc}"
FL=(-std=gnu11 -Wall -Wextra -Werror -O2 -g "-I$PT/libs/video" "-I$PT/include")   # array: the repo path has a space
SRC="$ROOT/tools/smoke/test_vres.c"
fails=0
if [[ "$($CCX -dumpmachine 2>/dev/null)" == *mingw* ]]; then   # .#win shell: cc is the cross compiler, build only
  ok=1
  $CCX "${FL[@]}" "$SRC" -o "$O/t_mingw.exe" 2>>"$O/mingw.log" || ok=0
  [[ $ok -eq 1 ]] && { echo "[test_vres] MinGW build ok (not run: no Windows runtime in this shell)"; exit 0; }
  echo "[test_vres] FAIL: MinGW build"; head -8 "$O/mingw.log"; exit 1
fi
build() { $CCX "${FL[@]}" $2 "$SRC" -o "$O/$1" || { echo "[test_vres] FAIL: build $1"; exit 1; }; }
# mutate NAME: scratch copy of libs/video/*.h + libs/kzr/core/vres with one exact-text substitution set; prints its libs/video dir
mutate() {
  local d="$O/mut_$1"; rm -rf "$d"; mkdir -p "$d/libs/video" "$d/libs/kzr/core/vres"
  cp "$PT"/libs/video/*.h "$d/libs/video/"; cp "$PT"/libs/kzr/core/vres/*.h "$d/libs/kzr/core/vres/"
  python3 -I - "$1" "$d/libs/kzr/core/vres/kzr_vres.h" <<'PY' || return 1
import re, sys
name, path = sys.argv[1:]
s = open(path).read()
def sub(a, b, rx=False):
    global s
    n = len(re.findall(a, s, re.S)) if rx else s.count(a)
    assert n == 1, (name, a, n)
    s = re.sub(a, b, s, flags=re.S) if rx else s.replace(a, b)
if name == 'FIRST_FIT':   # the plain first-fit clock over every block: no spare FIFO scan, spare blocks not skipped, retired blocks not queued
    sub(r"    const uint32_t ns = s->spare_n.*?\n    \}\n(?=    const uint32_t nc)", "", rx=True)
    sub("        if (b->page >= KZR_VRES_RETIRED) continue;   /* spare blocks are taken from the FIFO only */\n", "")
    sub("                kzr_vres_spare_push(s, (uint32_t)(b - s->blocks));\n", "")
elif name == 'COPYSEQ':   # a block with a pending page copy may be taken
    sub("    if (b->copy_seq > s->done_seq) return 0;\n", "")
elif name == 'ARM_AFTER':   # arm after the read
    a = "            s->wg.arm(s->wg.user, &b->arm, ph);   /* after the collect, before the bytes are read */\n"
    c = "            b->copy_seq = s->copy(s->user, i, (const uint8_t*)ph);\n"
    sub(a + c, c + a)
elif name == 'ACQUIRE_NOOP':   # the acquire neither ends the epoch nor holds
    sub("    kzr_vres_epoch(s);\n    if (met) return;", "    (void)ea; (void)want; if (1) return;\n    if (met) return;")
elif name == 'MINAGE':   # no min_age margin
    sub("s->min_age = 2;", "s->min_age = 0;")
elif name == 'ZEROBLK':   # no n_blocks == 0 guards
    sub("    if (!n_blocks) return;   /* a pool", "    /* a pool")
    sub("    if (!s->n_blocks) return;   /* no FIFO", "    /* no FIFO")
    sub("    if (!s->n_blocks) { s->alloc_fail++; return KZR_VRES_NONE; }\n", "")
else:
    sys.exit('unknown mutant ' + name)
open(path, 'w').write(s)
PY
  MUT_VIDEO="$d/libs/video"
}
build t_plain ""
out="$("$O/t_plain" 2>&1)" && rc=0 || rc=$?
echo "$out" | grep -E '^(decode|acquire_epoch|stress|PASS|FAIL)' | cut -c1-300
if [[ $rc -ne 0 ]] || ! echo "$out" | grep -q '^PASS'; then echo "[test_vres] FAIL: plain run rc $rc"; fails=$((fails+1)); fi
if TEST_VERIFY=1 "$O/t_plain" >"$O/verify.out" 2>&1 && grep -q '^PASS' "$O/verify.out"; then
  echo "[test_vres] verify run PASS"
else echo "[test_vres] FAIL: verify run"; tail -3 "$O/verify.out"; fails=$((fails+1)); fi
if echo 'int main(void){return 0;}' | $CCX -x c -fsanitize=address,undefined -o "$O/san_probe" - 2>/dev/null && "$O/san_probe" 2>/dev/null; then
  build t_san "-fsanitize=address,undefined -fno-sanitize-recover=undefined"
  sout="$("$O/t_san" 2>&1)" && src=0 || src=$?
  if [[ $src -eq 0 ]] && echo "$sout" | grep -q '^PASS'; then echo "[test_vres] ASan+UBSan clean"
  else echo "[test_vres] FAIL: sanitizer run (rc $src)"; echo "$sout" | head -5 | cut -c1-200; fails=$((fails+1)); fi
else echo "[test_vres] note: sanitizers unavailable, plain runs only"; fi

# negative controls: env (TEST_NEG[+TEST_VERIFY]) or mutant -> property that must FAIL (and one that must not)
ctl() {   # name, extra env, build flag, property, [property that must NOT fail]
  local name="$1" envs="$2" flag="$3" prop="$4" notprop="${5:-}" bin="$O/t_plain" mo mrc
  if [[ -n "$flag" ]]; then
    mutate "$flag" || { echo "[test_vres] FAIL: control $name: mutation did not apply"; fails=$((fails+1)); return; }
    $CCX "-I$MUT_VIDEO" "${FL[@]}" "$SRC" -o "$O/t_$name" 2>"$O/t_$name.log" || { echo "[test_vres] FAIL: build mutant $name"; head -5 "$O/t_$name.log"; fails=$((fails+1)); return; }
    bin="$O/t_$name"
  fi
  mo="$(env $envs "$bin" 2>&1)" && mrc=0 || mrc=$?
  if [[ $mrc -ne 0 ]] && echo "$mo" | grep -q "^FAIL $prop:" && { [[ -z "$notprop" ]] || ! echo "$mo" | grep -q "^FAIL $notprop:"; }; then
    echo "[test_vres] control $name: $prop fails as it must"
  else
    echo "[test_vres] FAIL: control $name did not break $prop (rc $mrc)"; echo "$mo" | grep -E '^(FAIL|PASS)' | head -4 | cut -c1-200; fails=$((fails+1))
  fi
}
ctl blind_watch       "TEST_NEG=1"              ""                                stale
ctl ignored_fence     "TEST_NEG=2"              ""                                overwrite
ctl writes_in_epoch   "TEST_NEG=3"              ""                                stale
ctl writes_in_epoch_v "TEST_NEG=3 TEST_VERIFY=1" ""                               verify_bad stale
ctl map_stamp_only    "TEST_NEG=4"              ""                                stamp_record
ctl no_hold           "TEST_NEG=5"              ""                                hold
ctl naive_clean       "TEST_NEG=6"              ""                                shared_reset
ctl first_fit         ""                        "FIRST_FIT"                        evict_current
ctl copyseq_ignored   ""                        "COPYSEQ"                         pending_copy
ctl arm_after_read    ""                        "ARM_AFTER"                       arm_order
ctl acquire_noop      ""                        "ACQUIRE_NOOP"                    acquire_epoch
ctl min_age_zero        ""                        "MINAGE"                          min_age
ctl zero_blocks_noguard ""                        "ZEROBLK"                         zero_blocks

MG=x86_64-w64-mingw32-gcc
if command -v $MG >/dev/null 2>&1; then
  if $MG "${FL[@]}" "$SRC" -o "$O/test_vres.exe" 2>"$O/mingw.log"; then echo "[test_vres] MinGW build ok"
  else echo "[test_vres] FAIL: MinGW build"; head -5 "$O/mingw.log"; fails=$((fails+1)); fi
else echo "[test_vres] note: no MinGW in this shell (use nix develop .#win for the cross build)"; fi
[[ $fails -eq 0 ]] || { echo "[test_vres] FAIL ($fails)"; exit 1; }
echo "[test_vres] PASS"
