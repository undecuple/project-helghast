#!/usr/bin/env bash
# Host test for KZ3 patch 2190: guest memory is identical before and after the funnel. No game data, no GPU.
# The BASELINE is the patched toolkit with patch 2190 reverse-applied (the walker as it was before the funnel). The same replay harness
# (tools/smoke/test_log_replay.c, LD_REPLAY_MEMDIGEST) replays one capture through both trees and writes a digest of every non-zero 4 KB page of
# the guest memory (the synthetic capture is made by the BASELINE walker, a version-1 file both readers take; report/notify timestamp words zeroed). Captures: the synthetic one built here, and work/captures when present.
# Checks: baseline = funnel digest, byte for byte (both captures); each tree is deterministic (run twice); negative control: the funnel tree with an
# UNROUTED memmove into a guest page (a walker function, not through the funnel) must give a different digest, and so must a funnel that writes
# a wrong value (swapped label word). SKIP when patch 2190 cannot be reversed (a later patch touches its sites: re-baseline then).
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_guest_mem_diff.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
source "$ROOT/tools/smoke/pub_srcs.inc"   # patch 2197: the harness links the publisher (pub_src_args per tree, in build)
P="$(ls "$ROOT"/port/patches/2190-*.patch 2>/dev/null | head -1)"
if [[ ! -f "$V/rsx_guest_signal.h" || -z "$P" ]]; then echo "[test_guest_mem_diff] SKIP: no patched toolkit with patch 2190"; exit 0; fi
REL="build/smoke-host/gs-memdiff"; O="$ROOT/$REL"; rm -rf "$O"; mkdir -p "$O/work" "$O/base/libs"
cp -a "$V" "$O/base/libs/video"; ln -s "$PT/runtime" "$O/base/runtime"; ln -s "$PT/include" "$O/base/include"
# patch 2197 sits on top of the funnel (it routes the funnel through the publisher): reverse it first (its video files only), then patch 2190
P97="$(ls "$ROOT"/port/patches/2197-*.patch 2>/dev/null | head -1)"
if [[ -n "$P97" ]]; then
  (cd "$ROOT" && git apply -R --include="$REL/base/libs/video/*" --whitespace=nowarn --directory="$REL/base" "$P97" >"$O/revert97.log" 2>&1) || { echo "[test_guest_mem_diff] SKIP: patch 2197 does not reverse on this tree ($(head -1 "$O/revert97.log" | cut -c1-120)); re-baseline the test"; exit 0; }
