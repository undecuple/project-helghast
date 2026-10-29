#!/usr/bin/env bash
# Pass 120: SPU job capture (port/spu/kz3_jobhook.c, runtime patch 1141) and replay (tools/spu/job_replay.c) on a
# synthetic job (r120_synth_job.c) run by a stand-in runtime (r120_cap_sim.c). Self-contained: needs only the patched
# toolkit (work/ps3recomp-patched, KZ3_PATCHED_DIR = another tree), no game data. Checks:
#   1 capture: KZ3_JOB_CAPTURE=7,n=3 writes 3 captures (cap.bin + index.json) with job and post-phase records;
#   2 native hook: KZ3_NATIVE_JOB=7:identity dispatches through the hook with the same outputs; an image with no
#     registered function is reported and runs lifted;
#   3 replay of the lifted job: strict and output EXACT on every capture;
#   4 negative controls: a corrupted GET byte in a capture makes strict replay fail; a wrong native candidate
#     fails the output check; a correct native candidate with other DMA chunking and the identity wrapper pass;
#   5 (pass 121) capture version 2: a selectWorkload answer (KERN) and an interrupt handler's LS change (IRQ_LS)
#     are recorded and served; altering either makes strict replay fail.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_drain.c ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q g_spu_jhook_on "$T/runtime/spu/spu_drain.c" || { echo "FAIL: $T has no patch 1141"; exit 1; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
fail=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }
CC="${CC:-cc}"
INC=(-I "$T/runtime/spu" -I "$T/include" -I "$T/runtime" -I "$ROOT/port/spu" -I "$ROOT/tools/spu")
FL=(-std=gnu17 -O1 -w -fno-strict-aliasing -ffp-contract=off -msse4.1)
S="$ROOT/tools/spu"
$CC "${FL[@]}" "${INC[@]}" -o "$OUT/sim" "$S/r120_cap_sim.c" "$S/r120_synth_job.c" "$ROOT/port/spu/kz3_jobhook.c" \
    "$ROOT/port/spu/kz3_native_jobs.c" "$ROOT/port/spu/kz3_native_post.c" "$ROOT/port/spu/kz3_native_dof.c" -lm >"$OUT/b1.log" 2>&1 || { echo "FAIL build sim: $(grep -m2 -E 'error|undefined' "$OUT/b1.log" | cut -c1-200)"; exit 1; }
