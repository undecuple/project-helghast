#!/usr/bin/env bash
# Runs every test in tools/host-tests.txt from the repo root (call inside `nix develop .#ci`, after
# tools/ps3recomp-patched.sh). Prints one line per test and a summary; exit 1 if any failed.
#   --gh   wrap each test's output in GitHub Actions log groups (CI)
#   -q     print only failures and the summary
#   -j N   run up to N tests at the same time (default 1; CI passes the core count). Output stays per test and in
#          list order, whatever order the tests finish in. Every test writes only to its own output dir (a mktemp
#          dir or a dir named after the test) and reads the shared toolkit work/ps3recomp-patched without writing it,
#          so tests may run side by side; a new test must keep that rule.
#   Rows also listed in tools/host-tests-linux.txt need a Linux host (compiler, bwrap, /proc, symlinks): on another OS
#   (Git Bash, MSYS, macOS) they are skipped, not failed.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
GH=0; Q=0; J=1
while (( $# )); do
  case "$1" in
    --gh) GH=1;;
    -q) Q=1;;
    -j) shift; J="${1:-}"; [[ "$J" =~ ^[1-9][0-9]*$ ]] || { echo "-j needs a positive number" >&2; exit 2; };;
    -j[1-9]*) J="${1#-j}"; [[ "$J" =~ ^[0-9]+$ ]] || { echo "unknown option $1" >&2; exit 2; };;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
  shift
done

D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
tests=(); missing=()
LINUX_ONLY=" "
case "$(uname -s 2>/dev/null)" in Linux) ;; *)
  [[ -f tools/host-tests-linux.txt ]] && LINUX_ONLY=" $(grep -vE '^\s*(#|$)' tools/host-tests-linux.txt | tr -d '\r' | tr '\n' ' ') ";; esac
# A test listed in tools/host-tests.txt that is not in the tree is a FAILURE, in the public tree too (an export that
# drops a listed test must not pass quietly). Tests that exist only in the private tree are listed in
# tools/host-tests-private.txt instead; the export leaves that list out, and there a missing file is skipped.
add_list() {   # add_list FILE STRICT(1 = a missing test fails)
  local t
  while read -r t; do
    t="${t%$'\r'}"
    [[ -z "$t" || "$t" == \#* ]] && continue
    if [[ ! -e "$t" ]]; then
      if (( $2 )); then missing+=("$t"); echo "FAIL $t (listed in $1 but not in this tree)"
      elif (( ! Q )); then echo "skip $t (private list, file not in this tree)"; fi
      continue
    fi
    if [[ "$LINUX_ONLY" == *" $t "* ]]; then (( Q )) || echo "skip $t (needs a Linux host)"; continue; fi
    tests+=("$t")
  done < "$1"
}
add_list tools/host-tests.txt 1
[[ -f tools/host-tests-private.txt ]] && add_list tools/host-tests-private.txt 0

n=${#tests[@]}; failed=(); printed=0; running=0
# Prints the finished tests that come next in list order (a test's .rc file appears when it is done).
flush() {
  local t rc
  while (( printed < n )) && [[ -e "$D/$printed.rc" ]]; do
    t="${tests[printed]}"; rc="$(<"$D/$printed.rc")"
    if (( GH )); then echo "::group::$t"; cat "$D/$printed.out"; echo "::endgroup::"; fi
    if (( rc )); then
      failed+=("$t")
      if (( ! GH )); then echo "FAIL $t"; { grep -E "^(FAIL|not ok|Error|error:)" "$D/$printed.out" | head -10; tail -5 "$D/$printed.out"; }; fi
    elif (( ! Q && ! GH )); then echo "ok   $t"; fi
    printed=$((printed + 1))
  done
}
for i in "${!tests[@]}"; do
  while (( running >= J )); do wait -n; running=$((running - 1)); flush; done
  t="${tests[i]}"
  case "$t" in *.py) run=(python3 "$t");; *) run=(bash "$t");; esac
  ( "${run[@]}" >"$D/$i.out" 2>&1; echo $? >"$D/$i.rc" ) &
  running=$((running + 1))
done
while (( running > 0 )); do wait -n; running=$((running - 1)); flush; done
flush
echo "host tests: $((n - ${#failed[@]}))/$((n + ${#missing[@]})) pass${failed:+; failed: ${failed[*]}}${missing:+; listed but missing: ${missing[*]}}"
(( ${#failed[@]} == 0 && ${#missing[@]} == 0 ))
