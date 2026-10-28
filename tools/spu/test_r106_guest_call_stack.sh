#!/usr/bin/env bash
# Pass 106: patch 1126 (upstream PR #489) host test. Runs the PR's tools/test_ppu_guest_call_stack.py from the patched
# toolkit (work/ps3recomp-patched, or KZ3_PATCHED_DIR): two host threads inside an HLE -> guest callback at once, each
# nesting a second one; every frame must survive (5 checks). With SAN=1 also an ASan/UBSan build of the same test.
# Self-contained (no game data); SKIP without the patched tree or on a non-POSIX host. Exit 1 on any failure.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/ppu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
[[ -f $T/tools/test_ppu_guest_call_stack.py ]] || { echo "FAIL: test_ppu_guest_call_stack.py missing (patch 1126 not applied?)"; exit 1; }
fail=0
out="$(python3 "$T/tools/test_ppu_guest_call_stack.py" 2>&1)" || fail=1
echo "$(grep -c '^\[PASS\]' <<<"$out") passed, $(grep -c '^\[FAIL\]' <<<"$out") failed: $(tail -1 <<<"$out" | cut -c1-160)"
if [[ ${SAN:-0} == 1 ]]; then
  O="$(mktemp -d)"
  (cd "$T" && python3 -c "import sys; sys.path.insert(0,'tools'); from check_ppu_scaffold import stub_header; open('$O/ppu_recomp.h','w').write(stub_header())" &&
   ${CXX:-c++} -std=c++17 -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
     -Wl,--gc-sections -I"$O" -I include -I runtime/platform runtime/ppu/ppu_loader.cpp \
     runtime/ppu/tests/test_ppu_guest_call_stack.cpp -pthread -ldl -o "$O/t") >"$O/b.log" 2>&1 ||
    { echo "FAIL san build: $(grep -m1 -E 'error|undefined' "$O/b.log" | cut -c1-200)"; fail=1; }
  if [[ -x $O/t ]]; then
    s="$(ASAN_OPTIONS=detect_leaks=0 "$O/t" 2>&1)"; r=$?
    n="$(grep -c 'runtime error\|ERROR: AddressSanitizer' <<<"$s")"
    echo "san: exit $r, $(grep -c '^\[PASS\]' <<<"$s") passed, $n sanitizer reports"
    [[ $r == 0 && $n == 0 ]] || fail=1
  fi
  rm -rf "$O"
fi
# Pass 107, patch 1128: slot reuse. 100 short host threads call ppu_guest_call_stack_release as they finish, then two
# concurrent ones must still get different stacks (fixture tools/spu/test_r107_guest_call_slots.cpp, 2 checks).
# EXPECT_FAIL=1: the tree has 1126 but not 1128 -> the last check must FAIL (the script then exits 0 on that).
O="$(mktemp -d)"
python3 -c "import sys; sys.path.insert(0,'$T/tools'); from check_ppu_scaffold import stub_header; open('$O/ppu_recomp.h','w').write(stub_header())" &&
  ${CXX:-c++} -std=c++17 -O1 -w -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$O" -I"$T/include" -I"$T/runtime/platform" \
    "$T/runtime/ppu/ppu_loader.cpp" "$ROOT/tools/spu/test_r107_guest_call_slots.cpp" -pthread -ldl -o "$O/r107" >"$O/b107.log" 2>&1 ||
  { echo "FAIL r107 build: $(grep -m1 -E 'error|undefined' "$O/b107.log" | cut -c1-200)"; fail=1; }
if [[ -x $O/r107 ]]; then
  s="$(timeout 60 "$O/r107" 2>&1)"; r=$?
  grep -E '^(before|after|\[FAIL\])' <<<"$s" | cut -c1-160
  echo "slot reuse: exit $r, $(grep -c '^\[PASS\]' <<<"$s") passed, $(grep -c '^\[FAIL\]' <<<"$s") failed"
  if [[ ${EXPECT_FAIL:-0} == 1 ]]; then
    [[ $r != 0 ]] && echo "OK: the tree without patch 1128 fails the reuse check as expected" || { echo "FAIL: expected the reuse check to fail"; fail=1; }
  else
    [[ $r == 0 ]] || fail=1
  fi
fi
rm -rf "$O"
exit $fail