build_replay() {  # out [defines...]
  local o=$1; shift
  local extra=()
  [[ $# -gt 0 ]] && extra=(-DREPLAY_NATIVE "$@")
  $CC "${FL[@]}" "${INC[@]}" -DHARNESS_REGISTER=r120_synth_register "${extra[@]}" -o "$o" "$S/job_replay.c" \
      "$S/r120_synth_job.c" "$T/runtime/spu/spu_drain.c" "$T/runtime/spu/spu_interp.c" -lm >"$o.log" 2>&1 ||
    { echo "FAIL build $(basename "$o"): $(grep -m2 -E 'error|undefined' "$o.log" | cut -c1-200)"; exit 1; }
}
build_replay "$OUT/rp"
build_replay "$OUT/rp_ok" -DNATIVE_OK
build_replay "$OUT/rp_bad" -DNATIVE_BAD
build_replay "$OUT/rp_id" -DNATIVE_IDENTITY

# 1 capture
a="$(KZ3_JOB_CAPTURE=7,n=3 KZ3_JOB_CAPTURE_OUT="$OUT/cap" "$OUT/sim" 3 2>"$OUT/sim.err")"
n=$(ls "$OUT"/cap/ovl7/*/cap.bin 2>/dev/null | wc -l)
if [[ $n -eq 3 ]] && grep -q '"post_dma_out": \[\["0x20","0x00030100"' "$OUT/cap/ovl7/0/index.json" &&
   grep -q '"job_atomic": \[\["0xD0"' "$OUT/cap/ovl7/0/index.json"; then ok "capture: 3 captures with job + post records"
else bad "capture: $n captures; $(head -c 300 "$OUT/cap/ovl7/0/index.json" 2>/dev/null) $(head -3 "$OUT/sim.err")"; fi
# 2 native hook
b="$(KZ3_NATIVE_JOB=7:identity "$OUT/sim" 3 2>/dev/null)"
if grep -q 'fn=hooked' <<<"$b" && [[ "$(sed 's/fn=[a-z]*//' <<<"$a")" == "$(sed 's/fn=[a-z]*//' <<<"$b")" ]]; then
  ok "native hook: identity wrapper dispatched, outputs equal"; else bad "native hook: [$b] vs [$a]"; fi
c="$(KZ3_NATIVE_JOB=7 "$OUT/sim" 1 2>&1)"
if grep -q 'has no registered host function' <<<"$c" && grep -q 'fn=lifted' <<<"$c"; then ok "native hook: unregistered image ignored"
else bad "native hook unregistered: $c"; fi
# 3 lifted replay
r="$("$OUT/rp" "$OUT"/cap/ovl7/*/cap.bin)"
if [[ $(grep -c ' EXACT ' <<<"$r") -eq 6 ]]; then ok "replay lifted: 3 x strict + output EXACT"; else bad "replay lifted: $r"; fi
# 4 negative controls and native candidates
cp "$OUT/cap/ovl7/1/cap.bin" "$OUT/corrupt.bin"
python3 - "$OUT/corrupt.bin" <<'PY'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, "rb").read())
off = 80 + 2 * (0x40000 + 2048)   # sizeof(struct kz3_jcap_hdr) = 80
nev = struct.unpack_from("<I", b, 36)[0]
for i in range(nev):   # first GET (XFER, cmd 0x40..0x4F): flip its first data byte
    kind, ph, ch, cmd, lsa, size = struct.unpack_from("<BBHIII", b, off)
    if kind == 4 and 0x40 <= (cmd & 0xFF) <= 0x4F:
        b[off + 40] ^= 0x5A; break
    off += 40 + ((size + 7) & ~7)
open(p, "wb").write(b)
PY
r="$(REPLAY_MODE=strict "$OUT/rp" "$OUT/corrupt.bin")"
if grep -qE ' (DIFF|DIVERGE) ' <<<"$r"; then ok "negative: corrupted GET fails strict replay"; else bad "corrupted capture: $r"; fi
# 5 (pass 121) the kernel answer and the interrupt handler's LS change come from the capture: altering either
#   record makes strict replay fail (so a passing replay really used them)
for what in kern irq; do
  cp "$OUT/cap/ovl7/1/cap.bin" "$OUT/$what.bin"
  python3 - "$OUT/$what.bin" "$what" <<'PY'
import struct, sys
p, what = sys.argv[1:]; b = bytearray(open(p, "rb").read())
off = 80 + 2 * (0x40000 + 2048)
nev = struct.unpack_from("<I", b, 36)[0]
for i in range(nev):
    kind, ph, ch, cmd, lsa, size = struct.unpack_from("<BBHIII", b, off)
    if what == "kern" and kind == 6:
        b[off + 24] ^= 0x01; break          # v[0]: the answer in r3
    if what == "irq" and kind == 8:
        b[off + 40 + 8] ^= 0x01; break      # first byte of the first changed-LS run
    off += 40 + ((size + 7) & ~7)
else:
    sys.exit("record not found")
open(p, "wb").write(b)
PY
  r="$(REPLAY_MODE=strict "$OUT/rp" "$OUT/$what.bin")"
  if grep -qE ' (DIFF|DIVERGE) ' <<<"$r"; then ok "negative: altered $what record fails strict replay"; else bad "altered $what: $r"; fi
done
r="$("$OUT/rp_ok" "$OUT"/cap/ovl7/*/cap.bin)"
if [[ $(grep -c ' EXACT ' <<<"$r") -eq 3 ]]; then ok "native candidate (other chunking): output EXACT"; else bad "native ok: $r"; fi
r="$("$OUT/rp_bad" "$OUT"/cap/ovl7/*/cap.bin)"
if [[ $(grep -c ' DIFF ' <<<"$r") -eq 3 ]]; then ok "negative: wrong native candidate fails"; else bad "native bad: $r"; fi
r="$(REPLAY_MODE=both "$OUT/rp_id" "$OUT"/cap/ovl7/*/cap.bin)"
if [[ $(grep -c ' EXACT ' <<<"$r") -eq 6 ]]; then ok "identity candidate: strict + output EXACT"; else bad "identity: $r"; fi
[[ $fail -eq 0 ]] && echo "== test_r120_jobreplay: PASS" || echo "== test_r120_jobreplay: FAIL"
exit $fail
