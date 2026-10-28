#!/usr/bin/env bash
# Run a command inside a bubblewrap sandbox.
#
#   tools/sandbox.sh [--gpu] [--net] [--ro PATH]... -- <command> [args...]
#
# Inside the sandbox:
#   - /nix (store) is read-only, the game disc ($KZ3_ROOT/Game) is read-only,
#   - only $KZ3_ROOT/work and $KZ3_ROOT/build are writable,
#   - the rest of the repo is read-only, $HOME is an empty tmpfs,
#   - no network (unless --net), no access to the user's display, audio or sessions,
#   - --gpu exposes the NVIDIA device nodes + /run/opengl-driver (for Vulkan/RPCS3 runs).
#   - --ro PATH binds an extra host path read-only at the same location (e.g. a Proton install).
#   - --x11 (developer use only) exposes the user's X display :0
#     (Xwayland socket) as $KZ3_REAL_DISPLAY; still no network, no audio, no D-Bus.
#   - --discord (developer use only; temporary test use, 2026-10-22) binds the user's Discord IPC socket
#     ($XDG_RUNTIME_DIR/discord-ipc-0) and nothing else of $XDG_RUNTIME_DIR, for the Rich Presence test through
#     wine-discord-ipc-bridge (tools/proton-test.sh --discord).
# Run it from inside `nix develop` so the tools are on PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GPU=0 NET=0
EXTRA_RO=()
LANE_RO=()
X11=0
DISCORD=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --gpu) GPU=1; shift ;;
    --net) NET=1; shift ;;
    --ro) EXTRA_RO+=("$2"); shift 2 ;;
    --x11) X11=1; shift ;;
    --discord) DISCORD=1; shift ;;
    --) shift; break ;;
    *) break ;;
  esac
