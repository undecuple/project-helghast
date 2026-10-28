#!/usr/bin/env bash
# Inno Setup under Proton, headless (release, plan 4.12): install ISCC into a private prefix, compile
# installer/kz3.iss, and walk the produced setup wizard page by page with screenshots. Never ships; ISCC is an
# approved build tool that is never committed and never in the payload. No network, Xvfb only, muted.
#
#   nix develop --command tools/release/wizard_test.sh install [VERSION]     (default 6.7.3; reads work/release/r13/dl)
#   nix develop --command tools/release/wizard_test.sh compile PAYLOAD OUTDIR [ISCC /D args...]
#   nix develop --command tools/release/wizard_test.sh wizard SETUP.exe SHOTDIR [fail|stop|real|tasks]
#   (pass 19: the welcome page waits KZ3_WT_CHECKWAIT s, default 8, after "Check for a new version": the offline
#   check ran Python under Proton for several seconds and swallowed the next keys)
#   nix develop --command tools/release/wizard_test.sh batch FILE   (pass 18: several walks under one gpu lock; FILE
#       is sourced inside the sandbox, each `run MODE ARGS...` line runs one mode, env assignments before it apply)
#   nix develop --command tools/release/wizard_test.sh tasks SETUP.exe SHOTDIR        (pass 18: the tasks page at
#       96 and 144 DPI, default and /TASKS=...shadersnow, each walk cancelled after the page; KZ3_WT_DPIS="96 144")
#   nix develop --command tools/release/wizard_test.sh uninstall SHOTDIR              (pass 18: unins000.exe of the
#       install in KZ3_WT_STATE, walked with shots; before/after facts of the dump (KZ3_WT_DUMPDIR, a Linux path), the
#       install folder and the saves in SHOTDIR/facts-*.txt; pass 20: KZ3_WT_INSTDIR = the install folder's name under
#       Programs, e.g. "Killzone 3 PC" for an old-name install; KZ3_WT_SAVES="n y" answers each saves question in turn)
#   nix develop --command tools/release/wizard_test.sh zip ZIP SHOTDIR                (pass 18: the portable zip
#       unpacked into KZ3_WT_STATE's C:\Games, run.cmd fed the answers, then the zip's launcher with KZ3_WT_PAD as
#       PAD_SCRIPT for KZ3_WT_PLAY s, a shot every 30 s)
#
# State: work/release/r13/compat (the prefix; KZ3_WT_STATE=DIR uses DIR/compat instead, e.g. a fresh prefix for a
# real install; the ISCC installer download stays in work/release/r13/dl). Takes the gpu lock and the sandbox itself
# (rule 11: never wrap it). Xvfb runs with -dpi 96; KZ3_WT_DPI changes it. Fixture: tools/release/wizard_test/demo_firstrun.py.
# "real" (pass 16): the pages get KZ3_WT_DUMP / KZ3_WT_UPDATE / KZ3_WT_EBOOT (Z:\ paths of a real dump, update and EBOOT,
# read in place through the sandbox's read-only binds) instead of the empty markers, the build runs to its RESULT
# line (polled in the log next to SETUP.exe, a shot every minute, at most KZ3_WT_REAL_MAX s, default 3600), then
# Finish starts the installed launcher ("Start Project Helghast" stays checked) and the display is shot after
# KZ3_WT_PLAY s (default 90). Wall times go to SHOTDIR/times.txt. Pass 18: KZ3_WT_KEEP=1 keeps an earlier install
# (install over it), KZ3_WT_TASKS passes /TASKS=..., KZ3_WT_WINEDPI sets Wine's LogPixels (96 = 100 %, 144 = 150 %).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTON_DIR="${KZ3_WT_PROTON:-$HOME/.local/share/Steam/compatibilitytools.d/Proton-GE Latest}"
R13="$ROOT/work/release/r13"
STATE="${KZ3_WT_STATE:-$R13}"

if [[ "${1:-}" != --inner ]]; then
  mkdir -p "$STATE/compat" "$STATE/steam"
  exec "$ROOT/tools/agents/lock.sh" gpu -- "$ROOT/tools/sandbox.sh" --gpu --ro "$PROTON_DIR" -- \
    bash "$0" --inner "$@"
