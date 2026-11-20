#!/usr/bin/env bash
# Lint for the two path bugs that made tests pass only in the main checkout. Pure grep/awk, no compiler.
#   1. SPLIT: a flag STRING that holds a $-path (W="-I$P/x -L$P/y", -I/-L/-o/-isystem) later expanded unquoted
#      ($W): the checkout path "Killzone 3" has a space, so it splits into broken words. Use an array: W=(-I"$P/x");
#      "${W[@]}". A deliberate case carries "# nosplit-ok" on the assignment line.
#   2. APPLY: `git apply` (also `git -C X apply`) into work/spu, work/recomp, ... (directories that
#      are links): git refuses paths beyond a symlink. Use GNU patch (as tools/ps3recomp-patched.sh does). Marker
#      "# gitapply-ok" on the line.
# Known offenders are listed in tools/smoke/path_split_baseline.txt (one path per line): they are listed, not failed;
# a NEW offender fails the test. Fix one and delete its baseline line (a baseline line that no longer flags is
# reported as "fixed, drop it").  --list prints every finding (file:line:kind:text) and exits 0.
# Negative control: a fixture with each bug must be flagged, a clean fixture must not.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE="$ROOT/tools/smoke/path_split_baseline.txt"
LINKED='work/(spu|spu-100|spu-main|recomp|recomp-100|bin|analysis|analysis-114)(/|[[:space:]"]|$)|KZ3_SPU_DIR|KZ3_RECOMP_DIR'

lint_file() {   # lint_file FILE : prints file:line:kind:text per finding
  local f="$1" name
  # kind SPLIT: scalar assignment whose string carries a -I/-L/-o/-isystem followed by a $ expansion
  while IFS=: read -r ln rest; do
    [[ "$rest" == *"nosplit-ok"* ]] && continue
    name="$(sed -E 's/^[[:space:]]*(local[[:space:]]+|export[[:space:]]+|declare[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)=.*/\2/' <<<"$rest")"
    # later (or same-line continuation) unquoted expansion: $NAME or ${NAME} not directly inside "..." as a whole word
    if awk -v n="$name" -v from="$ln" 'NR>from && $0 !~ /^[[:space:]]*#/ {
          s=$0; pat="[$][{]?" n "[}]?([^A-Za-z0-9_}\\[]|$)"
          while (match(s, pat)) {
            pre=(RSTART>1)?substr(s,RSTART-1,1):""; post=substr(s,RSTART+RLENGTH-1,1)
            if (!(pre=="\"" && post=="\"")) { found=1; exit }
            s=substr(s,RSTART+RLENGTH)
          } } END { exit !found }' "$f"; then
      printf '%s:%s:SPLIT:%s\n' "$f" "$ln" "$name"
    fi
  done < <(grep -nE '^[[:space:]]*(local[[:space:]]+|export[[:space:]]+|declare[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*="[^"]*-(I|L|o|isystem)[[:space:]]?"?[$]' "$f" 2>/dev/null)
  # kind APPLY
  grep -nE '(^|[^#a-zA-Z])git( +-C +[^ ]+)? +apply( |$)' "$f" 2>/dev/null | grep -vE '^[0-9]+:[[:space:]]*#' |
    grep -E "$LINKED" | grep -v 'gitapply-ok' | while IFS=: read -r ln rest; do printf '%s:%s:APPLY:%s\n' "$f" "$ln" "$rest"; done
}

lint_tree() {   # lint_tree DIR : all *.sh under DIR (not this test, not fixtures)
  local f
  while IFS= read -r f; do
    [[ "$f" == */test_no_path_split.sh ]] && continue
    lint_file "$f"
  done < <(find "$1" -name '*.sh' -type f | sort)
}

if [[ "${1:-}" == --list ]]; then lint_tree "$ROOT/tools" | sed "s#^$ROOT/##"; exit 0; fi

fail() { echo "[test_no_path_split] FAIL: $*"; exit 1; }

# --- negative control: fixtures with the bugs must be flagged, clean ones not
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat >"$T/bad_split.sh" <<'FX'
P="$ROOT/x y"
W="-std=c11 -I$P/include -L$P/lib"
cc $W a.c -o a
FX
cat >"$T/bad_out.sh" <<'FX'
FL="-O2 -o $O/prog"
cc ${FL} a.c
FX
cat >"$T/bad_apply.sh" <<'FX'
git apply --directory=work/spu "$P/x.patch"
git -C "$ROOT" apply work/recomp/x.patch
FX
cat >"$T/good.sh" <<'FX'
W=(-std=c11 -I"$P/include")
cc "${W[@]}" a.c
Q="-I$P/include"   # nosplit-ok
cc $Q a.c
X="-I$P/inc"
echo "$X"
# git apply work/spu in a comment
patch -p1 -d work/spu <x.patch
FX
n_bad="$(lint_file "$T/bad_split.sh" | wc -l)"; [[ "$n_bad" == 1 ]] || fail "negative control: split fixture gave $n_bad findings (want 1)"
[[ "$(lint_file "$T/bad_out.sh")" == *SPLIT* ]] || fail "negative control: -o string expanded as \${FL} not flagged"
[[ "$(lint_file "$T/bad_apply.sh" | grep -c APPLY)" == 2 ]] || fail "negative control: git apply into work/ dirs not flagged twice"
g="$(lint_file "$T/good.sh")"; [[ -z "$g" ]] || fail "negative control: clean fixture flagged: $g"

# --- the tree
found="$(lint_tree "$ROOT/tools" | sed "s#^$ROOT/##")"
new=0 known=0
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  f="${line%%:*}"
  if grep -qxF "$f" "$BASE" 2>/dev/null; then known=$((known+1)); else new=$((new+1)); echo "  NEW: $line"; fi
done <<<"$found"
if [[ -f "$BASE" ]]; then
  while IFS= read -r b; do
    [[ -z "$b" || "$b" == \#* ]] && continue
    grep -q "^$b:" <<<"$found" || echo "  fixed, drop from tools/smoke/path_split_baseline.txt: $b"
  done <"$BASE"
fi
(( new == 0 )) || fail "$new new path-split/apply finding(s); use arrays (W=(-I\"\$P\")) and GNU patch (as tools/ps3recomp-patched.sh does)"
echo "[test_no_path_split] PASS ($known known finding(s) in the baseline, 0 new; negative controls flagged)"
