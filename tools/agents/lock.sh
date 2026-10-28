#!/usr/bin/env bash
# Run a command while holding a shared lock (works across all parallel worktrees).
#   tools/agents/lock.sh NAME [--exclusive] -- CMD [ARGS...]      NAME: gpu | ghidra | extract | ...
# Locks live in the main checkout's work/locks/ (see the lock notes section 3).
# The gpu lock has slots (2026-10-11: two boot runs side by side each kept the solo speed, see
# the testing notes "Two runs on the GPU"): KZ3_GPU_SLOTS runs (default 2) hold it at once, one
# slot each (slot 0 = gpu.lock, slot 1 = gpu.1.lock; tools/agents/slotlock.sh). --exclusive, or KZ3_GPU_EXCLUSIVE=1
# in the environment, takes every slot: for anything that measures speed (tools/kz3-check.sh's fps baseline,
# tools/spu/cull_ab.sh, any runcmp --purpose perf/ab pair), since a neighbour skews it. The command sees
# KZ3_GPU_SLOT (0, 1, ... or all) and KZ3_GPU_EXCLUSIVE (1/0); runprov records them in run.json and runcmp refuses a
# perf/ab comparison of a run that shared. KZ3_GPU_SLOTS=1 restores one run at a time. Other names have one slot.
set -euo pipefail
NAME="${1:?usage: lock.sh NAME [--exclusive] -- CMD...}"; shift
EXCL=0; [[ "${1:-}" == "--exclusive" ]] && { EXCL=1; shift; }
[[ "${1:-}" == "--" ]] && shift
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN="$ROOT"; [[ -f "$ROOT/.kz3-main" ]] && MAIN="$(cat "$ROOT/.kz3-main")"
mkdir -p "$MAIN/work/locks"
# Re-entrant: a command already running under this lock (KZ3_HELD_LOCKS lists the holder's locks, inherited by its
# children) runs straight away. Without this, lock.sh gpu around kz3-check.sh (which takes gpu itself) deadlocked
# every worktree's GPU runs (RSX pass 22, 2026-10-09). An inner --exclusive cannot widen an outer shared slot.
[[ " ${KZ3_HELD_LOCKS:-} " == *" $NAME "* ]] && exec "$@"
# GPU priority (2026-10-11): while the maintainer holds a reservation (tools/agents/gpu-reserve.sh on), other worktrees'
# GPU runs wait here before queueing on the lock, so a milestone run (the 1.14 comparison, a merge boot check) goes next.
# The maintainer's own runs pass KZ3_GPU_PRIO=1.
# A reservation expires (its "expires EPOCH" line; an old file without one: 120 min after it was written), so a
# maintainer that died while holding it cannot stall the other worktrees.
RES="$MAIN/work/locks/gpu.reserved"; T0=$(date +%s)
res_live() {
  [[ -f "$RES" ]] || return 1
  local e; e=$(awk '/^expires /{print $2}' "$RES" 2>/dev/null)
  [[ -n "$e" ]] || e=$(( $(stat -c %Y "$RES" 2>/dev/null || echo 0) + 7200 ))
  (( $(date +%s) < e ))
}
if [[ "$NAME" == gpu && "${KZ3_GPU_PRIO:-0}" != 1 ]] && res_live; then
  echo "lock.sh: GPU reserved by the maintainer ($(head -1 "$RES")); waiting" >&2
  while res_live; do sleep "${KZ3_RES_POLL_S:-10}"; done
  [[ -f "$RES" ]] && echo "lock.sh: the GPU reservation expired; going ahead" >&2
fi
# KZ2 milestone priority (the KZ2 side reaches its next milestone, then priority returns): while
# the KZ2 maintainer's work/locks/kz2-priority file exists, a KZ3 GPU run waits here as long as a KZ2 run is
# queued (a gpu.queue token whose gpu.kz2.wJ marker is held, or a pid running in the KZ2 checkout or a kz2-* worktree, or with KZ2_HELD_LOCKS). Our reservation and the maintainer's own runs
# (KZ3_GPU_PRIO=1) are unaffected; runs already holding a slot are never interrupted. KZ2 deletes the file at its milestone.
kz2_waiting() {
  local p
  for p in $(cat "$MAIN/work/locks/gpu.queue" 2>/dev/null); do
    # A token tJ is KZ2's while KZ2's lock.sh holds gpu.kz2.wJ beside it (KZ2 3a8eebc, 2026-10-19).
    if [[ "$p" =~ ^t[0-9]+$ ]]; then ! flock -n "$MAIN/work/locks/gpu.kz2.w${p#t}" true 2>/dev/null && return 0; continue; fi
    case "$(readlink "/proc/$p/cwd" 2>/dev/null)" in "$(dirname "$MAIN")/Killzone 2"*|*/kz2-*) return 0 ;; esac
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -q '^KZ2_HELD_LOCKS=' && return 0
  done
  return 1
}
if [[ "$NAME" == gpu && "${KZ3_GPU_PRIO:-0}" != 1 && -f "$MAIN/work/locks/kz2-priority" ]] && kz2_waiting; then
  echo "lock.sh: KZ2 milestone priority ($(head -1 "$MAIN/work/locks/kz2-priority")); waiting for queued KZ2 runs" >&2
  while [[ -f "$MAIN/work/locks/kz2-priority" ]] && kz2_waiting; do sleep "${KZ3_RES_POLL_S:-10}"; done
