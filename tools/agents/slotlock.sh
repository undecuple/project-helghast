# shellcheck shell=bash disable=SC2034
# Slot locks (2026-10-11): a shared resource with N slots is N flock files in the main checkout's work/locks/.
# Slot 0 is NAME.lock, the old single lock, so a script that still takes it the old way (a parallel worktree on an older
# main) simply holds slot 0; slot i > 0 is NAME.i.lock. Sourced by tools/agents/lock.sh, tools/build-kz3.sh,
# tools/lift.sh and tools/spu/lift_spu.sh; no game data, no network.
#   slot_file DIR NAME I        prints the slot's path
#   slot_take DIR NAME N        holds one free slot (its fd stays open, so children and an exec'd command keep it);
#                               sets SLOT (0..N-1) and SLOT_FD
#   slot_take_all DIR NAME N    holds every slot, in order 0..N-1 (exclusive use); sets SLOT=all
#   (both export KZ3_HOLDS_NAME=1, so a child build, e.g. tools/agents/shims/ninja, does not queue for a slot
#   its parent already holds)
#   slot_held DIR NAME N        prints how many of the N slots are held right now, by anyone (this process included)
# No deadlock: a shared taker never waits while holding a slot (it rotates `flock -w 2` over the slots), and an
# exclusive taker takes the slots in order. No starvation of the exclusive taker: while it waits it writes its pid
# to NAME.exclusive, and shared takers hold off while that pid is alive (the marker is blanked, not deleted, when it
# is done: parallel worktrees run this inside a sandbox without the main checkout's tools/safe_rm.sh).
# First come, first served (2026-10-12: a parallel run lost every race for a free gpu slot for 35 min): every taker joins
# NAME.queue (one pid per line, edited under NAME.queue.lock; dead pids are dropped on every edit, nothing is deleted)
# and only the oldest live waiter tries the slots. A taker from an older copy of this file ignores the queue and can
# still jump it until its worktree is rebased.
# Waiters are named by a token, not a pid (2026-10-14: every tools/sandbox.sh run has its own pid namespace, so
# `kill -0` on another sandbox's pid failed, two sandboxed builds dropped each other from NAME.queue and both spun on
# an empty queue for an hour). A token is one of 64 files NAME.wJ held with flock while its owner waits; a token is
# alive while it is locked (the kernel drops the lock when the owner dies, in any namespace). No file is ever removed.
# Mixed copies (2026-10-19: KZ2's pid-only copy and this one erased each other's entries; all six waiters spun on an
# empty queue with the GPU idle): live numeric pids are kept, a pid in NAME.exclusive is honoured, and a waiter missing
# from the queue re-joins at the back on its next head check.
slot_file() { if (( $3 == 0 )); then echo "$1/$2.lock"; else echo "$1/$2.$3.lock"; fi; }
slot_tok_alive() { ! flock -n "$1/$2.w$3" true 2>/dev/null; }   # DIR NAME J: is token J held by a waiter?
# Shortest declared job first (2026-10-19, "Queue order"): on while NAME.sjf exists in the
# lock directory (one shared switch, so every waiter agrees; its "age S" line sets the aging limit, default 1200).
# A waiter declares LOCK_EST_MIN (minutes; lock.sh fills it from --seconds / level-run.sh, else slot_est's default) and
# its project (LOCK_PROJECT, else from the checkout path); both go into its token file NAME.wJ as "EST JOIN PROJECT".
# Order: waiters older than the aging limit first (oldest first), then by estimate in 5-minute steps, then Killzone 3
# before others in the same step (the tiebreak), then arrival. The queue file is rewritten in that order, so a copy
# that only reads the first line agrees. Nothing running is ever interrupted; reservations stay in lock.sh, above it.
slot_est() {   # slot_est NAME: the declared minutes, or the default for NAME
  local e="${LOCK_EST_MIN:-}"
  [[ "$e" =~ ^[0-9]+$ && "$e" -gt 0 ]] && { echo "$e"; return; }
  case "$1" in build) echo 20 ;; gpu) echo 30 ;; *) echo 10 ;; esac
}
slot_proj() {   # the waiter's project: LOCK_PROJECT, else KZ2's marker, else the checkout path
  [[ -n "${LOCK_PROJECT:-}" ]] && { echo "$LOCK_PROJECT"; return; }
  [[ -n "${KZ2_HELD_LOCKS:-}" ]] && { echo kz2; return; }
  case "$PWD" in */"Killzone 2"*|*/kz2-*) echo kz2 ;; */Ratchet*|*/rc-*) echo rc ;;
    */"Toro Lets Party"*|*/tlp-*|*/toro-*) echo toro ;; *) echo kz3 ;; esac
}
slot_key() {   # slot_key DIR NAME ENTRY NOW AGE: sort key of a live queue entry
  local est join proj
  if [[ "$3" =~ ^t[0-9]+$ ]]; then read -r est join proj < "$1/$2.w${3#t}" 2>/dev/null || true; fi
  [[ "$est" =~ ^[0-9]+$ ]] || est=$(LOCK_EST_MIN='' slot_est "$2")
  [[ "$join" =~ ^[0-9]+$ ]] || join=$4
  if (( $4 - join >= $5 )); then printf '0 %012d 0 0\n' "$join"
  else printf '1 %04d %d %012d\n' $(( (est + 4) / 5 )) "$([[ "${proj:-kz3}" == kz3 ]] && echo 0 || echo 1)" "$join"; fi
}
slot_tok() {   # slot_tok DIR NAME: hold a free token; sets SLOT_TOK (its number) and SLOT_TOK_FD
  local j fd
  while :; do
    for ((j = 0; j < 64; j++)); do
      exec {fd}>>"$1/$2.w$j"
      if flock -n "$fd"; then SLOT_TOK=$j; SLOT_TOK_FD=$fd
        printf '%s %s %s\n' "$(slot_est "$2")" "$(date +%s)" "$(slot_proj)" > "$1/$2.w$j"; return 0; fi
      exec {fd}>&-
    done
    sleep 1
  done
}
# Run history (2026-10-19: record the actual time against the estimate): NAME.history gets "start EPOCH SLOT EST
# PROJECT PID" when a slot is taken and "end EPOCH PID" when that process exits (a small watcher in the holder's own
# pid namespace, with every inherited descriptor closed so it never holds a slot or a pipe; a sandbox exit kills it, so
# readers also end a run when a later run takes its slot). Read by watchdog.sh.
slot_hist() {   # slot_hist DIR NAME SLOT
  local pid=$$ f="$1/$2.history"
  printf 'start %s %s %s %s %s\n' "$(date +%s)" "$3" "$(slot_est "$2")" "$(slot_proj)" "$pid" >> "$f" 2>/dev/null || return 0
  ( for d in /proc/$BASHPID/fd/*; do d=${d##*/}; (( d > 2 )) && eval "exec $d>&-"; done
    while kill -0 "$pid" 2>/dev/null; do sleep 5; done
    printf 'end %s %s\n' "$(date +%s)" "$pid" >> "$f" ) < /dev/null > /dev/null 2>&1 &
  disown $! 2>/dev/null || true
}
slot_tok_drop() { [[ -n "${SLOT_TOK_FD:-}" ]] && exec {SLOT_TOK_FD}>&-; SLOT_TOK=; SLOT_TOK_FD=; }
slot_excl_waiting() {   # is another process waiting for exclusive use of NAME?
  local t; t="$(cat "$1/$2.exclusive" 2>/dev/null || true)"
  # A marker is written only after its holder left the queue (slot_take_all), so a marked token that is queued again is
  # a new waiter that reused the number of a holder killed before it cleared the marker: stale (2026-10-20: KZ2's
  # head waited for "t1" while KZ3's queued t1 waited for the head, 4 runs stuck with the GPU idle).
  if [[ "$t" =~ ^t[0-9]+$ ]]; then [[ "$t" != "t${SLOT_TOK:-x}" ]] && ! grep -qx "$t" "$1/$2.queue" 2>/dev/null \
      && slot_tok_alive "$1" "$2" "${t#t}"
  else [[ "$t" =~ ^[0-9]+$ && "$t" != "$$" ]] && kill -0 "$t" 2>/dev/null; fi   # an old copy's pid marker
}
slot_q() {   # slot_q DIR NAME join|leave|head : edit the waiting queue under its lock; head prints the oldest live token
  local dir="$1" name="$2" op="$3" q="$1/$2.queue" fd p keep="" me="t$SLOT_TOK"
  exec {fd}>"$q.lock"; flock "$fd"
  [[ -f "$q" ]] || : > "$q"
  while read -r p; do
    if [[ "$p" =~ ^t[0-9]+$ ]]; then slot_tok_alive "$dir" "$name" "${p#t}" || continue
    elif [[ "$p" =~ ^[0-9]+$ ]]; then kill -0 "$p" 2>/dev/null || continue   # an old copy's pid entry: keep it
    else continue; fi
    [[ "$op" == leave && "$p" == "$me" ]] && continue
    [[ " $keep " == *" $p "* ]] || keep+="$p "
  done < "$q"
  # head re-joins too: a waiter whose entry another copy erased takes its place at the back instead of spinning.
  [[ "$op" != leave && " $keep " != *" $me "* ]] && keep+="$me "
  if [[ -f "$dir/$name.sjf" && -n "$keep" ]]; then
    local now age e; now=$(date +%s); age=$(awk '$1 == "age" && $2 ~ /^[0-9]+$/ {print $2}' "$dir/$name.sjf"); age=${age:-1200}
    keep="$(for e in $keep; do printf '%s %s\n' "$(slot_key "$dir" "$name" "$e" "$now" "$age")" "$e"; done | sort -s -k1,4 | awk '{printf "%s ", $5}')"
  fi
  printf '%s\n' $keep > "$q.new"; [[ -n "$keep" ]] || : > "$q.new"; mv "$q.new" "$q"
  [[ "$op" == head ]] && head -1 "$q"
  exec {fd}>&-
}
slot_take() {
  local dir="$1" name="$2" n="$3" i fd wait=0
  slot_tok "$dir" "$name"
  slot_q "$dir" "$name" join
  while :; do
    if slot_excl_waiting "$dir" "$name"; then sleep 2; continue; fi
    if [[ "$(slot_q "$dir" "$name" head)" != "t$SLOT_TOK" ]]; then sleep 0.3; continue; fi
    # First pass without waiting (a free slot is taken at once), then rotate with a 2 s wait per slot.
    for ((i = 0; i < n; i++)); do
      # Still the head? A shorter job may have joined while this one waited on a slot (NAME.sjf order).
      (( wait )) && [[ -f "$dir/$name.sjf" && "$(slot_q "$dir" "$name" head)" != "t$SLOT_TOK" ]] && break
      exec {fd}>"$(slot_file "$dir" "$name" "$i")"
      if flock -w "$wait" "$fd"; then SLOT=$i; SLOT_FD=$fd; slot_q "$dir" "$name" leave; slot_tok_drop
        slot_hist "$dir" "$name" "$i"
        export "KZ3_HOLDS_${name//[^A-Za-z0-9]/_}=1"; return 0; fi
      exec {fd}>&-
    done
    wait=$([[ -f "$dir/$name.sjf" ]] && echo 1 || echo 2)
  done
}
slot_take_all() {
  local dir="$1" name="$2" n="$3" i fd
  if (( n > 1 )); then
    slot_tok "$dir" "$name"
    slot_q "$dir" "$name" join
    while slot_excl_waiting "$dir" "$name" || [[ "$(slot_q "$dir" "$name" head)" != "t$SLOT_TOK" ]]; do sleep 2; done
    echo "t$SLOT_TOK" > "$dir/$name.exclusive"   # the token stays held until every slot is taken
    slot_q "$dir" "$name" leave
  fi
  for ((i = 0; i < n; i++)); do
    exec {fd}>"$(slot_file "$dir" "$name" "$i")"
    flock "$fd"
  done
  if (( n > 1 )); then : > "$dir/$name.exclusive"; slot_tok_drop; fi
  slot_hist "$dir" "$name" all
  SLOT=all; export "KZ3_HOLDS_${name//[^A-Za-z0-9]/_}=1"
}
slot_held() {
  local dir="$1" name="$2" n="$3" i c=0
  for ((i = 0; i < n; i++)); do flock -n "$(slot_file "$dir" "$name" "$i")" true 2>/dev/null || c=$((c + 1)); done
  echo "$c"
}
