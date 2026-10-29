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
tests=()
LINUX_ONLY=" "
case "$(uname -s 2>/dev/null)" in Linux) ;; *)
  [[ -f tools/host-tests-linux.txt ]] && LINUX_ONLY=" $(grep -vE '^\s*(#|$)' tools/host-tests-linux.txt | tr -d '\r' | tr '\n' ' ') ";; esac
while read -r t; do
  [[ -z "$t" || "$t" == \#* ]] && continue
  # A test whose file is not in this tree is skipped when the developer-only tools are absent (a developer-only tool is the
  # marker); in a full developer checkout a missing test is a failure.
  if [[ ! -e "$t" && ! -e tools/agents/merge-lane.sh ]]; then
    (( Q )) || echo "skip $t (private tooling, not exported)"; continue
  fi
  if [[ "$LINUX_ONLY" == *" $t "* ]]; then (( Q )) || echo "skip $t (needs a Linux host)"; continue; fi
  tests+=("$t")
done < tools/host-tests.txt

n=${#tests[@]}; failed=(); printed=0; running=0
# Prints the finished tests that come next in list order (a test's .rc file appears when it is done).
flush() {
  local t rc
  while (( printed < n )) && [[ -e "$D/$printed.rc" ]]; do
    t="${tests[printed]}"; rc="$(<"$D/$printed.rc")"
    if (( GH )); then echo "::group::$t"; cat "$D/$printed.out"; echo "::endgroup::"; fi
    if (( rc )); then
      failed+=("$t")
      if (( ! GH )); then echo "FAIL $t"; tail -5 "$D/$printed.out"; fi
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
echo "host tests: $((n - ${#failed[@]}))/$n pass${failed:+; failed: ${failed[*]}}"
(( ${#failed[@]} == 0 ))