fi
N=1
if [[ "$NAME" == gpu ]]; then
  N="${KZ3_GPU_SLOTS:-2}"; [[ "$N" =~ ^[1-9][0-9]*$ ]] || N=1
  [[ "${KZ3_GPU_EXCLUSIVE:-0}" == 1 ]] && EXCL=1
fi
# The run's declared minutes for the queue order (slotlock.sh): LOCK_EST_MIN, else the game seconds from the command
# (proton-test.sh --seconds N, level-run.sh NAME SECS) plus 3 minutes for staging and the Proton start.
if [[ -z "${LOCK_EST_MIN:-}" ]]; then
  _a=("$@")
  for ((_i = 0; _i < ${#_a[@]}; _i++)); do
    if [[ "${_a[_i]}" == --seconds && "${_a[_i+1]:-}" =~ ^[0-9]+$ ]]; then LOCK_EST_MIN=$(( _a[_i+1] / 60 + 3 )); break; fi
    if [[ "${_a[_i]}" == *level-run.sh && "${_a[_i+2]:-}" =~ ^[0-9]+$ ]]; then LOCK_EST_MIN=$(( _a[_i+2] / 60 + 3 )); break; fi
    # k4warm.sh NAME SECS (rsx K4 runs): + 2 min for its warm-cache staging (warmprep.sh)
    if [[ "${_a[_i]}" == *k4warm.sh && "${_a[_i+2]:-}" =~ ^[0-9]+$ ]]; then LOCK_EST_MIN=$(( _a[_i+2] / 60 + 5 )); break; fi
  done
  # An exclusive GPU run holds the slots through its quiet-machine wait (below), which a build next to it stretches:
  # RSX pass 86's 200 s k4warm runs held them 15 min against a derived 6 (2026-10-20). Count 6 more minutes for that wait.
  if [[ -n "${LOCK_EST_MIN:-}" && "$NAME" == gpu && ( "${KZ3_GPU_EXCLUSIVE:-0}" == 1 || "${KZ3_GPU_SLOTS:-2}" == 1 ) ]]; then
    LOCK_EST_MIN=$(( LOCK_EST_MIN + 6 ))
  fi
  [[ -n "${LOCK_EST_MIN:-}" ]] && export LOCK_EST_MIN
fi
# shellcheck source=slotlock.sh
source "$ROOT/tools/agents/slotlock.sh"
export KZ3_HELD_LOCKS="${KZ3_HELD_LOCKS:-} $NAME"
if (( EXCL || N <= 1 )); then
  slot_take_all "$MAIN/work/locks" "$NAME" "$N"
else
  slot_take "$MAIN/work/locks" "$NAME" "$N"
fi
# Quiet machine for exclusive GPU runs (SPU pass 74, 2026-10-15: two of three A/B pairs ran next to a build, one of them a
# release first-run build that held no build lock): wait until no build lock is held, no compiler/linker/lift process
# runs and the 1-minute load is below KZ3_QUIET_LOAD (default 6), up to KZ3_QUIET_MAX_S (default 3600; 0 = no wait).
# KZ3_QUIET=1 when reached, 0 when the run started anyway (runcmp refuses perf/ab readings with 0).
if [[ "$NAME" == gpu ]] && (( EXCL || N <= 1 )); then
  QT0=$(date +%s); QMAX="${KZ3_QUIET_MAX_S:-3600}"; KZ3_QUIET=0
  while :; do
    busy=0
    for l in "$MAIN"/work/locks/build.lock "$MAIN"/work/locks/build.[0-9].lock; do
      [[ -e "$l" ]] && ! flock -n "$l" true && busy=1
    done
    pgrep -x "${KZ3_QUIET_PROCS:-ninja|ninja.exe|cc1|cc1plus|clang|clang\+\+|ld|ld.lld|lld}" >/dev/null 2>&1 && busy=1
    (( busy == 0 )) && awk -v m="${KZ3_QUIET_LOAD:-6}" '{exit !($1 < m)}' /proc/loadavg && { KZ3_QUIET=1; break; }
    (( $(date +%s) - QT0 >= QMAX )) && break
    sleep $(( QMAX < 30 ? 1 : 15 ))
  done
  export KZ3_QUIET KZ3_QUIET_WAIT_S=$(( $(date +%s) - QT0 ))
fi
if [[ "$NAME" == gpu ]]; then
  # KZ3_GPU_WAIT_S: seconds spent waiting for the reservation and the slots (runprov records it in run.json).
  export KZ3_GPU_SLOT="$SLOT" KZ3_GPU_EXCLUSIVE=$(( EXCL || N <= 1 ? 1 : 0 )) KZ3_GPU_WAIT_S=$(( $(date +%s) - T0 ))
fi
exec "$@"
