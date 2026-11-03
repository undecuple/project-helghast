#!/usr/bin/env bash
# Moved-folder end-to-end test of the portable zip (release pass 79; public issue 20 follow-up, release pass 77's fix):
# the zip is unpacked into <R>/A/"Project Helghast", its first run builds the port under Proton (disc dump, 1.14
# update and EBOOT read in place, no network), then A is renamed to B and the zip's launcher logic starts again.
# R = work/release/r79 (KZ3_ZM_DIR overrides). Nothing is committed; the zip holds no game data.
#
#   1. make-zip   build the candidate zip (host, no GPU): patched toolkit, launcher, payload, zip. ~minutes.
#        nix develop .#win --command tools/sandbox.sh -- tools/release/zip_move_test.sh make-zip [VERSION]
#      -> R/out/Project-Helghast-<VERSION>-portable.zip (VERSION defaults to names.VERSION)
#   2. run        the Proton test (takes the gpu lock and the build lock, hours: the first build is ~1 h or more)
#        nix develop --command tools/release/zip_move_test.sh run [ZIP]
#      steps (wall time of each in R/evidence/times.txt):
#        unzip -> A first run (run.cmd, answers fed) -> stamp+exe exist? -> positive control (old-path hits in obj)
#        -> mv A B -> `launch.py --print` (decide()) -> the rebuild command it prints, run as the launcher would
#        -> `launch.py --print` again (action run) -> launcher exe -> kz3.exe from B with the menu-fast pad
#        for KZ3_ZM_PLAY s (default 60), screenshot, end.
#   3. check      the checks on what the run left (host, seconds; also run at the end of `run`)
#        tools/release/zip_move_test.sh check
#      -> R/evidence/results.txt, PASS/FAIL/INFO per check; exit 1 on any FAIL
#   4. selftest   host self-test of `check` on a fake tree (no game data, no Proton); what CI-style runs use
# Env: KZ3_ZM_DIR, KZ3_ZM_PLAY, KZ3_ZM_KEEP_PREFIX=1 (reuse R/compat), KZ3_WT_PROTON (Proton dir),
#      KZ3_ZM_DUMP / KZ3_ZM_UPDATE / KZ3_ZM_EBOOT (Linux paths; default $KZ3_GAME, $KZ3_UPDATE, work/bin/EBOOT-114.elf).
# The launcher's own yes/no box (a rebuild asks) cannot be answered headless; this runs the command decide() chose.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R="${KZ3_ZM_DIR:-$ROOT/work/release/r79}"
EV="$R/evidence"
TOP="Project Helghast"
MODE="${1:-}"; [[ "$MODE" == --inner ]] && { shift; MODE="${1:-}"; INNER=1; } || INNER=0
shift || true
winp() { echo "Z:${1//\//\\}"; }

