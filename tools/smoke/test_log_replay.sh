#!/usr/bin/env bash
# Host test for KZ3 patch 2171: the Level 1 replay harness (tools/smoke/test_log_replay.c) over a SYNTHETIC capture built here
# (no game data; captures are never committed). Checks: the walker's own capture of the stream replays to the same event
# stream (seq/method/arg, per-draw register digest, read ranges with byte digests, guest writes); the same capture replayed
# twice gives identical streams; the adversarial mode flags exactly the planted late read; negative controls (a dropped WORD
# record, a flipped RANGE byte, two reordered records) are all caught; the reader-side RANGE check (gap 6). SKIP without 2171.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_log_replay.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
source "$ROOT/tools/smoke/pub_srcs.inc"; pub_src_args "$V" "$PT/libs/kzr"   # patch 2197: the harness links the publisher
if [[ ! -f "$V/cellGcmSys.c" || ! -d "$PT/include" ]]; then echo "[test_log_replay] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
if ! grep -q rsx_fc_range_check "$V/rsx_fifo_capture.h" 2>/dev/null; then echo "[test_log_replay] SKIP: toolkit without patch 2171"; exit 77; fi
O="$ROOT/build/smoke-host/log_replay"; rm -rf "$O"; mkdir -p "$O/work"
CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
EXTRA_SRC=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA_SRC=("$V/rsx_tiling.c")
gcc "${CF[@]}" "$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" "$V/rsx_commands.c" "${PUB_SRC[@]}" "${EXTRA_SRC[@]}" -o "$O/t" || { echo "[test_log_replay] FAIL: build"; exit 1; }
T="$O/t"; W="$O/work"; rc=0
fail() { echo "[test_log_replay] FAIL: $*"; rc=1; }
run() { local name="$1"; shift; "$@" >"$O/$name.out" 2>"$O/$name.err"; echo $? >"$O/$name.rc"; }
rcof() { cat "$O/$1.rc"; }

run gen "$T" gen "$W/cap";                      [[ "$(rcof gen)" == 0 ]] || { fail "gen: $(tail -2 "$O/gen.out")"; }
CAP="$W/cap/fifo-capture.kz3fc"; [[ -f "$CAP" ]] || { fail "no capture written"; exit 1; }
run chk "$T" check "$CAP";                      [[ "$(rcof chk)" == 0 ]] || fail "check on a clean capture: $(cat "$O/chk.out")"

# 0. pass 157: the shadow page table must hold more pages than the old fixed 16384 slots (the in-level capture overfilled it and
#    the lookup spun); the control runs the old fixed table with bounded probes and must report the overflow
run smt "$T" smtest;                            [[ "$(rcof smt)" == 0 ]] || fail "shadow table with 40000 pages: $(cat "$O/smt.out")"
run smo "$T" smtest old;                        [[ "$(rcof smo)" != 0 ]] || fail "negative control: the old fixed table was NOT detected: $(cat "$O/smo.out")"
echo "[test_log_replay] shadow table: $(cat "$O/smt.out") | control: $(cat "$O/smo.out")"

# 1. replay through the current walker, compared with the capture
run r1 "$T" replay "$CAP" "$W/r1";              [[ "$(rcof r1)" == 0 ]] || fail "replay: $(tail -3 "$O/r1.out") $(tail -3 "$O/r1.err")"
run c1 "$T" cmp "$CAP" "$W/r1/fifo-capture.kz3fc"; [[ "$(rcof c1)" == 0 ]] || fail "capture vs replay: $(cat "$O/c1.out")"
grep -q "mismatches 0" "$O/c1.out" || fail "cmp line: $(cat "$O/c1.out")"
ev="$(sed -n 's/.*events \([0-9]*\) vs.*/\1/p' "$O/c1.out")"; [[ "${ev:-0}" -gt 200 ]] || fail "too few events compared: ${ev:-0}"

# 2. determinism: the same capture replayed again gives an identical event stream
run r2 "$T" replay "$CAP" "$W/r2";              [[ "$(rcof r2)" == 0 ]] || fail "second replay"
run c2 "$T" cmp "$W/r1/fifo-capture.kz3fc" "$W/r2/fifo-capture.kz3fc"; [[ "$(rcof c2)" == 0 ]] || fail "determinism: $(cat "$O/c2.out")"

