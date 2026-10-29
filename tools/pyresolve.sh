# shellcheck shell=bash
# Sourced by tools/ps3recomp-patched.sh and tools/build-kz3.sh: sets PY to a Python 3 that really runs.
# Order: python3, then `py -3` (the Windows launcher), then python. A candidate counts only if it prints version 3 and is
# not the Microsoft Store alias (a WindowsApps stub that opens the Store and exits non-zero or prints nothing); on
# Git Bash that stub is what `python3` resolves to when no real Python is installed (issue #9 of the sibling port
# project-pyrrhus, reported by feanor-noldorin). KZ3_PYTHON overrides. See
# docs/in-progress/reports/release.md r71.
kz3_pick_python() {
  local c p
  # shellcheck disable=SC2034
  if [[ -n "${KZ3_PYTHON:-}" ]]; then PY="$KZ3_PYTHON"; return 0; fi
  for c in python3 "py -3" python; do
    p="$(command -v "${c%% *}" 2>/dev/null)" || continue
    case "$p" in *[Ww]indows[Aa]pps*) continue;; esac
    [[ "$(${c} -c 'import sys; print(sys.version_info[0])' 2>/dev/null | tr -d '\r')" == 3 ]] || continue
    # shellcheck disable=SC2034  # PY is read by the sourcing script
    PY="$c"; return 0
  done
  echo "no usable Python 3 found (python3, py -3 and python were tried; the Microsoft Store alias does not count). Install Python 3 from python.org or set KZ3_PYTHON." >&2
  return 1
}
kz3_pick_python || exit 2