# ---------------------------------------------------------------------------------------------------------- check
# One check per line in results.txt. OLDRE matches the old folder in either slash style, doubled backslashes (JSON)
# included, case-blind (Windows paths).
OLDRE='r79[\\/]+A[\\/]'
res() { echo "$1 $2" | tee -a "$EV/results.txt"; }
do_check() {
  local B="$R/B/$TOP" b; b="$B/build"; : > "$EV/results.txt"
  local ok=1
  bad() { ok=0; }
  # 1 the console note (stdout of the second run, and the run's own log)
  if grep -q "The install folder was moved or renamed" "$EV/second-run.log" 2>/dev/null; then res PASS "console note line in second-run.log: $(grep -m1 'install folder was moved' "$EV/second-run.log" | tr -d '\r' | cut -c1-160)"
  else res FAIL "no 'install folder was moved or renamed' line in $EV/second-run.log"; bad; fi
  # 2 steps: lifts kept, configure/compile/stamp run again (the second run's log is the newest firstrun log)
  local log="$B/Project-Helghast-firstrun.log"
  for s in load inventory ppu-lift spu-lift; do
    if grep -qE "^## step [0-9]+/[0-9]+ $s: done earlier" "$log" 2>/dev/null; then res PASS "step $s: done earlier, skipped"
    else res FAIL "step $s not 'done earlier' in $log: $(grep -m1 -E "^## step [0-9]+/[0-9]+ $s:" "$log" 2>/dev/null | cut -c1-100)"; bad; fi
  done
  for s in configure compile stamp; do
    if grep -qE "^## step [0-9]+/[0-9]+ $s: " "$log" 2>/dev/null && ! grep -qE "^## step [0-9]+/[0-9]+ $s: done earlier" "$log"; then res PASS "step $s ran again"
    else res FAIL "step $s did not run again in $log"; bad; fi
  done
  grep -qE "^RESULT " "$log" 2>/dev/null && res INFO "log RESULT: $(grep -m1 -E '^RESULT ' "$log" | cut -c1-120)" || { res FAIL "no RESULT line in $log"; bad; }
  # 3 nothing under A, A is gone entirely
  if [[ ! -e "$R/A" && ! -L "$R/A" ]]; then res PASS "no A folder after the second run"; else res FAIL "A exists: $(ls -A "$R/A" | head -3 | tr '\n' ' ')"; bad; fi
  # 4 positive control (the search can see the old path in a tree that has it), then obj
  local c; c="$(cat "$EV/control-obj-hits.txt" 2>/dev/null || echo 0)"
  if [[ "$c" =~ ^[0-9]+$ && "$c" -gt 0 ]]; then res PASS "control: $c files in B/build/obj named the old folder right after the move"
  else res FAIL "control found no old-path hits before the heal ($c): the obj search proves nothing"; bad; fi
  local n; n="$(grep -rilaE "$OLDRE" "$b/obj" 2>/dev/null | wc -l)"
  [[ "$n" == 0 ]] && res PASS "no file under build/obj names the old folder" || { res FAIL "$n files under build/obj name the old folder: $(grep -rilaE "$OLDRE" "$b/obj" | head -3 | tr '\n' ' ')"; bad; }
  # the rest of the tree: INFO (the first run's log may name A; the rotated logs are history)
  res INFO "files outside build/obj naming the old folder: $(grep -rilaE "$OLDRE" "$B" --exclude-dir=obj --exclude-dir=toolchain --exclude-dir=lifted 2>/dev/null | sed "s|^$B/||" | head -8 | tr '\n' ' ')"
  # 5 paths.json and kz3.ini
  for f in paths.json kz3.ini; do
    if [[ ! -f "$b/$f" ]]; then res FAIL "build/$f missing"; bad; continue; fi
    grep -qiaE "$OLDRE" "$b/$f" && { res FAIL "build/$f names the old folder"; bad; } || res PASS "build/$f has no old-folder path"
  done
  if grep -qiaE 'r79[\\/]+B[\\/]' "$b/kz3.ini" 2>/dev/null; then res PASS "kz3.ini names B: $(grep -m1 -iaE 'r79[\\/]+B' "$b/kz3.ini" | tr -d '\r' | cut -c1-140)"
  else res FAIL "kz3.ini does not name B (the launcher did not write it?)"; bad; fi
  # 6 kz3.exe rebuilt in B and started
  if [[ -f "$b/kz3.exe" && "$b/kz3.exe" -nt "$EV/moved.mark" ]]; then res PASS "build/kz3.exe is newer than the move"; else res FAIL "build/kz3.exe missing or not rebuilt after the move"; bad; fi
  local gl; gl="$(ls -t "$b"/logs/*.log 2>/dev/null | head -1)"
  if [[ -n "$gl" && "$gl" -nt "$EV/moved.mark" ]]; then
    res PASS "game log from B: ${gl#"$R"/}"
    res INFO "metrics: $(python3 -I "$ROOT/tools/kz3_metrics.py" "$gl" --alive 1 2>&1 | tr '\n' ' ' | cut -c1-300)"
    grep -qE 'levels\.|main_menu|menu' "$gl" && res INFO "log mentions a menu/level lump" || res INFO "no menu/level lump line (first boot without the profile: judge from the screenshot)"
  else res FAIL "no game log newer than the move in $b/logs"; bad; fi
  [[ -f "$EV/play.png" ]] && res INFO "screenshot: $EV/play.png" || res INFO "no screenshot"
  (( ok )) && { res PASS "ALL CHECKS PASSED"; return 0; } || { res FAIL "SOME CHECKS FAILED"; return 1; }
}

