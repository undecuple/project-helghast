#!/usr/bin/env bash
# Reproducible PPU lift of the KZ3 EBOOT with ps3recomp + our patches.
#
#   nix develop .#re --command tools/sandbox.sh -- tools/lift.sh                       # update 1.14 (default)
#   KZ3_EBOOT_VER=100 nix develop .#re --command tools/sandbox.sh -- tools/lift.sh     # disc v1.00 (A/B reference)
#
# KZ3_EBOOT_VER (default tools/spu/kz3ver.py DEFAULT, 114 since series step 6 of the RE notes s.36.4) picks the
# ELF, code end, TOC, analysis set, override table, lift fixups and output directory (kz3ver.py LIFT_DIRS):
#   114: work/bin/EBOOT-114.elf  code end 0xe753f4 TOC 0x139fe40 work/analysis-114 ppu_overrides-114.txt       -> work/recomp
#   100: work/bin/EBOOT.elf      code end 0xdfbbd4 TOC 0x132c960 work/analysis     port/spu/ppu_overrides.txt   -> work/recomp-100
# The two versions lift into different directories so they do not overwrite each other's lift (s.36.5 risk 7); the
# .lift-stamp's elf= names the version (tools/assets/lift_table.sh, build-kz3.sh, CMake and the remote guard read it).
# KZ3_RECOMP_DIR overrides the output directory (a parallel worktree's work/recomp is the main checkout's, read-only).
#
# 1. work/ps3recomp-patched = a copy of third_party/ps3recomp at its pinned commit, with every
#    port/patches/*.patch applied (fresh each run, tools/ps3recomp-patched.sh).
# 2. ppu_loader + find_functions (if $AN/functions.json is missing).
# 3. Lift fixups (ppu_lift_fixups-<ver>.txt, only 1.14 has one): tools/release/lift_fixups.py writes
#    $OUT/functions.fixed.json (function ends find_functions got wrong) and $OUT/jt_hints.txt (jump tables the lifter's
#    scan cannot resolve, read through PPU_JT_HINTS, patch 3004).
# 4. ppu_lifter with --code-end (the last executable section ends there and .rodata starts
#    2026-10-08: 0xdf5710 cut off five executable sections after the import stubs, 11 calls dropped) and --overrides
#    (functions the host replaces, patch 1018) and --chunks 64 (patch 3005: address-keyed
#    chunks, only changed files rewritten) into $OUT: ppu_recomp.h, ppu_overrides.h, ppu_recomp_000..063.cpp,
#    ppu_recomp_table.cpp, .lift-stamp.
# 5. lift_audit into $AN/lift_audit.log ($OUT/lift_audit.log when KZ3_RECOMP_DIR is set).
# Needs the decrypted EBOOT (EBOOT.elf) the player made from their own copy.
set -euo pipefail
echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true   # first OOM victim, not the editor
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# Build lock: a build globs work/recomp, which this script replaces (see tools/build-kz3.sh).
LOCKS="${KZ3_MAIN:-$ROOT}/work/locks"; mkdir -p "$LOCKS"
# Every build slot (2026-10-11: KZ3_BUILD_SLOTS builds may run at once; a lift must wait for all of them): tools/agents/slotlock.sh.
source "$ROOT/tools/agents/slotlock.sh"; slot_take_all "$LOCKS" build "${KZ3_BUILD_SLOTS:-1}"
PT=work/ps3recomp-patched
# Version and output directory from the one place that decides them (tools/spu/kz3ver.py; exit 2 on a bad version).
KV="$(python3 tools/spu/kz3ver.py ver recomp)" || exit 2
read -r VER OUT <<<"$KV"
case "$VER" in
  100) ELF=work/bin/EBOOT.elf;     CODE_END=0xdfbbd4; TOC=0x132c960; AN=work/analysis
       OVR=port/spu/ppu_overrides.txt ;;
  114) ELF=work/bin/EBOOT-114.elf; CODE_END=0xe753f4; TOC=0x139fe40; AN=work/analysis-114
       # The 1.14 table lives in re/data/overrides until it moves next to the 1.00 one (s.36.6).
       OVR=port/spu/ppu_overrides-114.txt; [[ -f "$OVR" ]] || OVR=re/data/overrides/ppu_overrides-114.txt ;;
  *) echo "KZ3_EBOOT_VER must be 100 or 114" >&2; exit 2 ;;
