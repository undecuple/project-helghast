#!/usr/bin/env bash
# Stage build/kz3-win/kz3.exe (or $KZ3_STAGE_EXE: another build's exe, e.g. a toolchain trial) for a Proton run in work/kz3-stage/:
#   kz3.exe, PS3_GAME/USRDIR/EBOOT.elf (copy of work/bin/EBOOT-114.elf, plus dev_hdd0/game/BCUS98234 -> GameUpdate, the
#   installed update where a PS3 keeps it; KZ3_EBOOT_VER=100: of work/bin/EBOOT.elf, no update), and symlinks to every other disc file
#   (Wine follows them; the sandbox mounts Game/ read-only). The VFS root is the stage dir, so run with the
#   relative ELF path:
#
#   tools/kz3-stage.sh
#   tools/agents/lock.sh gpu -- nix develop --command tools/proton-test.sh --seconds 30 --name kz3-boot \
#     work/kz3-stage/kz3.exe ./PS3_GAME/USRDIR/EBOOT.elf
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
G="$(readlink -f Game)/PS3_GAME"
S=work/kz3-stage
EXE="${KZ3_STAGE_EXE:-build/kz3-win/kz3.exe}"
# EBOOT version: tools/spu/kz3ver.py DEFAULT (114, the update's, since series step 6 of the RE notes s.36.4);
# KZ3_EBOOT_VER=100 the disc one. It must match the exe's build (a 1.00 exe: KZ3_EBOOT_VER=100 here too).
VER="$(python3 tools/spu/kz3ver.py ver)" || exit 2
case "$VER" in 100) EB=work/bin/EBOOT.elf ;; 114) EB=work/bin/EBOOT-114.elf ;; *) echo "KZ3_EBOOT_VER must be 100 or 114" >&2; exit 2 ;; esac
for f in "$EXE" "$EB" "$G/USRDIR/data_ps3.psarc"; do
  [[ -e "$f" ]] || { echo "missing $f" >&2; exit 1; }
done
if [[ -e "$S" ]]; then "$ROOT/tools/safe_rm.sh" "$S"; fi
mkdir -p "$S/PS3_GAME/USRDIR"
cp "$EXE" "$S/kz3.exe"
cp "$EB" "$S/PS3_GAME/USRDIR/EBOOT.elf"
# The port's UI fonts (ui-style.md "Fonts"; main.cpp loads <exe dir>\fonts, the installer ships them): without them a
# Proton run falls back to Wine's serif face (RSX pass 89). Fetched once by tools/release/installer_art.py --fetch-fonts (OFL).
FD=work/release/fonts   # a parallel worktree uses the main checkout's copy (.kz3-main)
[[ -d "$FD" || ! -f .kz3-main ]] || FD="$(cat .kz3-main)/work/release/fonts"
ttfs=("$FD"/*.ttf)   # a glob, not compgen: nixpkgs' non-interactive bash has no compgen
if [[ -f "${ttfs[0]}" ]]; then
  mkdir -p "$S/fonts" && cp "$FD"/*.ttf "$FD"/OFL-*.txt "$S/fonts/"
else
  echo "kz3-stage: no fonts in work/release/fonts (tools/release/installer_art.py --fetch-fonts): UI text falls back" >&2
fi
for f in "$G"/*; do [[ "$(basename "$f")" == USRDIR ]] || ln -s "$f" "$S/PS3_GAME/"; done
for f in "$G"/USRDIR/*; do [[ "$(basename "$f")" == EBOOT.BIN ]] || ln -s "$f" "$S/PS3_GAME/USRDIR/"; done
# 1.14: the installed update, read by the data overlay (psarc_vfs.cpp, port/vfs; wired on main in series step 4).
if [[ "$VER" == 114 ]]; then
  [[ -e GameUpdate ]] || { echo "missing GameUpdate (install the 1.14 update, see docs/requirements.md)" >&2; exit 1; }
  # the update sits under the disc's title id (US BCUS98234, EU BCES01007: untested region), read from its PARAM.SFO
  TID="$(python3 -c 'import sys,re;d=open(sys.argv[1],"rb").read(65536);m=re.search(rb"TITLE_ID.*?BC[A-Z]{2}[0-9]{5}",d,re.S);print(m.group(0)[-9:].decode() if m else "BCUS98234")' "$G/PARAM.SFO" 2>/dev/null || echo BCUS98234)"
  mkdir -p "$S/dev_hdd0/game" && ln -s "$(readlink -f GameUpdate)" "$S/dev_hdd0/game/$TID"
fi
echo "staged $S ($VER)"