case "$MODE" in
# ------------------------------------------------------------------------------------------------------- selftest
selftest)
  T="$(mktemp -d)"; export KZ3_ZM_DIR="$T/r79" R="$T/r79" EV="$T/r79/evidence"
  B="$R/B/$TOP"; mkdir -p "$EV" "$B/build/obj" "$B/build/logs"
  touch "$EV/moved.mark"; sleep 1
  printf 'NOTE The install folder was moved or renamed (it was Z:\\x\\r79\\A\\%s): ...\n' "$TOP" > "$EV/second-run.log"
  printf '%s\n' "## step 1/9 check: x" "## step 2/9 load: done earlier, skipped" "## step 3/9 inventory: done earlier, skipped" \
    "## step 4/9 ppu-lift: done earlier, skipped" "## step 5/9 spu-lift: done earlier, skipped" "## step 6/9 configure: Configuring" \
    "## step 7/9 compile: Compiling" "## step 8/9 stamp: Finishing" "RESULT ok" > "$B/Project-Helghast-firstrun.log"
  echo 3 > "$EV/control-obj-hits.txt"; echo '{"dump":"/g"}' > "$B/build/paths.json"
  printf 'vfs_root=Z:\\x\\r79\\B\\%s\\build\\x\n' "$TOP" > "$B/build/kz3.ini"
  echo exe > "$B/build/kz3.exe"; echo "[perf] t=1" > "$B/build/logs/g.log"
  KZ3_ZM_DIR="$R" bash "$0" check > "$T/out1.txt" 2>&1; rc1=$?
  printf 'x r79/A/Project Helghast/port\n' > "$B/build/obj/build.ninja"; mkdir "$R/A"
  KZ3_ZM_DIR="$R" bash "$0" check > "$T/out2.txt" 2>&1; rc2=$?
  echo "good tree rc=$rc1 (want 0), bad tree rc=$rc2 (want 1), FAIL lines on bad: $(grep -c '^FAIL' "$T/out2.txt") (want 3: A exists, obj hit, ALL)"
  [[ $rc1 == 0 && $rc2 == 1 ]] && echo "selftest OK" || { echo "selftest FAILED"; tail -12 "$T/out1.txt"; exit 1; }
  rm -rf "$T"
  ;;
check)
  mkdir -p "$EV"; do_check ;;
# ------------------------------------------------------------------------------------------------------ make-zip
make-zip)
  V="${1:-$(python3 -I -c "import sys; sys.path.insert(0,'$ROOT/tools/release'); import names; print(names.VERSION)")}"
  mkdir -p "$R/out" "$EV"; T0=$SECONDS; : > "$EV/make-times.txt"
  mark() { echo "$1 $((SECONDS - T0)) s" | tee -a "$EV/make-times.txt"; }
  MAIN="${KZ3_MAIN:-$ROOT}"; TC="$MAIN/work/release/toolchain-cache"
  [[ -d "$TC" ]] || { echo "no toolchain cache at $TC (fetch_toolchain.py, needs the network once)"; exit 2; }
  # the cache holds no sdl3 pin (SDL3.dll comes from work/release/sdl3 through --sdl3), so a manifest without it
  python3 -I - "$ROOT/tools/release/toolchain.json" "$R/tc.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1], encoding="utf-8"))