fi
shift
MODE="${1:?mode}"; shift

export DISPLAY=:98
XVFB=""   # a nested walk (the tasks mode) reuses the display of the walk that started it
if [[ ! -e /tmp/.X11-unix/X98 ]]; then
  Xvfb :98 -screen 0 1280x800x24 -dpi "${KZ3_WT_DPI:-96}" -nolisten tcp >/dev/null 2>&1 &
  XVFB=$!
fi
for _ in $(seq 50); do [[ -e /tmp/.X11-unix/X98 ]] && break; sleep 0.1; done
export STEAM_COMPAT_DATA_PATH="$STATE/compat" STEAM_COMPAT_CLIENT_INSTALL_PATH="$STATE/steam"
# The 32-bit side of steam-run has no libfreetype (no /usr/lib32/libfreetype.so.6): an old-WoW64 32-bit program
# such as Inno Setup gets no TrueType fonts, so dialog metrics are 0 ("AnchorOuterPage: BaseUnitX = 0"). New WoW64
# runs 32-bit programs on the 64-bit Unix libraries, which have it.
export PROTON_USE_WOW64="${PROTON_USE_WOW64:-1}"
export WINEDLLOVERRIDES="winepulse.drv,winealsa.drv,wineoss.drv=d" WINEDEBUG="${WINEDEBUG:--all}"
C="$STATE/compat/pfx/drive_c"
# Pass 20: the prefix is read at each call, so a batch FILE that switches KZ3_WT_STATE between walks runs its own
# `proton run` lines in that prefix (pass 19's and 20's first batches ran them in the starting prefix instead)
proton() { STEAM_COMPAT_DATA_PATH="${KZ3_WT_STATE:-$R13}/compat" steam-run "$PROTON_DIR/proton" "$@"; }
# Pass 18: wineserver -k needs the prefix (else it stops ~/.wine's server, not Proton's): the game a real walk's
# Finish started kept running into the next walk and took its key presses
# (and even so the game windows outlived it in pass 18's batch 3, so the game and launcher processes are also ended
# by name: the sandbox has its own pid namespace, so this never reaches another lane's run)
wskill() { WINEPREFIX="${1:-${KZ3_WT_STATE:-$R13}/compat/pfx}" steam-run "$PROTON_DIR/files/bin/wineserver" -k >/dev/null 2>&1
  pkill -f 'kz3\.exe|Project Helghast\.exe' 2>/dev/null; true; }
winpath() { echo "Z:${1//\//\\}"; }
cleanup() {
  wskill
  [[ -n "$XVFB" ]] && kill "$XVFB" 2>/dev/null
}
trap cleanup EXIT

# Window helpers: the wizard is a top-level X window named "Setup - <AppName>"; no window manager on Xvfb.
# several windows can match (at 144 DPI the first hit was a small helper window, shots cropped 596x432): the largest wins
win() { local best="" ba=0 w; for w in $(xdotool search --onlyvisible --name "$1" 2>/dev/null); do eval "$(xdotool getwindowgeometry --shell "$w")"; (( WIDTH*HEIGHT > ba )) && { ba=$((WIDTH*HEIGHT)); best=$w; }; done; echo "$best"; }
shot() {  # shot NAME: the setup window's area (or the whole display if no window)
  local w; w="$(win "${2:-Setup - }")"
  if [[ -n "$w" ]]; then
    eval "$(xdotool getwindowgeometry --shell "$w")"
    import -window root -crop "${WIDTH}x${HEIGHT}+${X}+${Y}" +repage "$SHOTS/$1.png" 2>/dev/null
  else
    import -window root "$SHOTS/$1.png" 2>/dev/null
  fi
  echo "shot $1 ${w:-root}"
}
key() { local w; w="$(win "Setup - ")"; [[ -n "$w" ]] && xdotool windowfocus --sync "$w" 2>/dev/null; xdotool key --delay 80 "$@"; sleep 1.5; }
typ() { local w; w="$(win "Setup - ")"; [[ -n "$w" ]] && xdotool windowfocus --sync "$w" 2>/dev/null
  xdotool key End shift+Home; xdotool type --delay 30 "$1"; sleep 0.5; }

