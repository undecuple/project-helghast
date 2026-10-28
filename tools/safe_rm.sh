#!/usr/bin/env bash
# Guarded recursive delete. Only removes paths inside $KZ3_ROOT/work or $KZ3_ROOT/build.
#   tools/safe_rm.sh <path>...
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for p in "$@"; do
  real="$(readlink -f -- "${p:?empty path}")"
  case "$real" in
    "$ROOT/work/"?*|"$ROOT/build/"?*) rm -rf -- "${real:?}" ;;
    *) echo "safe_rm: refusing '$p' (resolves to '$real', outside work/ and build/)" >&2; exit 1 ;;
  esac
done