esac
AUDIT="$AN/lift_audit.log"
if [[ -n "${KZ3_RECOMP_DIR:-}" ]]; then OUT="$KZ3_RECOMP_DIR"; AUDIT="$OUT/lift_audit.log"; fi
FIX=port/spu/ppu_lift_fixups-$VER.txt; [[ -f "$FIX" ]] || FIX=re/data/overrides/ppu_lift_fixups-$VER.txt
[[ -f "$ELF" ]] || { echo "missing $ELF (decrypt EBOOT.BIN with RPCS3 first)" >&2; exit 1; }
[[ -f "$OVR" ]] || { echo "missing $OVR" >&2; exit 1; }

"$ROOT/tools/ps3recomp-patched.sh"

mkdir -p "$AN" "$OUT"
if [[ ! -f "$AN/functions.json" ]]; then
  python3 "$PT/tools/ppu_loader.py" "$ELF" -o "$AN/"
  python3 "$PT/tools/find_functions.py" "$ELF" --seed-json "$AN/EBOOT.functions.json" \
    --output "$AN/functions.json"
fi

# Lift fixups (same helper the release first-run uses; stops the lift when a fixup does not match the function list).
FUNCS="$AN/functions.json"
unset PPU_JT_HINTS
if [[ -f "$FIX" ]]; then
  FUNCS="$OUT/functions.fixed.json"
  python3 tools/release/lift_fixups.py --fixups "$FIX" --functions "$AN/functions.json" --out "$FUNCS" \
    --jt-hints "$OUT/jt_hints.txt"
  export PPU_JT_HINTS="$ROOT/$OUT/jt_hints.txt"; [[ "$OUT" == /* ]] && PPU_JT_HINTS="$OUT/jt_hints.txt"
fi

# --chunks 64 (patch 3005, 2026-10-11): 64 source files cut by guest code bytes plus ppu_recomp_table.cpp, stable across
# relifts, each written only when its content changed, so a relift that changes one function (an override row, a lifter
# fix for one opcode) recompiles one ~70k-line file instead of all eight 600k-line ones (391-476 s and up to 4.4 GB each).
# The override announcements go to work/recomp/ppu_overrides.h (only port/spu/kz3_ppu_overrides.cpp includes it).
# The lifter removes stale chunk files itself, after writing (no `find -delete` here: that would reset every mtime).
KZ3_LIFT_CHUNKS="${KZ3_LIFT_CHUNKS:-64}"
python3 "$PT/tools/ppu_lifter.py" "$ELF" --functions "$FUNCS" \
  --hle-stubs "$AN/EBOOT.imports.json" --toc "$TOC" --code-end "$CODE_END" \
  --overrides "$OVR" -j 4 --chunks "$KZ3_LIFT_CHUNKS" --output "$OUT" > "$OUT/lift.log" 2>&1
grep -m5 'jump-table hint' "$OUT/lift.log" || true
tail -5 "$OUT/lift.log"
# Lift stamp: what this lift was made from (tools/build-kz3.sh compares ppu_overrides.txt's mtime against it, since an
# unchanged ppu_recomp.h now keeps its old mtime).
printf 'elf=%s overrides=%s patchset=%s chunks=%s date=%s\n' "$ELF" "$(sha256sum "$OVR" | cut -c1-16)" \
  "$(cat "$PT/.kz3-patchset" 2>/dev/null)" "$KZ3_LIFT_CHUNKS" "$(date -Is)" > "$OUT/.lift-stamp"

python3 "$PT/tools/lift_audit.py" --elf "$ELF" --functions "$FUNCS" \
  --recomp-dir "$OUT" --baseline "$AN/none.json" > "$AUDIT" 2>&1 || true
sed -n '/=== Pass 1/,/TOTAL \/\* unresolved/p' "$AUDIT"
