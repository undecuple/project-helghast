#!/usr/bin/env bash
# Run a Windows exe under the user's Proton, hidden and muted, inside the sandbox.
#
#   tools/proton-test.sh [--seconds N] [--shot FILE.png] [--proton DIR] [--name NAME] EXE [ARGS...]
#
# Run from `nix develop` (needs steam-run, xvfb, imagemagick, bubblewrap on PATH).
#  - Sandbox: --gpu (NVIDIA device nodes), no network, repo/game read-only, Proton dir read-only.
#  - Copies EXE's directory to work/proton/<name>/app and uses its own prefix work/proton/<name>/compat.
#  - Hidden display: Xvfb inside the sandbox; never the user's screen. Exception: --real-display (developer use only)
#    runs on the user's X display :0 for real frame-rate numbers; the screenshot is then
#    of the game window only, never the whole screen. Audio stays disabled either way.
#  - Muted: Wine audio drivers disabled through WINEDLLOVERRIDES (never via the mixer).
#  - After N seconds (default 10) takes a screenshot (default work/shots/<name>.png), then kills the run.
# Exit status: 0 if the program was still running at screenshot time or exited 0; the log is printed. 2: refused
# before the run (runenv_check.py: a path switch the sandbox cannot see or write). 4: the run logged "cannot write".
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECONDS_RUN=10 SHOT="" NAME="" PROTON_DIR="$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest"
NETARG=()
DISCARG=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds) SECONDS_RUN="$2"; shift 2 ;;
    --shot) SHOT="$2"; shift 2 ;;
    --proton) PROTON_DIR="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --real-display) REAL=1; shift ;;
    # --net (sandbox.sh refuses it in a parallel worktree): network for the run, e.g. the telemetry trial
    # (2026-10-13). The game itself is never run with it.
    --net) NETARG=(--net); shift ;;
    # --discord (developer use only, temporary test use, 2026-10-22): the Discord socket in the sandbox and
    # wine-discord-ipc-bridge started in the run's prefix before the game (KZ3_DISCORD_BRIDGE = its .exe).
    --discord) DISCARG=(--discord); shift ;;
    *) break ;;
  esac
done
[[ $# -ge 1 ]] || { echo "usage: $0 [--seconds N] [--shot F] [--proton DIR] [--name N] [--net] EXE [ARGS]" >&2; exit 2; }
EXE="$(readlink -f "$1")"; shift
NAME="${NAME:-$(basename "$EXE" .exe)}"
SHOT="${SHOT:-$ROOT/work/shots/$NAME.png}"
RUN="$ROOT/work/proton/$NAME"
[[ -x "$PROTON_DIR/proton" ]] || { echo "no proton at $PROTON_DIR" >&2; exit 2; }

# Path switches preflight (2026-10-15, after a sweep spent 55 GPU minutes writing a template the sandbox could not
# write): every KZ3_/LD_/RSX_ value that is a drive path must be inside this checkout, outputs under work/ or build/.
python3 "$ROOT/tools/runenv_check.py" "$ROOT" || { echo "proton-test: refused before the run (path switches above)" >&2; exit 2; }
mkdir -p "$RUN/app" "$RUN/compat" "$RUN/steam" "$(dirname "$SHOT")"
cp -a "$(dirname "$EXE")/." "$RUN/app/"

# Run provenance (tools/runprov.py, 2026-10-11): work/proton/<name>/run.json records the staged exe, git state,
# patch set, env, Proton, fixtures and cache state before the run, and the outcome from the log after it. It never
# fails a run; tools/runcmp.py reads it before two runs are compared.
DISPLAY_KIND=xvfb; [[ ${REAL:-0} -eq 1 ]] && DISPLAY_KIND=real
if command -v python3 >/dev/null; then
  python3 "$ROOT/tools/runprov.py" start "$RUN" --exe "$RUN/app/$(basename "$EXE")" --seconds "$SECONDS_RUN" \
    --display "$DISPLAY_KIND" --proton "$PROTON_DIR" --root "$ROOT" || true
fi

X11ARG=(); [[ ${REAL:-0} -eq 1 ]] && X11ARG=(--x11)
# On the real display under niri, float the game window when it appears: niri throttles a
# window it does not show (scrolled out of the strip, another workspace) to one frame per second, which voided
# rd-t1. Runtime IPC only (niri msg); the niri config file is not touched. KZ3_NIRI_FLOAT=0 turns it off.
if [[ ${REAL:-0} -eq 1 && "${KZ3_NIRI_FLOAT:-1}" == 1 ]] && command -v niri >/dev/null && niri msg version >/dev/null 2>&1; then
  ( before="$(niri msg -j windows 2>/dev/null | python3 -c 'import json,sys; print(" ".join(str(w["id"]) for w in json.load(sys.stdin)))')"
    for _ in $(seq 1 240); do
      sleep 1
      id="$(niri msg -j windows 2>/dev/null | python3 -c '
import json, re, sys
old = set(sys.argv[1].split())
for w in json.load(sys.stdin):
    tag = "%s %s" % (w.get("app_id") or "", w.get("title") or "")
    # The game window title starts with "ps3recomp" (the runtime window); the Wine console (cmd.exe) opens first.
    if str(w["id"]) not in old and (w.get("title") or "").startswith("ps3recomp"):
        print(w["id"], int(bool(w.get("is_floating")))); break' "$before")"
      [[ -z "$id" ]] && continue
      read -r wid fl <<<"$id"
      (( fl )) || niri msg action toggle-window-floating --id "$wid" >/dev/null 2>&1
      echo "proton-test: floated niri window $wid" >&2; break
    done ) &
fi
set +e
"$ROOT/tools/sandbox.sh" --gpu "${X11ARG[@]}" "${NETARG[@]}" "${DISCARG[@]}" --ro "$PROTON_DIR" -- \
  bash "$ROOT/tools/proton-inner.sh" "$PROTON_DIR" "$RUN" "$SECONDS_RUN" "$SHOT" "$(basename "$EXE")" "$@"
RC=$?
set -e
# A run whose requested output could not be written failed its purpose: exit 4 so series and chains stop at the first
# such run instead of repeating it (same 2026-10-15 sweep: eight chapters of "[pso-template] cannot write").
if grep -qE "cannot (write|create)" "$RUN/app/app_stdout.txt" 2>/dev/null; then
  grep -m3 -E "cannot (write|create)" "$RUN/app/app_stdout.txt" >&2
  echo "proton-test: the run could not write a requested output (exit 4)" >&2
  [[ $RC -eq 0 ]] && RC=4
fi
if command -v python3 >/dev/null; then
  python3 "$ROOT/tools/runprov.py" finish "$RUN" --root "$ROOT" || true
fi
exit $RC