setdpi() {  # Wine's own DPI (Xvfb's -dpi does not change it): both keys Wine reads, then a fresh wineserver
  proton run reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d "$1" /f >/dev/null 2>&1
  proton run reg add 'HKLM\System\CurrentControlSet\Hardware Profiles\Current\Software\Fonts' /v LogPixels \
    /t REG_DWORD /d "$1" /f >/dev/null 2>&1
  wskill; sleep 1
}
keyw() {  # keyw TITLE-PATTERN KEYS...: focus the newest visible window matching, then press
  local w; w="$(xdotool search --onlyvisible --name "$1" 2>/dev/null | tail -1)"; shift
  [[ -n "$w" ]] && xdotool windowfocus --sync "$w" 2>/dev/null; xdotool key --delay 80 "$@"; sleep 1.5
}
fclick() {  # pass 19: a click on the wizard image gives the window the input focus (an install over an earlier
  # install lost the first keys without it: the welcome page took neither Space nor Return)
  local w; w="$(win "Setup - ")"; [[ -z "$w" ]] && return
  eval "$(xdotool getwindowgeometry --shell "$w")"; xdotool mousemove $((X + 60)) $((Y + 150)) click 1; sleep 1
}
names() { xdotool search --onlyvisible --name . getwindowname %@ 2>/dev/null | sort -u | head -8 | tr '\n' '|'; echo; }
INST_REL="users/steamuser/AppData/Local/Programs/${KZ3_WT_INSTDIR:-Project Helghast}"   # pass 20: an old-name install
facts() {  # facts FILE: what the uninstall must remove or keep (Linux view of the prefix; the dump through the bind)
  { echo "# $(date +%T)"
    if [[ -n "${KZ3_WT_DUMPDIR:-}" ]]; then
      echo "dump files $(find "$KZ3_WT_DUMPDIR/" -type f | wc -l) bytes $(du -sb "$KZ3_WT_DUMPDIR/" | cut -f1)"
      echo "dump PS3_GAME/USRDIR mtime $(stat -c %Y "$KZ3_WT_DUMPDIR/PS3_GAME/USRDIR" 2>/dev/null)"
    fi
    local I="$C/$INST_REL"
    echo "install exists: $([[ -d "$I" ]] && echo yes || echo no)"
    [[ -d "$I" ]] && { echo "install top: $(ls -A "$I" | tr '\n' ' ')"; echo "install bytes $(du -sb "$I" | cut -f1)"
      echo "vfs: $(ls -lA "$I/vfs" 2>/dev/null | tail -n +2 | awk '{print $1, $NF}' | tr '\n' ';')"
      echo "build exists: $([[ -d "$I/build" ]] && echo yes || echo no)"; }
    local A="$C/users/steamuser/AppData/Roaming/Project Helghast" O="$C/users/steamuser/AppData/Roaming/Killzone 3 PC"
    echo "appdata: $(ls -A "$A" 2>/dev/null | tr '\n' ' ')"
    echo "old appdata: $([[ -d "$O" ]] && find "$O" -type f | sed "s|$O/||" | sort | tr '\n' ' ' || echo absent)"
    [[ -f "$A/keybinds.ini" ]] && echo "keybinds sha $(sha256sum "$A/keybinds.ini" | cut -c1-16)"
    [[ -d "$A/gamedata" ]] && echo "gamedata files $(find "$A/gamedata" -type f | wc -l) sha $(find "$A/gamedata" -type f -exec sha256sum {} + | sort | sha256sum | cut -c1-16)"
  } > "$1"; cat "$1" | cut -c1-200
}