# 3. adversarial mode: the planted deferred read (made after label/get publications) is the only hazard
run a1 "$T" replay "$CAP" "$W/a1" adv;          [[ "$(rcof a1)" == 4 ]] || fail "adversarial replay exit $(rcof a1) (want 4 = hazards): $(tail -4 "$O/a1.out")"
grep -q "hazards 1 " "$O/a1.out" || fail "adversarial: want exactly 1 hazard: $(grep hazards "$O/a1.out")"
grep -q "read after label write" "$O/a1.out" || fail "adversarial: the hazard should follow a label write: $(grep 'read after' "$O/a1.out")"
# pass 157: LD_FIFO_REPLAY_HAZ=1 writes one TSV row per hazard (record, tag, space, addr, len, draw, draw-end record, first cause, its record, last cause, its record)
mkdir -p "$W/a1h"; LD_FIFO_REPLAY_HAZ=1 "$T" replay "$CAP" "$W/a1h" adv >"$O/a1h.out" 2>&1
[[ "$(wc -l < "$W/a1h/hazards.tsv" 2>/dev/null)" == 1 ]] || fail "hazards.tsv: want exactly 1 row: $(head -3 "$W/a1h/hazards.tsv" 2>&1)"
[[ "$(cut -f8 "$W/a1h/hazards.tsv" 2>/dev/null)" == *publication* || "$(cut -f8 "$W/a1h/hazards.tsv" 2>/dev/null)" == "label write" ]] || fail "hazards.tsv first cause column: $(cat "$W/a1h/hazards.tsv" 2>&1)"
run ca "$T" cmp "$CAP" "$W/a1/fifo-capture.kz3fc"; [[ "$(rcof ca)" == 1 ]] || fail "adversarial stream must differ from the capture (the pattern was read): $(cat "$O/ca.out")"
grep -q "mismatches 1" "$O/ca.out" || fail "adversarial mismatches: $(cat "$O/ca.out")"
run a2 "$T" replay "$CAP" "$W/a2" adv;          run ca2 "$T" cmp "$W/a1/fifo-capture.kz3fc" "$W/a2/fifo-capture.kz3fc"; [[ "$(rcof ca2)" == 0 ]] || fail "adversarial determinism"

# 4. negative controls (each rewrites the capture with valid checksums; replay + compare must flag it)
for kind in drop-word flip-range swap-words swap-cmds; do
  run m_$kind "$T" mutate "$CAP" "$W/m_$kind.kz3fc" $kind; [[ "$(rcof m_$kind)" == 0 ]] || { fail "mutate $kind: $(cat "$O/m_$kind.out")"; continue; }
  mkdir -p "$W/m_$kind"; cp "$W/m_$kind.kz3fc" "$W/m_$kind/fifo-capture.kz3fc"
  run rm_$kind "$T" replay "$W/m_$kind/fifo-capture.kz3fc" "$W/rm_$kind"
  run cm_$kind "$T" cmp "$W/m_$kind/fifo-capture.kz3fc" "$W/rm_$kind/fifo-capture.kz3fc"
  if [[ "$(rcof rm_$kind)" == 0 && "$(rcof cm_$kind)" == 0 ]]; then fail "negative control $kind was NOT flagged (replay and compare both clean)"; else echo "[test_log_replay] negative control $kind flagged (replay rc $(rcof rm_$kind), cmp rc $(rcof cm_$kind))"; fi
done
run ck "$T" check "$W/m_flip-range.kz3fc";      [[ "$(rcof ck)" == 1 ]] || fail "gap 6: the reader-side check missed the flipped RANGE byte: $(cat "$O/ck.out")"
run ck2 "$T" check "$W/m_drop-word.kz3fc";      [[ "$(rcof ck2)" == 0 ]] || fail "gap 6: check flagged a capture with no bad RANGE"

