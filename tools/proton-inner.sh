#!/usr/bin/env bash
# Inside the sandbox (called by tools/proton-test.sh): Xvfb + Proton run + screenshot + cleanup.
set -uo pipefail
PROTON_DIR="$1" RUN="$2" SECS="$3" SHOT="$4" EXE="$5"; shift 5

if [[ -n "${KZ3_REAL_DISPLAY:-}" ]]; then
  export DISPLAY="$KZ3_REAL_DISPLAY"; XVFB=""          # user's display (proton-test.sh --real-display)
else
  export DISPLAY=:99
  Xvfb :99 -screen 0 "${KZ3_XVFB_SCREEN:-1280x720}x24" -nolisten tcp >/dev/null 2>&1 &
  XVFB=$!
  for _ in $(seq 50); do [[ -e /tmp/.X11-unix/X99 ]] && break; sleep 0.1; done
fi

export STEAM_COMPAT_DATA_PATH="$RUN/compat"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$RUN/steam"
export WINEDLLOVERRIDES="winepulse.drv,winealsa.drv,wineoss.drv=d${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export PROTON_LOG=1 PROTON_LOG_DIR="$RUN"
export VK_ICD_FILENAMES=/run/opengl-driver/share/vulkan/icd.d/nvidia_icd.json
# kz3.exe with the logger (patch 2030) writes only errors to stdout unless this is set; checks parse stdout.
export KZ3_LOG_STDOUT="${KZ3_LOG_STDOUT:-1}" KZ3_NO_ERROR_BOX="${KZ3_NO_ERROR_BOX:-1}"
cd "$RUN/app" || exit 1

# Proton does not wire a console program's stdout to ours; capture it with cmd's redirect instead
# and pass the program's exit code back with delayed expansion (!errorlevel!). Proton's log: proton.log.
# Rich Presence test (proton-test.sh --discord): wine-discord-ipc-bridge relays \\.\pipe\discord-ipc-0 to the bound
# Discord socket. Started by the game's own cmd line (same Proton session and wineserver as the game).
BRIDGE=""
if [[ -n "${KZ3_DISCORD_BRIDGE:-}" && -S "${XDG_RUNTIME_DIR:-/nonexistent}/discord-ipc-0" ]]; then
  BRIDGE="start \"\" /b \"Z:${KZ3_DISCORD_BRIDGE//\//\\}\" > discord-bridge-out.txt 2>&1 & ping -n 3 127.0.0.1 >nul & "
fi
steam-run "$PROTON_DIR/proton" run cmd /v:on /c "$BRIDGE$EXE $* > app_stdout.txt 2>&1 & echo [exit code !errorlevel!]>> app_stdout.txt & exit !errorlevel!" >"$RUN/proton.log" 2>&1 &
APP=$!
# KZ3_VIDEO=START:SECONDS: snapshot the hidden display from START s
# for SECONDS s as fast as import manages (~5-10/s), then tools/make_clip.py builds <shot>.mp4 at wall-clock speed
# with the game's own fps (from its [flip] log, GCM_FLIPCOUNT=1) burned in. Xvfb only, never the user's display.
# KZ3_VIDEO_EXT=png grabs lossless frames instead (slower, for stills such as the README picture; make_clip.py reads jpg).
VID=""
if [[ -n "${KZ3_VIDEO:-}" && -n "$XVFB" ]]; then
  VSTART="${KZ3_VIDEO%%:*}" VLEN="${KZ3_VIDEO##*:}"; VDIR="$RUN/clip"; mkdir -p "$VDIR"
  ( sleep "$VSTART"; T0=$(date +%s.%N); i=0
    while :; do
      now=$(date +%s.%N); awk -v a="$now" -v b="$T0" -v l="$VLEN" 'BEGIN{exit !(a-b < l)}' || break
      VEXT="${KZ3_VIDEO_EXT:-jpg}"; VQ=(); [[ "$VEXT" == jpg ]] && VQ=(-quality 85)
      f=$(printf '%06d.%s' $i "$VEXT"); import -window root "${VQ[@]}" "$VDIR/$f" 2>/dev/null \
        && echo "$f $(awk -v a="$now" -v b="$T0" -v s="$VSTART" 'BEGIN{printf "%.3f", a-b+s}')" >> "$VDIR/times.txt"
      i=$((i+1))
    done ) &
  VID=$!
fi
# KZ3_EXIT_EARLY=1 (opt-in, for runs that end by themselves such as a toolchain trial): stop waiting once the
# program has exited, so a long --seconds cap does not hold the GPU lock after the work is done.
if [[ "${KZ3_EXIT_EARLY:-0}" == 1 ]]; then
  for (( _t = 0; _t < SECS; _t += 5 )); do
    grep -aq '\[exit code' "$RUN/app/app_stdout.txt" 2>/dev/null && break
    sleep 5
  done
else
  sleep "$SECS"
fi
[[ -n "$VID" ]] && { kill "$VID" 2>/dev/null; wait "$VID" 2>/dev/null; }
# Alive = the cmd wrapper has not written its "[exit code N]" line yet. The steam-run/proton launcher PID is not
# a reliable signal: it can return while the game keeps running, which used to report a live run as "status 255".
ALIVE=1; grep -aq '\[exit code' "$RUN/app/app_stdout.txt" 2>/dev/null && ALIVE=0
if [[ -z "$XVFB" ]]; then
  # Real display: capture the game's own window only, never the user's screen.
  WID=""
  for q in "--class ${EXE}" "--class ${EXE%.exe}" "--name ${EXE%.exe}" "--name Killzone"; do
    WID="$(xdotool search --onlyvisible $q 2>/dev/null | tail -1)"; [[ -n "$WID" ]] && break
  done
  [[ -n "$WID" ]] && import -window "$WID" "$SHOT" 2>/dev/null && echo "screenshot: $SHOT (game window)"
else
  import -window root "$SHOT" 2>/dev/null && echo "screenshot: $SHOT"
fi
if [[ -n "$VID" && -s "$RUN/clip/times.txt" ]]; then
  python3 "$(dirname "$0")/make_clip.py" "$RUN/clip" "$RUN/app/app_stdout.txt" "${SHOT%.png}.mp4" || echo "clip: failed"
fi
STATUS=0
if [[ $ALIVE -eq 1 ]]; then
  echo "program still running after ${SECS}s (expected for a render loop); stopping it"
else
  wait "$APP" 2>/dev/null
  STATUS="$(grep -ao 'exit code -\?[0-9]*' "$RUN/app/app_stdout.txt" 2>/dev/null | tail -1 | grep -o -- '-\?[0-9]*$')"
  STATUS="${STATUS:-255}"; echo "program exited with status $STATUS"
fi
# Stop Proton/wine for this prefix and Xvfb; everything is inside this sandbox's PID namespace.
STEAM_COMPAT_DATA_PATH="$RUN/compat" steam-run "$PROTON_DIR/files/bin/wineserver" -k >/dev/null 2>&1 || true
kill "$APP" "$XVFB" 2>/dev/null || true
echo "--- program output (tail) ---"; tail -n 30 "$RUN/app/app_stdout.txt" 2>/dev/null || echo "(no output file)"
[[ "$STATUS" =~ ^[0-9]+$ ]] || STATUS=1
exit $(( STATUS > 255 ? 1 : STATUS ))