done
[[ $# -gt 0 ]] || { echo "usage: $0 [--gpu] [--net] -- cmd..." >&2; exit 2; }

# macOS build hosts (docs/BUILDING.md): bubblewrap is Linux-only, so the command runs directly, from the repo root,
# with the same KZ3_* paths. The game dump stays untouched because the build only reads it; lanes never run here.
if [[ "$(uname -s)" == Darwin ]]; then
  [[ -e "$ROOT/.kz3-lane" ]] && { echo "sandbox.sh: lane worktrees need the Linux sandbox" >&2; exit 1; }
  echo "sandbox.sh: macOS, no bubblewrap: running without a sandbox" >&2
  cd "$ROOT" && export KZ3_ROOT="$ROOT" KZ3_GAME="$ROOT/Game" KZ3_WORK="$ROOT/work" && exec "$@"
fi

# Lane profile (least privilege). In a lane worktree (.kz3-lane exists) the policy comes from the MAIN checkout's
# tools/agents/lanes.json, which lanes cannot edit: never network; GPU and extra read-only binds only for lanes
# with "gpu": true. Fails closed if the policy cannot be read.
if [[ -f "$ROOT/.kz3-lane" ]]; then
  LANE="$(cat "$ROOT/.kz3-lane")"
  POLICY_MAIN="$(cat "$ROOT/.kz3-main" 2>/dev/null || true)"
  LANE_GPU="$(python3 -c 'import json,sys; print(int(bool(json.load(open(sys.argv[1]))["lanes"][sys.argv[2]]["gpu"])))' \
               "$POLICY_MAIN/tools/agents/lanes.json" "$LANE" 2>/dev/null || echo 0)"
  [[ $NET -eq 1 ]] && { echo "sandbox: lane '$LANE' may not use --net" >&2; exit 3; }
  [[ $X11 -eq 1 ]] && { echo "sandbox: lane '$LANE' may not use the user's display (--x11)" >&2; exit 3; }
  [[ $DISCORD -eq 1 ]] && { echo "sandbox: lane '$LANE' may not use the Discord socket (--discord)" >&2; exit 3; }
  if [[ $GPU -eq 1 || ${#EXTRA_RO[@]} -gt 0 ]] && [[ "$LANE_GPU" != 1 ]]; then
    echo "sandbox: lane '$LANE' may not use --gpu or extra binds" >&2; exit 3
  fi
  # read_dirs (2026-10-16, the kz2 lane: KZ3 agents doing static work for the Killzone 2 port): host folders the
  # lane reads but never writes, bound read-only from the policy (never from the lane's own arguments).
  while IFS= read -r d; do [[ -n "$d" && -d "$d" ]] && LANE_RO+=("$d"); done < <(python3 -c 'import json,sys
for d in json.load(open(sys.argv[1]))["lanes"][sys.argv[2]].get("read_dirs", []): print(d)' "$POLICY_MAIN/tools/agents/lanes.json" "$LANE" 2>/dev/null)
fi

mkdir -p "$ROOT/work" "$ROOT/build"

args=(
  --die-with-parent --new-session
  --unshare-all
  --ro-bind /nix /nix
  --ro-bind /etc/resolv.conf /etc/resolv.conf
  --ro-bind-try /etc/ssl /etc/ssl
  --ro-bind-try /etc/static/ssl /etc/static/ssl
  --ro-bind /run/current-system/sw /run/current-system/sw
  --ro-bind-try /usr/bin/env /usr/bin/env
  --ro-bind-try /bin/sh /bin/sh
  --proc /proc --dev /dev --tmpfs /tmp
  --tmpfs "$HOME"
  --ro-bind "$ROOT" "$ROOT"
  --bind "$ROOT/work" "$ROOT/work"
  --bind "$ROOT/build" "$ROOT/build"
  --chdir "$ROOT"
  --setenv HOME "$HOME"
  --setenv KZ3_ROOT "$ROOT"
  --setenv KZ3_GAME "$ROOT/Game"
  --setenv KZ3_WORK "$ROOT/work"
  --unsetenv DISPLAY --unsetenv WAYLAND_DISPLAY
  --unsetenv PULSE_SERVER --unsetenv DBUS_SESSION_BUS_ADDRESS
  --setenv PATH "$ROOT/tools/agents/shims:$PATH"   # ninja shim: every build takes a build slot (tools/agents/shims/ninja)
  --setenv TMPDIR /tmp --setenv TMP /tmp --setenv TEMP /tmp --setenv TEMPDIR /tmp
)
# Game is read-only. In lane worktrees (and when it lives on another disk) Game is a symlink: bwrap cannot
# mount over a symlink, so bind the real target read-only at its own path and let the link resolve.
if [[ -L "$ROOT/Game" ]]; then
  G="$(readlink -f "$ROOT/Game")"; [[ -e "$G" ]] && args+=(--ro-bind "$G" "$G")
elif [[ -e "$ROOT/Game" ]]; then
  args+=(--ro-bind "$ROOT/Game" "$ROOT/Game")
fi
# GameUpdate: link to the installed game update (RPCS3 dev_hdd0/game/BCUS98234), read-only like Game.
if [[ -L "$ROOT/GameUpdate" ]]; then
  U="$(readlink -f "$ROOT/GameUpdate")"; [[ -e "$U" ]] && args+=(--ro-bind "$U" "$U") && args+=(--setenv KZ3_UPDATE "$ROOT/GameUpdate")
fi
if [[ $DISCORD -eq 1 ]]; then
  XRD="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  [[ -S "$XRD/discord-ipc-0" ]] || { echo "sandbox: no Discord socket at $XRD/discord-ipc-0 (is Discord running?)" >&2; exit 2; }
  args+=(--bind "$XRD/discord-ipc-0" "$XRD/discord-ipc-0" --setenv XDG_RUNTIME_DIR "$XRD")
fi
if [[ $NET -eq 1 ]]; then
  args+=(--share-net)
  # Inside the steam-run FHS shell (`nix develop`), /etc/resolv.conf and /etc/ssl point into /.host-etc, which is not
  # mounted here, so DNS and the CA bundle were unreachable (telemetry trial 2026-10-13: WinHTTP 12157). Only with
  # --net, and only these three read-only paths of the host's /etc.
  [[ -d /.host-etc ]] && args+=(--ro-bind-try /.host-etc/resolv.conf /.host-etc/resolv.conf
                                --ro-bind-try /.host-etc/ssl /.host-etc/ssl --ro-bind-try /.host-etc/static/ssl /.host-etc/static/ssl)
fi
# Lane worktrees (tools/agents/new-lane.sh): the main checkout's work/ is shared read-only (decrypted EBOOT,
# analysis, lifted code via symlinks), its work/locks stays writable for flock.
if [[ -f "$ROOT/.kz3-main" ]]; then
  MAIN="$(cat "$ROOT/.kz3-main")"
  if [[ -d "$MAIN/work" && "$MAIN" != "$ROOT" ]]; then
    mkdir -p "$MAIN/work/locks"
    args+=(--ro-bind "$MAIN/work" "$MAIN/work" --bind "$MAIN/work/locks" "$MAIN/work/locks" --setenv KZ3_MAIN "$MAIN")
    # A worktree's .git is a file pointing into the main checkout's .git; bind it read-only so git works inside the
    # sandbox (tools/ps3recomp-patched.sh's git apply, tools/checkpoint.py's commit stamp). Nothing can be written.
    [[ -d "$MAIN/.git" ]] && args+=(--ro-bind "$MAIN/.git" "$MAIN/.git")
  fi
fi
# Static data on a slower drive: work/extracted and work/move-when-done may be links into one approved folder, named
# (resolved, one line) in ~/.config/kz3/static-root, outside the repo. Bind only those two targets, read-only, so the
# links resolve; nothing else on that drive. No file, no static binds.
STATIC_ROOT="$(head -n1 "$HOME/.config/kz3/static-root" 2>/dev/null || true)"
STATIC_SEEN=" "
for w in "$ROOT/work" "${MAIN:+$MAIN/work}"; do
  for n in extracted move-when-done; do
    [[ -n "$w" && -L "$w/$n" ]] || continue
    T="$(readlink -f "$w/$n")"
    [[ -n "$STATIC_ROOT" && "$T" == "$STATIC_ROOT/$n" && -d "$T" && "$STATIC_SEEN" != *" $T "* ]] || continue
    args+=(--ro-bind "$T" "$T"); STATIC_SEEN+="$T "
  done
done
for p in "${EXTRA_RO[@]}"; do args+=(--ro-bind "$(readlink -f "$p")" "$p"); done
for p in "${LANE_RO[@]}"; do args+=(--ro-bind "$p" "$p"); done
if [[ $X11 -eq 1 ]]; then
  [[ -S /tmp/.X11-unix/X0 ]] || { echo "sandbox: no X display socket /tmp/.X11-unix/X0" >&2; exit 2; }
  args+=(--bind /tmp/.X11-unix/X0 /tmp/.X11-unix/X0 --setenv KZ3_REAL_DISPLAY :0)
  # The user's game controller for real-display play sessions (developer use only, like --x11): the evdev/joystick
  # nodes and the udev database Wine's winebus (SDL) enumerates them with. Xbox pads on xpad have no hidraw node.
  [[ -d /dev/input ]] && args+=(--dev-bind /dev/input /dev/input)
  args+=(--ro-bind-try /run/udev /run/udev)
fi
if [[ $GPU -eq 1 ]]; then
  args+=(--ro-bind /run/opengl-driver /run/opengl-driver)
  for d in /dev/nvidia* /dev/dri; do [[ -e $d ]] && args+=(--dev-bind "$d" "$d"); done
  args+=(--ro-bind-try /sys /sys)
fi

exec bwrap "${args[@]}" -- "$@"
