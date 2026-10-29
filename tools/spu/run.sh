#!/usr/bin/env bash
# One Proton boot of the staged kz3.exe with the round-7 pad script, metrics afterwards.
#   tools/spu/run.sh NAME SECS [--save-from RUN] [--no-stage]
# --save-from RUN copies work/proton/RUN/app/gamedata (RUN may also be a path to another checkout's run dir) (the savedata the game wrote: cellSaveData's root is
#   ./gamedata/dev_hdd0/home/00000001/savedata relative to the exe dir) into this run's stage, so this boot sees the profile.
# Extra env (PAD_SCRIPT, RSX_PERF, ...) passes through. Takes the GPU lock for the whole run, staging included.
# Staging (SPU pass 52, RSX pass 48's request): queued runs used to stage the exe and copy their save into the shared
# work/kz3-stage BEFORE taking the GPU lock, so a run queued behind another overwrote the waiting run's save (and exe).
# Now the script re-runs itself under the lock; inside it, work/kz3-stage is (re)staged unless --no-stage, then this run
# gets its own dir work/kz3-stage-run/NAME (hard links to the base, plus its own gamedata), removed after the run.
# The gpu lock has two slots, so two runs can stage at once: each only writes its own dir; the base is re-staged by a
# run without --no-stage only (ab-runs.sh stages once and reuses it; a staging run queued in between still replaces it).
# Test hooks (tools/spu/test_run_stage.sh): KZ3_RUN_LOCK (default gpu), KZ3_RUN_STAGE_BASE (default work/kz3-stage;
# set = no staging), KZ3_RUN_CMD (replaces "nix develop --command tools/proton-test.sh"; gets the same arguments).
# KZ3_EBOOT_VER passes through to tools/kz3-stage.sh (it picks the staged EBOOT; default kz3ver.DEFAULT = 114);
# the SPU lift built into the exe must be the same version: a mismatch with the version's lift dir is reported before the run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"
LOCK="${KZ3_RUN_LOCK:-gpu}"
if [[ " ${KZ3_HELD_LOCKS:-} " != *" $LOCK "* ]]; then
  exec tools/agents/lock.sh "$LOCK" -- "$ROOT/tools/spu/run.sh" "$@"
fi
NAME="$1" SECS="$2"; shift 2
FROM="" STAGE=1
while [[ $# -gt 0 ]]; do case "$1" in --save-from) FROM="$2"; shift 2;; --no-stage) STAGE=0; shift;; *) echo "unknown $1" >&2; exit 2;; esac; done
# The lift of this run's version (kz3ver.SPU_OUT_REL: work/spu for 1.14, work/spu-100 for 1.00): it must exist and be that version.
LD="$(python3 -c 'import sys; sys.path.insert(0, "tools/spu"); import kz3ver; print(kz3ver.SPU_OUT_REL)')"
WANT="${KZ3_EBOOT_VER:-$(python3 -c 'import sys; sys.path.insert(0, "tools/spu"); import kz3ver; print(kz3ver.VER)')}"
LV="$(cat "$LD/eboot_ver" 2>/dev/null || { [[ "$WANT" == 100 ]] && echo 100 || echo none; })"   # no file: the old default, v1.00
if [[ "$LV" != "$WANT" ]]; then
  echo "warning: KZ3_EBOOT_VER=$WANT but $LD holds the $LV SPU lift" >&2
fi
BASE="${KZ3_RUN_STAGE_BASE:-work/kz3-stage}"
if [[ $STAGE == 1 && -z "${KZ3_RUN_STAGE_BASE:-}" ]]; then tools/kz3-stage.sh >/dev/null; fi
[[ -f "$BASE/kz3.exe" ]] || { echo "no staged exe in $BASE" >&2; exit 1; }
S="work/kz3-stage-run/$NAME"
[[ -e "$S" ]] && tools/safe_rm.sh "$S"
mkdir -p "$S"
for f in "$BASE"/*; do
  [[ "$(basename "$f")" == gamedata ]] && continue
  cp -al "$f" "$S/" 2>/dev/null || cp -a "$f" "$S/"
done
if [[ -n "$FROM" ]]; then
  SRC="work/proton/$FROM/app/gamedata"; [[ "$FROM" == /* ]] && SRC="$FROM/app/gamedata"
  [[ -d "$SRC" ]] || { echo "no gamedata in run $FROM" >&2; tools/safe_rm.sh "$S"; exit 1; }
  cp -a "$SRC" "$S/"
  echo "save from $FROM: $(find "$S/gamedata" -type f | wc -l) files"
  export KZ3_RUN_SAVE_FROM="$SRC"     # run provenance (tools/runprov.py): the save's source dir in run.json
fi
mkdir -p work/spu
# KZ3_PROTON_ARGS: extra proton-test.sh options, e.g. --real-display (the maintainers' own display only).
read -r -a RUNCMD <<< "${KZ3_RUN_CMD:-nix develop --command tools/proton-test.sh}"
PAD_SCRIPT="${PAD_SCRIPT:-10:0x4000,15:0x4000,20:0x4000,25:0x0008,30:0x4000,40:0x4000,50:0x4000}" GCM_FLIPCOUNT=1 \
  "${RUNCMD[@]}" ${KZ3_PROTON_ARGS:-} --seconds "$SECS" --name "$NAME" \
  "$S/kz3.exe" ./PS3_GAME/USRDIR/EBOOT.elf > "work/spu/run-$NAME.log" 2>&1 && RC=0 || RC=$?
tools/safe_rm.sh "$S"
L="work/proton/$NAME/app/app_stdout.txt"
echo "== $NAME"
if [[ -f "$L" ]]; then python3 tools/kz3_metrics.py "$L" --alive 1 2>&1 | grep -E "flips|last_flip|fps" | head -6; fi
# A crash or a non-zero game exit is a result (exit 0 here); proton-test's 2 (refused before the run: a path switch the
# sandbox cannot use) and 4 (the run could not write a requested output) are not, so they stop ab-runs.sh, level-run.sh
# and any chain at the first such run (2026-10-15: a sweep repeated an unwritable template export for eight chapters).
if [[ $RC == 2 || $RC == 4 ]]; then tail -4 "work/spu/run-$NAME.log" >&2; exit $RC; fi
exit 0
