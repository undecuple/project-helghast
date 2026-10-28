#!/usr/bin/env bash
# Runs every test in tools/host-tests.txt from the repo root (call inside `nix develop .#ci`, after
# tools/ps3recomp-patched.sh). Prints one line per test and a summary; exit 1 if any failed.
#   --gh   wrap each test's output in GitHub Actions log groups (CI)
#   -q     print only failures and the summary
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
GH=0; Q=0
for a in "$@"; do case "$a" in --gh) GH=1;; -q) Q=1;; *) echo "unknown option $a" >&2; exit 2;; esac; done
n=0; failed=()
while read -r t; do
  [[ -z "$t" || "$t" == \#* ]] && continue
  # The public export leaves out private tooling (tools/agents/), so its tests are skipped there; in the private repo
  # a missing test is still a failure (2026-10-11: test_merge_tooling.py turned the public CI red).
  # tools/remote/ is private too (2026-10-11); the export keeps tools/agents/lock.sh, so the marker is merge-lane.sh.
  # Since 2026-10-13 any test the export leaves out is skipped there (export-public.sh EXCLUDE: test_run_stage.sh tests
  # the private tools/spu/run.sh), not only tools/agents/ and tools/remote/.
  if [[ ! -e "$t" && ! -e tools/agents/merge-lane.sh ]]; then
    (( Q )) || echo "skip $t (private tooling, not exported)"; continue
  fi
  n=$((n + 1))
  case "$t" in *.py) run=(python3 "$t");; *) run=(bash "$t");; esac
  if (( GH )); then echo "::group::$t"; "${run[@]}"; rc=$?; echo "::endgroup::"
  else out="$("${run[@]}" 2>&1)"; rc=$?; fi
  if (( rc )); then failed+=("$t"); (( GH )) || { echo "FAIL $t"; tail -5 <<<"$out"; }
  elif (( ! Q && ! GH )); then echo "ok   $t"; fi
done < tools/host-tests.txt
echo "host tests: $((n - ${#failed[@]}))/$n pass${failed:+; failed: ${failed[*]}}"
(( ${#failed[@]} == 0 ))
