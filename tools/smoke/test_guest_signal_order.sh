#!/usr/bin/env bash
# Host test for KZ3 patch 2190: the guest-signal funnel. No game data, no GPU.
#  1. static lint (guest_signal_lint.py): every kind has a call site through its class entry point, non-exempt kinds only on the
#     walker path, no raw guest store in a walker-path function, every other raw-store function classified
#  2. header table: kind numbers = the fcap WORD tags of patch 2164/2171 for 1..6 and 9..14, the exempt set is exactly {6,12,13,14}
#  3. negative controls on a scratch copy of the sources: a raw vm_write32 and an unrouted memmove in walker functions, a wrong entry
#     point, a deleted call site, a non-exempt kind moved into an API function: each must make the lint fail
#  4. replay of the synthetic capture through the real walker (test_log_replay.c): the re-capture holds annotation words, every
#     span of kind 16 / 17 is followed by a WR_WALKER range that lies inside it, only known kinds appear, counters on the walker
#     thread; real capture work/captures too when present; the two-thread test (off-walker = offr, writes land directly)
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_guest_signal_order.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
source "$ROOT/tools/smoke/pub_srcs.inc"; pub_src_args "$V" "$PT/libs/kzr"   # patch 2197: the harness links the publisher
if [[ ! -f "$V/rsx_guest_signal.h" ]]; then echo "[test_guest_signal_order] SKIP: no patched toolkit with patch 2190 ($V)"; exit 0; fi
O="$ROOT/build/smoke-host/gs-order"; rm -rf "$O"; mkdir -p "$O/work"
rc=0; fail() { echo "[test_guest_signal_order] FAIL: $*"; rc=1; }
L="$ROOT/tools/smoke/guest_signal_lint.py"
# 1
out="$(python3 -I "$L" "$V" 2>&1)" || fail "lint on the tree: $out"; echo "[test_guest_signal_order] $(tail -1 <<<"$out")"
# 2
python3 -I - "$V/rsx_guest_signal.h" <<'PY' || rc=1
import re, sys
h = open(sys.argv[1]).read()
k = {n: int(v) for n, v in re.findall(r'\b(RSX_GS_[A-Z0-9_]+)\s*=\s*(\d+)', h)}
tags = {'GET_JUMP': 1, 'GET_REL': 2, 'LABEL': 3, 'GET_END': 4, 'REPORT': 5, 'REF': 6, 'SEM_BE': 9, 'NOTIFY': 10, 'REPORT_RB': 11, 'RING_JUMP': 12, 'CTX_WORD': 13, 'PUT_RECYCLE': 14}
bad = [n for n, t in tags.items() if k.get('RSX_GS_' + n) != t]
ex = sorted(k[n] for n, c, e in re.findall(r'\[(RSX_GS_[A-Z0-9_]+)\]\s*=\s*\{\s*"[a-z0-9_]+",\s*\'(\w)\',\s*(\d)', h) if e == '1')
if bad or ex != [6, 12, 13, 14]: print('[test_guest_signal_order] FAIL: tag numbers %s exempt %s (want [6, 12, 13, 14])' % (bad, ex)); sys.exit(1)
print('[test_guest_signal_order] header: 12 capture tags match, exempt kinds', ex)
PY
# 3
ctl() { # name sed-script-on-file file
  local d="$O/neg_$1"; mkdir -p "$d"; cp "$V/cellGcmSys.c" "$V/rsx_commands.c" "$V/rsx_live_draw.c" "$V/rsx_guest_signal.h" "$d/"
  python3 -I - "$d/$3" "$2" <<'PY'
import sys
p, spec = sys.argv[1], sys.argv[2]
a, b = spec.split('=>', 1); s = open(p).read()
if s.count(a) < 1: print('control anchor missing: ' + a); sys.exit(3)
open(p, 'w').write(s.replace(a, b, 1))
PY
  [[ $? == 0 ]] || { fail "control $1: anchor not found"; return; }
  if python3 -I "$L" "$d" >"$d/out" 2>&1; then fail "negative control $1 was NOT flagged"; else echo "[test_guest_signal_order] control $1 flagged: $(grep -m1 LINT "$d/out" | cut -c1-110)"; fi
}
ctl rawwrite   'static void gcm_rsx_process_fifo_body(void)
{=>static void gcm_rsx_process_fifo_body(void)
{ vm_write32(0x1000, 1);' cellGcmSys.c
ctl memmove    'static void gcm_rsx_process_fifo_body(void)
{=>static void gcm_rsx_process_fifo_body(void)
{ memmove(vm_base + 0x2000, vm_base + 0x3000, 16);' cellGcmSys.c
ctl entry      'gcm_guest_signal_note(RSX_GS_REF_PUSH,=>gcm_guest_signal(RSX_GS_REF_PUSH,' cellGcmSys.c
ctl nosite     'gcm_guest_signal_note(RSX_GS_PRESENT, buffer_id, 0u);=>' rsx_live_draw.c
ctl droplabel  'gcm_guest_signal(RSX_GS_LABEL, la, v);=>(void)0;' cellGcmSys.c
python3 -I - "$O" "$V" <<'PY' || rc=1
import sys, os, re   # control: a non-exempt kind (REPORT) called from an API function must be flagged
o, v = sys.argv[1], sys.argv[2]; d = o + '/neg_apicall'; os.makedirs(d, exist_ok=True)
for f in ('cellGcmSys.c', 'rsx_commands.c', 'rsx_live_draw.c', 'rsx_guest_signal.h'): open(d + '/' + f, 'w').write(open(v + '/' + f).read())
s = open(d + '/cellGcmSys.c').read(); a = 's32 cellGcmSetFlipCommandWithWaitLabel('
i = s.index(a); j = s.index('{', i); s = s[:j + 1] + ' gcm_guest_signal(RSX_GS_REPORT, 0, 0);' + s[j + 1:]; open(d + '/cellGcmSys.c', 'w').write(s)
PY
d="$O/neg_apicall"; if python3 -I "$L" "$d" >"$d/out" 2>&1; then fail "negative control apicall was NOT flagged"; else echo "[test_guest_signal_order] control apicall flagged: $(grep -m1 LINT "$d/out" | cut -c1-110)"; fi
# 4
CF=(-std=gnu17 -O1 -g -w -I "$PT/include" -I "$V" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread -lm)
EXTRA=(); [[ -f "$V/rsx_tiling.c" ]] && EXTRA=("$V/rsx_tiling.c")
gcc "${CF[@]}" "$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" "$V/rsx_commands.c" "${PUB_SRC[@]}" "${EXTRA[@]}" -o "$O/t" || { echo "[test_guest_signal_order] FAIL: build harness"; exit 1; }
T="$O/t"; W="$O/work"
"$T" gsthread >"$O/gst.out" 2>&1 || fail "two-thread funnel test: $(cat "$O/gst.out")"; tail -1 "$O/gst.out" | sed 's/^/[test_guest_signal_order] /'
"$T" gen "$W/cap" >"$O/gen.out" 2>&1 || { fail "gen: $(tail -2 "$O/gen.out")"; exit 1; }
chk() { # capture-file label
  "$T" dump "$1" 0 100000000 | python3 -I -c '
import sys, re
KN = set(list(range(1, 7)) + list(range(8, 26)))
rec = []
for l in sys.stdin:
    m = re.match(r"(\d+) t(\d+) len(\d+):(.*)", l)
    if m: rec.append((int(m.group(2)), [int(x, 16) for x in m.group(4).split()]))
tags = {}; spans = 0; bad = []
for i, (t, w) in enumerate(rec):
    if t == 7 and w and w[0] == 1:
        tags[w[1]] = tags.get(w[1], 0) + 1
        if w[1] not in KN: bad.append("unknown tag %d" % w[1])
        if w[1] in (16, 17):
            spans += 1; ea, n = w[2], w[3]
            nxt = next(((x, ww) for x, ww in rec[i + 1:i + 40] if x == 6 and ww[0] == 2), None)   # RANGE kind WR_WALKER
            if nxt is None: bad.append("span %d at %d has no WR_WALKER range after it" % (w[1], i))
            elif not (nxt[1][2] >= ea and nxt[1][2] + nxt[1][3] <= ea + n): bad.append("span %d [%x+%x) does not hold its WR_WALKER range [%x+%x)" % (w[1], ea, n, nxt[1][2], nxt[1][3]))
if bad: print("FAIL: " + "; ".join(bad[:4])); sys.exit(1)
print("kinds %s, %d spans each followed by a contained WR_WALKER range" % (sorted(tags.items()), spans))
' || return 1
}
run_one() { # label cap
  local r="$W/r_$1"; mkdir -p "$r"
  "$T" replay "$2" "$r" >"$O/$1.out" 2>"$O/$1.err" || { fail "$1 replay: $(tail -2 "$O/$1.out") $(head -2 "$O/$1.err")"; return; }
  grep -q "diverged 0 extra 0 left 0" "$O/$1.out" || fail "$1: replay diverged: $(grep -m1 'records' "$O/$1.out")"
  echo "[test_guest_signal_order] $1: $(grep -m1 '\[replay-gs\]' "$O/$1.out" | cut -c1-200)"
  local res; res="$(chk "$r/fifo-capture.kz3fc" "$1" 2>&1)" || { fail "$1 re-capture: $res"; return; }
  echo "[test_guest_signal_order] $1 re-capture: $res" | cut -c1-260
  grep -m1 '\[replay-gs\]' "$O/$1.out" | grep -q ' 3=[1-9][0-9]*/0' || fail "$1: no label (kind 3) signals on the walker thread / offr != 0"
}
run_one synth "$W/cap/fifo-capture.kz3fc"
RC="$ROOT/work/captures/fifo-capture.kz3fc"
if [[ -f "$RC" ]]; then run_one playthrough "$RC"; else echo "[test_guest_signal_order] SKIP playthrough: no work/captures"; fi
[[ $rc == 0 ]] && echo "[test_guest_signal_order] PASS" || { echo "[test_guest_signal_order] FAIL"; exit 1; }