# 4b. pass 156: the real-capture failures, each reproduced by putting the pass 155 harness behaviour back (LD_REPLAY_OLD_*)
#   VM:     the capture's labels sit at 0x7F000000 (gen uses the game's value, not the toolkit default), the old harness sized
#           its memory from the default and the first label write faulted (rc 139, the real capture's crash).
#   PREFIX: a window that opens mid-pass starts with the tail of that pass (get word, PASS_END): the walker cannot re-emit it.
#   REF:    a ref publication by a guest thread mid-pass (the walker does not publish it itself).
#   REPORT: the ZPASS value of a GET_REPORT is a renderer answer (the stub renderer answers 0xA5A5xxxx while generating).
run vm_ok "$T" replay "$CAP" "$W/vm_ok";         [[ "$(rcof vm_ok)" == 0 ]] || fail "replay with the capture's own address layout: $(tail -3 "$O/vm_ok.err")"
env LD_REPLAY_OLD_VM=1 "$T" replay "$CAP" "$W/vm_old" >"$O/vm_old.out" 2>"$O/vm_old.err"; vmrc=$?
[[ $vmrc -ne 0 ]] || fail "negative control OLD_VM: the pass 155 memory layout replayed clean"
echo "[test_log_replay] negative control OLD_VM flagged (rc $vmrc)"
for ctl in OLD_REF OLD_REPORT OLD_WB; do
  env "LD_REPLAY_$ctl=1" "$T" replay "$CAP" "$W/c_$ctl" >"$O/c_$ctl.out" 2>"$O/c_$ctl.err"; crc=$?
  [[ $crc -ne 0 ]] || fail "negative control $ctl was NOT flagged"
  grep -q "first divergence" "$O/c_$ctl.err" && echo "[test_log_replay] negative control $ctl flagged (rc $crc)" || fail "negative control $ctl: no divergence reported: $(tail -2 "$O/c_$ctl.err")"
done
run mp "$T" mutate "$CAP" "$W/m_prefix.kz3fc" prefix; [[ "$(rcof mp)" == 0 ]] || fail "mutate prefix: $(cat "$O/mp.out")"
mkdir -p "$W/m_prefix"; cp "$W/m_prefix.kz3fc" "$W/m_prefix/fifo-capture.kz3fc"
run mpr "$T" replay "$W/m_prefix/fifo-capture.kz3fc" "$W/rm_prefix"; [[ "$(rcof mpr)" == 0 ]] || fail "a capture that opens mid-pass must replay clean: $(tail -3 "$O/mpr.out") $(grep -m1 -A2 'first div' "$O/mpr.err")"
grep -q "prefix 1" "$O/mpr.out" || fail "prefix not reported: $(grep prefix "$O/mpr.out")"
env LD_REPLAY_OLD_PREFIX=1 "$T" replay "$W/m_prefix/fifo-capture.kz3fc" "$W/rm_prefix_old" >"$O/mpo.out" 2>"$O/mpo.err"; mporc=$?
[[ $mporc -ne 0 ]] && echo "[test_log_replay] negative control OLD_PREFIX flagged (rc $mporc)" || fail "negative control OLD_PREFIX was NOT flagged"

# 5. the new capture records (gaps 1-3): notes of all three kinds, report/ring words, a renderer write-back
python3 - "$CAP" <<'PY' || rc=1
import struct,sys
d=open(sys.argv[1],'rb').read(); p=32; tags={}; notes={}; wr=0; rd_in=0; rd_out=0
while p+24<=len(d):
    t,l,s,c=struct.unpack_from('<IIQQ',d,p); b=d[p+24:p+24+l]; p+=24+l
    if t==7: tg=struct.unpack_from('<I',b,4)[0]; tags[tg]=tags.get(tg,0)+1
    if t==10: a=struct.unpack_from('<I',b,12)[0]; notes[a]=notes.get(a,0)+1
    if t==6:
        k,_,_,_,_,tg=struct.unpack_from('<6I',b,0)
        if k==3: wr+=1
        if k==0: rd_in+=tg==1; rd_out+=tg==0
ok = notes.get(0,0)>=1 and notes.get(1,0)>=1 and notes.get(2,0)>=1 and all(tags.get(x,0)>=1 for x in (5,6,9,11,12,13,14)) and wr>=1 and rd_in>=1 and rd_out>=1
print("[test_log_replay] capture content: word tags %s note kinds %s WR_RENDERER %d RD inside draw %d outside %d" % (sorted(tags.items()),sorted(notes.items()),wr,rd_in,rd_out))
sys.exit(0 if ok else 1)
PY
[[ $? -eq 0 ]] || fail "the capture lacks a record kind the 2171 hooks should write"
[[ $rc -eq 0 ]] && echo "[test_log_replay] PASS ($(grep -h '^\[replay\] records' "$O/r1.out" | head -1))" || { echo "[test_log_replay] FAIL"; exit 1; }