case "$MODE" in
install)
  V="${1:-6.7.3}"
  # the installer must match the pin in toolchain.json (same check as release.yml); a mismatch aborts
  want=$(python3 -I -c 'import json,sys;print(next(c["sha256"] for c in json.load(open(sys.argv[1]))["build_tools"] if c["name"]=="innosetup" and c["version"]==sys.argv[2]))' "$ROOT/tools/release/toolchain.json" "$V") \
    || { echo "no pinned innosetup $V in toolchain.json"; exit 1; }
  got=$(sha256sum "$R13/dl/innosetup-$V.exe" | cut -d' ' -f1)
  [[ "$got" == "$want" ]] || { echo "innosetup-$V.exe SHA-256 mismatch: $got != $want"; exit 1; }
  # /CURRENTUSER: per-user, no admin; /DIR: fixed path for ISCC.exe; no wizard pages are shown at all
  proton run "$R13/dl/innosetup-$V.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER \
    '/DIR=C:\inno' '/LOG=C:\inno-install.log' >"$R13/install-$V.out" 2>&1
  echo "install rc=$?"; ls "$C/inno" 2>&1 | head -20
  tail -5 "$C/inno-install.log" 2>/dev/null | cut -c1-200
  ;;
compile)
  PAYLOAD="$1" OUT="$2"; shift 2; mkdir -p "$OUT"
  cd "$ROOT/installer" || exit 1
  T0=$SECONDS
  proton run 'C:\windows\system32\cmd.exe' /v:on /c "C:\\inno\\ISCC.exe /DPayload=$(winpath "$PAYLOAD") /O$(winpath "$OUT") $* kz3.iss > $(winpath "$OUT")\\iscc.txt 2>&1 & echo [exit code !errorlevel!] >> $(winpath "$OUT")\\iscc.txt" \
    >"$OUT/proton.out" 2>&1
  echo "compile: $((SECONDS - T0)) s"; tail -25 "$OUT/iscc.txt" | cut -c1-220; ls -la "$OUT"/*.exe 2>/dev/null
  ;;
wizard)
  SETUP="$1" SHOTS="$2" FAIL="${3:-}"; mkdir -p "$SHOTS"
  TIMES="$SHOTS/times.txt"; T0=$SECONDS; : > "$TIMES"
  mark() { echo "$1 $((SECONDS - T0)) s" | tee -a "$TIMES"; }
  DUMP="${KZ3_WT_DUMP:-C:\kz3test\dump}" UPDATE="${KZ3_WT_UPDATE:-C:\kz3test\update}" EBOOT="${KZ3_WT_EBOOT:-}"
  # Empty marker files only (the wizard's presence checks); no game data.
  for f in dump/PS3_GAME/PARAM.SFO dump/PS3_GAME/USRDIR/data_ps3.psarc update/PARAM.SFO update/USRDIR/EBOOT.BIN \
           update/USRDIR/added_data_ps3.psarc.edat update/USRDIR/EBOOT.elf; do
    mkdir -p "$(dirname "$C/kz3test/$f")"; : > "$C/kz3test/$f"; done
  [[ "${KZ3_WT_KEEP:-0}" == 1 ]] || rm -rf "$C/users/steamuser/AppData/Local/Programs/Project Helghast"
  [[ "${KZ3_WT_KEEP:-0}" == 1 ]] && facts "$SHOTS/facts-before.txt"
  # firstrun.py rewrites the log next to SETUP.exe: an earlier walk's RESULT line must not end this one's wait
  if [[ "$FAIL" == real ]]; then for l in "$(dirname "$SETUP")"/*.log; do [[ -f "$l" ]] && mv "$l" "$l.$(date +%s).old"; done; fi
  [[ -n "${KZ3_WT_WINEDPI:-}" ]] && setdpi "$KZ3_WT_WINEDPI"
  # Windows 10 has Segoe UI (Inno's dialog font); this prefix does not, and Wine's fallback is a serif face. Map it
  # to Tahoma (in the prefix) so the shots are closer to what a player sees. KZ3_WT_NOSUBST=1 keeps the fallback.
  if [[ "${KZ3_WT_NOSUBST:-0}" != 1 ]]; then
    # Proton writes "Segoe UI"="Times New Roman" into the prefix (a reg add did not override it): edit the hive while
    # no wineserver runs
    # On a prefix's first run user.reg does not exist yet (the edit silently did nothing and the first prefix showed the
    # serif face): create the prefix first (wineboot -u via a trivial command), stop its server so the hive is flushed.
    if ! grep -q '^"Segoe UI"=' "$STATE/compat/pfx/user.reg" 2>/dev/null; then
      proton run 'C:\windows\system32\cmd.exe' /c ver >/dev/null 2>&1; wskill "$STATE/compat/pfx"
    fi
    sed -i 's/^"Segoe UI"="Times New Roman"/"Segoe UI"="Tahoma"/' "$STATE/compat/pfx/user.reg" 2>/dev/null
  fi
  [[ "$FAIL" == fail ]] && export KZ3_WIZARD_DEMO_FAIL=1
  [[ "$FAIL" == stop ]] && export KZ3_WIZARD_DEMO_SECS="${KZ3_WIZARD_DEMO_SECS:-8}"
  export KZ3_WIZARD_DEMO_SECS="${KZ3_WIZARD_DEMO_SECS:-3}"
  mkdir -p "$C/kz3test"
  TASKARG=(); [[ -n "${KZ3_WT_TASKS:-}" ]] && TASKARG=("/TASKS=$KZ3_WT_TASKS")
  proton run "$SETUP" '/LOG=C:\kz3test\setup-inno.log' "${TASKARG[@]}" >"$SHOTS/proton.out" 2>&1 &
  for _ in $(seq 600); do [[ -n "$(win 'Setup - ')" ]] && break; sleep 1; done
  sleep "${KZ3_WT_SHOWWAIT:-3}"; mark "wizard-shown"   # pass 19: KZ3_WT_SHOWWAIT for a slow first start
  fclick   # pass 20: every walk (a fresh-prefix walk also lost its first keys once, stuck at the licence page)
  xdotool search --onlyvisible --name . getwindowname %@ 2>/dev/null | head -10
  shot 01-welcome
  # Keyboard only (Alt+letter is unreliable here: it selected the licence text): the welcome page opens with the
  # focus on "Check for a new version" (Space = the click; offline in the sandbox), Return = the default button.
  # Pass 29 (step 9s): the "Setup type" page after the welcome page. KZ3_WT_TYPE=advanced (default, every page as
  # before plus the fonts and Advanced options pages) or recommended (no tasks, EBOOT, fonts or Advanced pages: the
  # marker update has USRDIR\EBOOT.elf). The type list takes Down + Space; KZ3_WT_ADV="keys" acts on the Advanced page.
  WT_TYPE="${KZ3_WT_TYPE:-advanced}"
  key space; sleep "${KZ3_WT_CHECKWAIT:-8}"; shot 02-welcome-checked; key Return; shot 02b-type
  # An install over one made with Advanced preselects Advanced: Up + Space picks Recommended either way
  if [[ "$WT_TYPE" == advanced ]]; then key Down space; shot 02c-type-advanced; else key Up space; shot 02c-type-recommended; fi
  key Return; shot 03-licence
  key Tab Up; shot 03b-licence-accepted; key Return; shot 04-dir; key Return
  if [[ "$WT_TYPE" == advanced ]]; then
    shot 05-tasks
    if [[ "$FAIL" == tasks ]]; then  # cancel: Escape = Cancel, the "Exit Setup?" box takes y
      key Escape; sleep 1; keyw . y; sleep 3; names; mark "end"; exit 0
    fi
    key Return
  fi
  shot 06-dump
  typ "$DUMP"; key Return; shot 07-update
  typ "$UPDATE"; key Return
  if [[ "$WT_TYPE" == advanced ]]; then
    shot 08-eboot
    [[ -n "$EBOOT" ]] && { typ "$EBOOT"; shot 08b-eboot-typed; }
    key Return; shot 08c-fonts; key Return; shot 08d-advanced
    [[ -n "${KZ3_WT_ADV:-}" ]] && { key ${KZ3_WT_ADV}; shot 08e-advanced-set; }
    key Return
  fi
  shot 09-ready
  mark "pages-done"; key Return; sleep 4; shot 10-installing
  if [[ "$FAIL" == stop ]]; then
    # The build page's only enabled control is "Stop the build": Tab focuses it, Space clicks; the confirmation
    # defaults to No, so "y" answers Yes
    sleep 3; shot 11-build-1; key Tab; key space; sleep 1; shot 11-stop-confirm "zzz-no-such-window"; key y; sleep 4
    shot 11-stopped; for _ in $(seq 10); do sleep 2; done; shot 13-finish
  else
    for i in 1 2 3 4 5 6; do sleep 4; shot "11-build-$i"; done
  fi
  if [[ "$FAIL" == real ]]; then
    # The build's RESULT line lands in the log next to SETUP.exe ({src}); the file copy before it is the first wait
    LOG=""; N=0
    while (( SECONDS - T0 < ${KZ3_WT_REAL_MAX:-3600} )); do
      sleep 10; N=$((N + 1))
      [[ -z "$LOG" ]] && LOG="$(ls -t "$(dirname "$SETUP")"/*.log 2>/dev/null | head -1)"
      (( N % 6 == 0 )) && { shot "11-build-$((N / 6))"; tail -1 "$LOG" 2>/dev/null | cut -c1-160; }
      [[ -n "$LOG" ]] && grep -q "^RESULT " "$LOG" && break
    done
    mark "build-result"; grep -m1 "^RESULT " "$LOG" 2>/dev/null; sleep 5; shot 13-finish
    # Finish with "Start Project Helghast" checked: the installed launcher starts build\kz3.exe (launch.py "run")
    key Return; sleep "${KZ3_WT_PLAY:-90}"; mark "played"
    xdotool search --onlyvisible --name . getwindowname %@ 2>/dev/null | head -10
    shot 14-game "zzz-no-such-window"; shot 14-game-window "Killzone"
  elif [[ "$FAIL" == stop ]]; then :
  elif [[ "$FAIL" == fail ]]; then
    sleep 20; xdotool search --onlyvisible --name . getwindowname %@ 2>/dev/null | head; shot 12-error "Setup"
    shot 12-error-root "zzz-no-such-window"
  else
    for _ in $(seq 30); do sleep 2; xdotool search --onlyvisible --name . getwindowname %@ 2>/dev/null | grep -q . || break
      [[ -n "$(xdotool search --onlyvisible --name 'Setup - ' 2>/dev/null)" ]] || break; done
    sleep 3; shot 13-finish
  fi
  shot 99-display "zzz-no-such-window"; mark "end"
  [[ "${KZ3_WT_KEEP:-0}" == 1 ]] && facts "$SHOTS/facts-after.txt"
  grep -m20 -i "error\|exception\|page" "$C/kz3test/setup-inno.log" 2>/dev/null | cut -c1-200
  ;;
batch)
  run() { echo "=== $(date +%T) $*"; bash "$0" --inner "$@"; wskill "${KZ3_WT_STATE:-$R13}/compat/pfx"; }
  # shellcheck disable=SC1090
  source "$1"
  ;;
tasks)
  SETUP="$1" SHOTS="$2"
  for d in ${KZ3_WT_DPIS:-96 144}; do
    for t in default shadersnow; do
      [[ "$t" == default ]] && unset KZ3_WT_TASKS || export KZ3_WT_TASKS="desktopicon,buildnow,shadersnow"
      KZ3_WT_WINEDPI="$d" bash "$0" --inner wizard "$SETUP" "$SHOTS/dpi$d-$t" tasks 2>&1 | grep -v '^shot ' | tail -3
      wskill
    done
  done
  ;;
uninstall)
  SHOTS="$1"; mkdir -p "$SHOTS"; T0=$SECONDS
  wskill; facts "$SHOTS/facts-before.txt"
  # Not silent: the player's path (Start menu > Uninstall). Pages: "Are you sure" (default Yes) -> progress ->
  # "Delete your saves too?" (default No = keep; KZ3_WT_DELSAVES=1 answers y) -> "was removed" (OK).
  proton run "$(winpath "$C/$INST_REL/unins000.exe")" >"$SHOTS/proton.out" 2>&1 &
  for _ in $(seq 120); do [[ -n "$(xdotool search --onlyvisible --name Uninstall 2>/dev/null)" ]] && break; sleep 1; done
  sleep 2; names; shot 21-confirm Uninstall; keyw Uninstall y   # the box's default is No
  sleep "${KZ3_WT_UNINST_WAIT:-40}"   # the removal itself (thousands of toolchain files) before the saves question
  # Pass 20: KZ3_WT_SAVES="n y" answers several saves questions in turn (new data folder, then the old-name one)
  ANS="${KZ3_WT_SAVES:-$([[ "${KZ3_WT_DELSAVES:-0}" == 1 ]] && echo y || echo n)}"; I=0
  for a in $ANS; do
    I=$((I + 1)); sleep 2; names; shot "22-saves$I" Uninstall; shot "22-saves$I-root" "zzz-no-such-window"
    keyw Uninstall "$a"; sleep 2
  done
  sleep 4; names; shot 23-done Uninstall; keyw Uninstall Return; sleep 5; names
  echo "uninstall walk $((SECONDS - T0)) s"
  facts "$SHOTS/facts-after.txt"
  ;;
zip)
  ZIP="$1" SHOTS="$2"; mkdir -p "$SHOTS"; TIMES="$SHOTS/times.txt"; T0=$SECONDS; : > "$TIMES"
  mark() { echo "$1 $((SECONDS - T0)) s" | tee -a "$TIMES"; }
  G="$C/Games"; rm -rf "$G"; mkdir -p "$G"
  proton run 'C:\windows\system32\cmd.exe' /c ver >/dev/null 2>&1; mark "prefix-ready"
  python3 -I -m zipfile -e "$ZIP" "$G" && mark "unzipped"; du -sb "$G" | cut -f1
  D="$G/Project Helghast"; WD='C:\Games\Project Helghast'
  # run.cmd's questions: "Use these folders as the starting answers?" (n), dump, update, EBOOT, fonts folder (Enter),
  # check now (n), shaders now (n); the pause at the end reads EOF. A driver .cmd holds the quoted paths (cmd /c on
  # a Wine-built command line mangles nested quotes).
  printf '%s\r\n' n "${KZ3_WT_DUMP:?}" "${KZ3_WT_UPDATE:?}" "${KZ3_WT_EBOOT:?}" '' n n '' '' > "$G/answers.txt"
  printf '@echo off\r\ncall "%s\\run.cmd" < C:\\Games\\answers.txt > C:\\Games\\run-out.txt 2>&1\r\n' "$WD" > "$G/drive.cmd"
  proton run 'C:\windows\system32\cmd.exe' /c 'C:\Games\drive.cmd' >"$SHOTS/proton-build.out" 2>&1
  mark "run.cmd-returned"; grep -m1 "^RESULT " "$D"/Project-Helghast-firstrun*.log 2>/dev/null
  tail -4 "$G/run-out.txt" | tr -d '\r' | cut -c1-200
  [[ -f "$D/build/kz3.exe" ]] || { echo "no build/kz3.exe"; exit 1; }
  export PAD_SCRIPT="${KZ3_WT_PAD:-}" GCM_FLIPCOUNT=1
  proton run "$WD\\Project Helghast.exe" >"$SHOTS/proton-game.out" 2>&1 &
  mark "launcher-started"; N=0
  while (( N * 30 < ${KZ3_WT_PLAY:-300} )); do sleep 30; N=$((N + 1)); shot "$(printf 'play-%03d' $((N * 30)))" "zzz-no-such-window"; done
  names; mark "played"
  ls -t "$D"/build/logs/*.log 2>/dev/null | head -1 | xargs -r -I{} cp {} "$SHOTS/game.log"
  cp "$D"/logs/launcher.log "$SHOTS/" 2>/dev/null; du -sb "$D" | cut -f1
  ;;
*) echo "unknown mode $MODE" >&2; exit 2 ;;
esac