fi
(cd "$ROOT" && git apply -R --whitespace=nowarn --directory="$REL/base" "$P" >"$O/revert.log" 2>&1) || { echo "[test_guest_mem_diff] SKIP: patch 2190 does not reverse on this tree ($(head -1 "$O/revert.log" | cut -c1-120)); re-baseline the test"; exit 0; }
[[ ! -f "$O/base/libs/video/rsx_guest_signal.h" ]] || { echo "[test_guest_mem_diff] FAIL: the baseline still has the funnel header"; exit 1; }
rc=0; fail() { echo "[test_guest_mem_diff] FAIL: $*"; rc=1; }
build() { # tree-root out [extra gcc args]
  local t="$1" out="$2"; shift 2
  local vd="$t/libs/video" ex=(); [[ -f "$vd/rsx_tiling.c" ]] && ex=("$vd/rsx_tiling.c")
  pub_src_args "$vd" "$PT/libs/kzr"
  gcc -std=gnu17 -O1 -g -w -I "$t/include" -I "$vd" -ffunction-sections -fdata-sections "-Wl,--gc-sections" -pthread "$@" \
      "$ROOT/tools/smoke/test_log_replay.c" "$ROOT/tools/smoke/test_sync_hist_stubs.c" "$ROOT/tools/smoke/test_log_replay_stubs.c" "$vd/rsx_commands.c" "${PUB_SRC[@]}" "${ex[@]}" -lm -o "$out"
}
build "$O/base" "$O/tb" || { echo "[test_guest_mem_diff] FAIL: build baseline"; exit 1; }
build "$PT" "$O/tf" || { echo "[test_guest_mem_diff] FAIL: build funnel"; exit 1; }
# negative controls: scratch copies of the funnel tree
mkdir -p "$O/n1/libs" "$O/n2/libs"
for n in n1 n2; do cp -a "$V" "$O/$n/libs/video"; ln -s "$PT/runtime" "$O/$n/runtime"; ln -s "$PT/include" "$O/$n/include"; ln -s "$PT/libs/kzr" "$O/$n/libs/kzr"; done
python3 -I - "$O" <<'PY' || { echo "[test_guest_mem_diff] FAIL: control anchors"; exit 1; }
import sys
o = sys.argv[1]
p = o + '/n1/libs/video/cellGcmSys.c'; s = open(p).read()   # unrouted memmove at the pass end (control page word copied into a spare page)
a = '    gcm_guest_signal(RSX_GS_GET_END, GCM_CONTROL_GUEST_ADDR + 4,'
assert s.count(a) == 1; s = s.replace(a, '    { extern u8* vm_base; memmove(vm_base + GCM_CONTROL_GUEST_ADDR + 0x100, vm_base + GCM_CONTROL_GUEST_ADDR, 16); }\n' + a); open(p, 'w').write(s)
p = o + '/n2/libs/video/rsx_commands.c'; s = open(p).read()  # a funnel that writes a wrong value (the label word's low bit flipped)
a = "if (!(rsx_pub_routes() && rsx_pub_take('W', kind, ea, val, NULL))) vm_write32(ea, val);"   # patch 2197 wrapped the store in the publisher's route
assert s.count(a) == 1; s = s.replace(a, "if (!(rsx_pub_routes() && rsx_pub_take('W', kind, ea, val, NULL))) vm_write32(ea, kind == 3u ? val ^ 1u : val);"); open(p, 'w').write(s)
PY
build "$O/n1" "$O/t1" || { echo "[test_guest_mem_diff] FAIL: build control 1"; exit 1; }
build "$O/n2" "$O/t2" || { echo "[test_guest_mem_diff] FAIL: build control 2"; exit 1; }
"$O/tb" gen "$O/work/cap" >"$O/gen.out" 2>&1 || { fail "gen: $(tail -2 "$O/gen.out")"; exit 1; }
dig() { # binary capture tag  -> digest file $O/d_tag
  local b="$1" cap="$2" tag="$3"; mkdir -p "$O/work/o_$tag"
  LD_REPLAY_MEMDIGEST="$O/d_$tag" "$b" replay "$cap" "$O/work/o_$tag" >"$O/o_$tag.out" 2>"$O/o_$tag.err" || { fail "replay $tag: $(tail -2 "$O/o_$tag.out") $(head -2 "$O/o_$tag.err")"; return 1; }
  [[ -s "$O/d_$tag" ]]
}
same() { cmp -s "$O/d_$1" "$O/d_$2"; }
one() { # label capture
  local l="$1" cap="$2"
  dig "$O/tb" "$cap" "$l.b1" && dig "$O/tb" "$cap" "$l.b2" && dig "$O/tf" "$cap" "$l.f1" && dig "$O/tf" "$cap" "$l.f2" || return
  same "$l.b1" "$l.b2" || fail "$l: the baseline digest is not deterministic"
  same "$l.f1" "$l.f2" || fail "$l: the funnel digest is not deterministic"
  same "$l.b1" "$l.f1" || fail "$l: guest memory differs between the baseline and the funnel: $(diff "$O/d_$l.b1" "$O/d_$l.f1" | head -4 | tr '\n' ' ')"
  local n; n=$(wc -l <"$O/d_$l.b1"); [[ "$n" -gt 4 ]] || fail "$l: too few non-zero pages ($n)"
  dig "$O/t1" "$cap" "$l.c1" && { same "$l.b1" "$l.c1" && fail "$l: negative control (unrouted memmove) was NOT caught" || echo "[test_guest_mem_diff] $l control unrouted memmove caught ($(diff "$O/d_$l.b1" "$O/d_$l.c1" | grep -c '^[<>]') differing digest lines)"; }
  dig "$O/t2" "$cap" "$l.c2" && { same "$l.b1" "$l.c2" && fail "$l: negative control (wrong funnel value) was NOT caught" || echo "[test_guest_mem_diff] $l control wrong funnel value caught"; }
  echo "[test_guest_mem_diff] $l: baseline = funnel over $n non-zero pages ($(grep -m1 'memory digest' "$O/o_$l.f1.out" | cut -c1-80))"
}
one synth "$O/work/cap/fifo-capture.kz3fc"
RCAP="$ROOT/work/captures/fifo-capture.kz3fc"
if [[ -f "$RCAP" ]]; then one playthrough "$RCAP"; else echo "[test_guest_mem_diff] SKIP playthrough: no work/captures"; fi
[[ $rc == 0 ]] && echo "[test_guest_mem_diff] PASS" || { echo "[test_guest_mem_diff] FAIL"; exit 1; }
