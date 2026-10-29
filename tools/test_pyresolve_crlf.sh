#!/usr/bin/env bash
# Host test (proposal): tools/pyresolve.sh skips a Microsoft Store style python3 and refuses with exit 2 when no Python
# 3 exists; tools/ps3recomp-patched.sh refuses early (exit 2, "autocrlf") on a scratch repo whose worktree line endings
# differ from the stored ones. Needs only git, bash, python3.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT; fail=0
ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fail=1; }
mkdir -p "$T/WindowsApps"; printf '#!/bin/sh\nexit 9\n' > "$T/WindowsApps/python3"; chmod +x "$T/WindowsApps/python3"
r="$(PATH="$T/WindowsApps:$PATH" bash -c 'source tools/pyresolve.sh; echo "$PY"')"
[[ "$r" == python || "$r" == "py -3" ]] && ok "store alias skipped ($r)" || bad "store alias picked ($r)"
BASH_BIN="$(command -v bash)"; PATH=/nonexistent "$BASH_BIN" -c 'source tools/pyresolve.sh' >/dev/null 2>"$T/err"; rc=$?
[[ $rc == 2 ]] && grep -q "no usable Python 3" "$T/err" && ok "no Python: refuses with exit 2" || bad "no Python (rc $rc)"
S="$T/repo"; mkdir -p "$S/third_party/ps3recomp" "$S/port/patches" "$S/tools"
cp tools/ps3recomp-patched.sh tools/pyresolve.sh "$S/tools/"
( cd "$S" && git init -q && git config user.email a@b && git config user.name t && git config core.autocrlf false
  printf 'int a;\n' > third_party/ps3recomp/a.c && git add -A && git -c core.autocrlf=false commit -qm x
  git config core.autocrlf true; printf 'int a;\r\n' > third_party/ps3recomp/a.c )
# the stub submodule is a plain folder here, so ls-files --eol is asked of the outer repo through the same path
out="$(cd "$S" && bash tools/ps3recomp-patched.sh 2>&1)"; rc=$?
[[ $rc == 2 && "$out" == *autocrlf* ]] && ok "CRLF-rewritten tree refused early" || bad "CRLF tree not refused (rc $rc): ${out:0:200}"
exit $fail
