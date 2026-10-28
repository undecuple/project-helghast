#!/usr/bin/env bash
# Reproducible lift of all KZ3 SPU code (22 SPU ELFs + 75 WWS job modules + the WWS job-manager SPURS policy module) with ps3recomp + our patches.
#
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/lift_spu.sh [-j N] [--only NAME,...]
#
# 1. work/spu/ps3recomp-patched = a copy of third_party/ps3recomp at its pinned commit, with every
#    port/patches/*.patch applied (fresh each run; same method as tools/lift.sh).
# 2. tools/spu/lift_spu.py lifts every image into work/spu/lift/<name>/ and writes work/spu/spu_lift.json.
# 3. tools/spu/spu_audit.py writes the audit to work/spu/spu_audit.txt.
# 4. tools/spu/job_refs.py writes PPU references to each image to work/spu/job_refs.tsv (+ job_refs.txt).
# 5. tools/spu/gen_spu_register.py writes the registration TU work/spu/gen/ (port/spu/CMakeLists.txt builds
#    target kz3_spu from it), and tools/spu/test_spu_register.py checks every registered fingerprint against
#    the runtime's own spu_workload_fingerprint().
# Needs work/bin/EBOOT.elf, work/bin/spu/*.elf and work/analysis/jobmods.tsv (shared, read-only).
# EBOOT version, the variable tools/lift.sh uses: KZ3_EBOOT_VER=114 (default, kz3ver.DEFAULT) or 100; 114 = (work/bin/EBOOT-114.elf,
# work/analysis-114/spu_images/*.elf, work/analysis-114/jobmods.tsv, re/data/spu_seeds_114.tsv; image keys translated
# through re/data/spu_map_114.tsv by tools/spu/kz3ver.py). Output per version (kz3ver.SPU_OUT_REL): work/spu for 1.14,
# work/spu-100 for 1.00 (pass 149), so the two lifts and the builds that glob them sit side by side; the version is recorded in
# <out>/eboot_ver and the generator and tests refuse a lift of the other version.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
SUB=third_party/ps3recomp
# Output directory of this version's lift (tools/spu/kz3ver.py SPU_OUT_REL: 1.14 work/spu, v1.00 work/spu-100; KZ3_SPU_OUT).
OUT="$(python3 -c 'import sys; sys.path.insert(0, "tools/spu"); import kz3ver; print(kz3ver.SPU_OUT_REL)')"
PT="$OUT/ps3recomp-patched"
python3 - <<'PY' || exit 1
import os, sys
sys.path.insert(0, "tools/spu")
import kz3ver
miss = [p for p in (kz3ver.EBOOT, kz3ver.JOBTSV, kz3ver.SPU_DIR) if not os.path.exists(p)]
print(f"EBOOT {kz3ver.VER}: {os.path.relpath(kz3ver.EBOOT)}, {os.path.relpath(kz3ver.SPU_DIR)}")
sys.exit(f"missing {miss}" if miss else 0)
PY

# Hold the build lock (tools/build-kz3.sh): a build re-globs work/spu, so a lift replacing it mid-build breaks the
# build with missing files (RSX pass 12, 2026-10-08).
LOCKS="${KZ3_MAIN:-$ROOT}/work/locks"; mkdir -p "$LOCKS"
# Every build slot (2026-10-11: KZ3_BUILD_SLOTS builds may run at once; a lift must wait for all of them): tools/agents/slotlock.sh.
source "$ROOT/tools/agents/slotlock.sh"; slot_take_all "$LOCKS" build "${KZ3_BUILD_SLOTS:-1}"

if [[ -e "$PT" ]]; then "$ROOT/tools/safe_rm.sh" "$PT"; fi
if [[ -e "$OUT/lift" ]]; then "$ROOT/tools/safe_rm.sh" "$OUT/lift"; fi
mkdir -p "$OUT"
mkdir -p "$PT"
tar -C "$SUB" --exclude=.git -cf - . | tar -C "$PT" -xf -
# Apply from inside the copy with repo discovery stopped above it: in a lane worktree .git is a file pointing
# into the main checkout, which the sandbox does not mount, so `git apply` must run as a plain patch tool.
for p in port/patches/*.patch; do
  [[ -e "$p" ]] || continue
  out="$(cd "$PT" && GIT_CEILING_DIRECTORIES="$ROOT/$OUT" git apply --verbose --whitespace=nowarn "$ROOT/$p" 2>&1)" \
    || { echo "$out" >&2; exit 1; }
  grep -q "^Applied patch .* cleanly" <<<"$out" || { echo "patch $p did not apply: $out" >&2; exit 1; }
  echo "applied $p"
done

python3 tools/spu/lift_spu.py --ps3recomp "$PT" --out "$OUT" "$@"
python3 tools/spu/spu_audit.py "$OUT/spu_lift.json" > "$OUT/spu_audit.txt"
python3 tools/spu/job_refs.py --out "$OUT/job_refs.tsv" > "$OUT/job_refs.txt"
sed -n '1,/^== instruction/p' "$OUT/spu_audit.txt"
head -1 "$OUT/job_refs.txt"
python3 tools/spu/gen_spu_register.py
python3 tools/spu/test_spu_register.py
