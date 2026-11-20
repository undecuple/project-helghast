#!/usr/bin/env bash
# Guarded recursive delete. Only removes paths inside $KZ3_ROOT/work or $KZ3_ROOT/build.
#   tools/safe_rm.sh <path>...
# A path that is itself a symlink is unlinked (the link only, never its target: a copy of this script that resolved
# links with readlink -f deleted a link's target). A path that passes through a symlinked
# folder is refused, so a link can never steer a delete somewhere else.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
for p in "$@"; do
  : "${p:?empty path}"
  lex="$(realpath -s -m -- "$p")"            # absolute, links NOT resolved
  parent_lex="$(dirname -- "$lex")"
  parent_real="$(readlink -f -- "$parent_lex")" || parent_real=""
  if [[ "$parent_real" != "$parent_lex" ]]; then
    echo "safe_rm: refusing '$p' (its folder '$parent_lex' goes through a symlink to '$parent_real')" >&2; exit 1
  fi
  case "$lex" in
    "$ROOT/work/"?*|"$ROOT/build/"?*) ;;
    *) echo "safe_rm: refusing '$p' ('$lex' is outside work/ and build/)" >&2; exit 1 ;;
  esac
  if [[ -L "$lex" ]]; then rm -f -- "$lex"          # the link itself, never its target
  else rm -rf -- "${lex:?}"; fi
done
