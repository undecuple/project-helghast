#!/usr/bin/env bash
# Lint the files a commit touches: ruff for Python, shellcheck (warning level)
# for shell, clang-format --dry-run for our own C/C++. Configs: ruff.toml, .clang-format.
#   tools/lint.sh [FILE...]     no files = the staged files (git diff --cached)
# Warn-only: prints findings and exits 0, unless KZ3_LINT_STRICT=1 (then findings fail). Called by .githooks/pre-commit.
# Scope: tools/, re/, port/kz3, port/spu, port/vfs. Never port/patches (upstream style), third_party, work/ or build/.
# Outside `nix develop` the tools are not on PATH; the script re-runs itself in the .#re shell (a few seconds).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 0
if ! command -v ruff >/dev/null || ! command -v shellcheck >/dev/null || ! command -v clang-format >/dev/null; then
  [[ -n "${KZ3_LINT_INNER:-}" ]] && { echo "lint: tools missing in the .#re shell" >&2; exit 0; }
  KZ3_LINT_INNER=1 exec nix develop "$ROOT#re" --command "$0" "$@" 2>/dev/null
fi
if (( $# )); then files=("$@"); added=("$@"); else
  mapfile -t files < <(git diff --cached --name-only --diff-filter=ACM)
  mapfile -t added < <(git diff --cached --name-only --diff-filter=A)
fi
# clang-format only for new files: existing ones differ from the style on 25-50 % of their lines (2026-10-09), and
# reformatting them would conflict with every open parallel branch.
is_added() { local a; for a in "${added[@]}"; do [[ "$a" == "$1" ]] && return 0; done; return 1; }
py=(); sh=(); cc=()
for f in "${files[@]}"; do
  [[ -f "$f" ]] || continue
  case "$f" in third_party/*|work/*|build/*|port/patches/*|Game/*|GameUpdate/*) continue ;; esac
  case "$f" in
    *.py) py+=("$f") ;;
    *.sh) sh+=("$f") ;;
    port/kz3/*.c|port/kz3/*.cpp|port/kz3/*.h|port/spu/*.c|port/spu/*.cpp|port/spu/*.h|port/vfs/*.c|port/vfs/*.h|tools/*.c)   # in case patterns * also matches /
      is_added "$f" && cc+=("$f") ;;
  esac
done
found=0
if (( ${#py[@]} )); then ruff check -q "${py[@]}" || found=1; fi
if (( ${#sh[@]} )); then shellcheck -S warning -f gcc "${sh[@]}" || found=1; fi
for f in "${cc[@]}"; do
  n=$(clang-format --dry-run "$f" 2>&1 | grep -c "warning:" || true)
  (( n > 0 )) && { echo "$f: $n clang-format differences (clang-format -i $f to apply)"; found=1; }
done
if (( found )); then
  [[ "${KZ3_LINT_STRICT:-0}" == 1 ]] && { echo "lint: findings above (KZ3_LINT_STRICT=1)" >&2; exit 1; }
  echo "lint: findings above (warn-only)" >&2
fi
exit 0