m["components"] = [c for c in m["components"] if c["name"] != "sdl3"]
json.dump(m, open(sys.argv[2], "w", encoding="utf-8"), indent=1)
PY
  tools/ps3recomp-patched.sh >"$EV/patched.out" 2>&1 || { tail -5 "$EV/patched.out"; exit 1; }; mark "ps3recomp-patched"
  [[ -f "$ROOT/work/release/sdl3/SDL3.dll" && -f "$ROOT/work/release/fonts/Rajdhani-Bold.ttf" ]] || { echo "missing work/release/sdl3 or fonts"; exit 2; }
  tools/release/build_launcher.sh "$R/launcher" >"$EV/launcher.out" 2>&1 || { tail -5 "$EV/launcher.out"; exit 1; }; mark "launcher"
  python3 -I tools/release/assemble_payload.py --out "$R/payload" --replace --version "$V" --launcher "$R/launcher/Project Helghast.exe" \
    --toolchain "$TC" --toolchain-manifest "$R/tc.json" >"$EV/assemble.out" 2>&1 || { tail -8 "$EV/assemble.out"; exit 1; }; mark "assemble"
  python3 -I tools/release/pack_zip.py --payload "$R/payload" --out "$R/out" --replace >"$EV/pack.out" 2>&1 || { tail -8 "$EV/pack.out"; exit 1; }; mark "pack"
  ls -l "$R"/out/*.zip; cat "$R"/out/*.sha256
  ;;
# --------------------------------------------------------------------------------------------------------- run
run)
  PROTON_DIR="${KZ3_WT_PROTON:-$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest}"
  if (( ! INNER )); then
    ZIP="${1:-$(ls "$R"/out/*-portable.zip 2>/dev/null | head -1)}"
    [[ -f "$ZIP" ]] || { echo "no zip: run make-zip first (or pass ZIP)"; exit 2; }
    mkdir -p "$R/compat" "$R/steam" "$EV"
    export LOCK_EST_MIN="${LOCK_EST_MIN:-150}"
    exec "$ROOT/tools/agents/lock.sh" gpu -- "$ROOT/tools/agents/lock.sh" build -- \
      "$ROOT/tools/sandbox.sh" --gpu --ro "$PROTON_DIR" -- bash "$0" --inner run "$ZIP"
  fi
  ZIP="$1"
  GAME="${KZ3_ZM_DUMP:-${KZ3_GAME:-$ROOT/Game}}"; UPD="${KZ3_ZM_UPDATE:-${KZ3_UPDATE:-$ROOT/GameUpdate}}"
  EBOOT="${KZ3_ZM_EBOOT:-$ROOT/work/bin/EBOOT-114.elf}"
  export DISPLAY=:97
  [[ -e /tmp/.X11-unix/X97 ]] || { Xvfb :97 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XVFB=$!; }
  for _ in $(seq 50); do [[ -e /tmp/.X11-unix/X97 ]] && break; sleep 0.1; done
  export STEAM_COMPAT_DATA_PATH="$R/compat" STEAM_COMPAT_CLIENT_INSTALL_PATH="$R/steam" PROTON_USE_WOW64="${PROTON_USE_WOW64:-1}"
  export WINEDLLOVERRIDES="winepulse.drv,winealsa.drv,wineoss.drv=d" WINEDEBUG="${WINEDEBUG:--all}"
  proton() { steam-run "$PROTON_DIR/proton" "$@"; }
  wskill() { WINEPREFIX="$R/compat/pfx" steam-run "$PROTON_DIR/files/bin/wineserver" -k >/dev/null 2>&1; pkill -f 'kz3\.exe|Project Helghast\.exe' 2>/dev/null; true; }
  trap 'wskill; [[ -n "${XVFB:-}" ]] && kill "$XVFB" 2>/dev/null' EXIT
  mkdir -p "$EV"; : > "$EV/times.txt"; T0=$SECONDS; L=$SECONDS
  mark() { echo "$1 $((SECONDS - L)) s (total $((SECONDS - T0)) s)" | tee -a "$EV/times.txt"; L=$SECONDS; }
  stop() { echo "STOP: $1" | tee -a "$EV/times.txt"; exit 1; }
  # fresh A and B; a kept prefix is the only thing reused
  "$ROOT/tools/safe_rm.sh" "$R/A" "$R/B" 2>/dev/null || rm -rf "$R/A" "$R/B"
  [[ "${KZ3_ZM_KEEP_PREFIX:-0}" == 1 ]] || { "$ROOT/tools/safe_rm.sh" "$R/compat" 2>/dev/null || rm -rf "$R/compat"; mkdir -p "$R/compat"; }
  rm -f "$EV/moved.mark" "$EV/results.txt" "$EV"/*.out "$EV/control-obj-hits.txt" "$EV/play.png"
  proton run 'C:\windows\system32\cmd.exe' /c ver >/dev/null 2>&1; mark "prefix-ready"
  mkdir -p "$R/A"; python3 -I -m zipfile -e "$ZIP" "$R/A" || stop "unzip"; mark "unzipped ($(du -sb "$R/A" | cut -f1) bytes)"
  AD="$R/A/$TOP"; [[ -f "$AD/run.cmd" ]] || stop "no run.cmd in the zip"
  # run.cmd's questions (wizard_test.sh zip): starting answers (n), dump, update, EBOOT, fonts (Enter), check now (n),
  # shaders now (n), pauses at the end read EOF. A driver .cmd keeps the quoted path (cmd /c mangles nested quotes).
  printf '%s\r\n' n "$(winp "$GAME")" "$(winp "$UPD")" "$(winp "$EBOOT")" '' n n '' '' > "$R/answers.txt"
  printf '@echo off\r\ncall "%s\\run.cmd" < "%s" > "%s" 2>&1\r\n' "$(winp "$AD")" "$(winp "$R/answers.txt")" "$(winp "$EV/first-run.out")" > "$R/drive-a.cmd"
  proton run 'C:\windows\system32\cmd.exe' /c "$(winp "$R/drive-a.cmd")" >"$EV/proton-a.out" 2>&1; mark "A first run returned"
  grep -m1 "^RESULT " "$AD"/Project-Helghast-firstrun.log 2>/dev/null | tee -a "$EV/times.txt"
  [[ -f "$AD/build/build-stamp.json" && -f "$AD/build/kz3.exe" ]] || { tail -6 "$EV/first-run.out" | tr -d '\r' | cut -c1-200; stop "A has no build-stamp.json or kz3.exe"; }
  cp "$AD/Project-Helghast-firstrun.log" "$EV/first-run.log"
  # positive control: the build tree names A
  grep -rilaE "$OLDRE" "$AD/build/obj" 2>/dev/null | wc -l > "$EV/control-obj-hits.txt"
  # the move
  mv "$R/A" "$R/B" || stop "rename A to B"; touch "$EV/moved.mark"; mark "renamed A to B"
  BD="$R/B/$TOP"; PY="$(winp "$BD")\\toolchain\\python\\python.exe"
  # decide() as the launcher runs it: launch.py --print (it heals paths.json, then decides); the printed build command is
  # what the launcher would start after the yes/no box
  # launch.py --print's stdout does not reach a Linux pipe under Proton (empty file, rc 0; checked by hand 2026-10-29):
  # a small runner inside the game's Python writes the report to a file instead
  cat > "$R/print.py" <<'PYW'
import contextlib, io, runpy, sys
out, root = sys.argv[1], sys.argv[2]
buf, rc = io.StringIO(), 0
sys.argv = [root + "\\kz3-setup\\release\\launch.py", "--root", root, "--print"]
try:
    with contextlib.redirect_stdout(buf):
        runpy.run_path(sys.argv[0], run_name="__main__")
except SystemExit as e:
    rc = e.code or 0
open(out, "w", encoding="utf-8").write(buf.getvalue())
sys.exit(rc)
PYW
  # each proton call waits for the previous session's wineserver to end: a call started while it shuts down returned at
  # once and ran nothing (fsync "previous shm file was not properly removed", 2026-10-29)
  wwait() { WINEPREFIX="$R/compat/pfx" steam-run "$PROTON_DIR/files/bin/wineserver" -w >/dev/null 2>&1; true; }
  lprint() { wwait; proton run "$PY" "$(winp "$R/print.py")" "$(winp "$1")" "$(winp "$BD")"; wwait; }
  lprint "$EV/print1.json" 2>"$EV/print1.err"; mark "decide() #1"
  python3 -I - "$EV/print1.json" "$EV/rebuild-argv.txt" <<'PY' || stop "print1.json is not the launcher's report"
import json, sys
t = open(sys.argv[1], encoding="utf-8", errors="replace").read()
j = json.loads(t[t.index("{"):])
print("action:", j["action"], "|", j["reason"])
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(j.get("build") or []))
PY
  grep -q '"action": "rebuild"' "$EV/print1.json" || stop "decide() did not say rebuild (see print1.json)"
  mapfile -t ARGV < "$EV/rebuild-argv.txt"
  # through a .cmd driver as the first run (a direct proton run of python.exe on this argv returned at once and ran
  # nothing, 2026-10-29; the driver keeps every argument quoted)
  { printf '@echo off\r\n'; printf '"%s" ' "${ARGV[@]}"; printf '< NUL > "%s" 2>&1\r\n' "$(winp "$EV/second-run.log")"; } > "$R/drive-b.cmd"
  wwait; proton run 'C:\windows\system32\cmd.exe' /c "$(winp "$R/drive-b.cmd")" </dev/null >"$EV/second-run.out" 2>&1; wwait; mark "rebuild (configure + compile again)"
  lprint "$EV/print2.json" 2>>"$EV/print1.err"
  grep -q '"action": "run"' "$EV/print2.json" || stop "decide() #2 is not run (see print2.json)"; mark "decide() #2 = run"
  # the launcher exe, then kz3.exe from B on the menu-fast pad
  PAD="$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$ROOT/tools/pad/menu-fast.pad")"
  PAD_SCRIPT="$PAD" GCM_FLIPCOUNT=1 KZ3_LOG_STDOUT=1 proton run "$(winp "$BD")\\Project Helghast.exe" >"$EV/proton-game.out" 2>&1 &
  sleep "${KZ3_ZM_PLAY:-60}"
  import -window root "$EV/play.png" 2>/dev/null; wskill; mark "game ran ${KZ3_ZM_PLAY:-60} s"
  cp "$BD/logs/launcher.log" "$EV/launcher.log" 2>/dev/null
  do_check; rc=$?; mark "checks"; exit $rc
  ;;
*) sed -n 2,26p "$0" | cut -c3-; exit 2 ;;
esac
