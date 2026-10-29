#!/usr/bin/env bash
# Memory-safe Windows build: KZ3_BUILD_SLOTS builds at a time (default 1) across the main checkout and every parallel worktree,
# with the job count capped by available memory. Run inside the sandbox:
#   nix develop .#win --command tools/sandbox.sh -- tools/build-kz3.sh build/kz3-win [extra cmake --build args]
# Why: a cc1plus on an old 600k-line lifted PPU chunk peaked at 6.7 GB; -j8/-j20 builds next to Proton and RPCS3 ran
# the 31 GB machine out of memory three times on 2026-10-08 (the kernel killed VS Code). Jobs = (MemAvailable - 8 GB) /
# 1.5 GB / slots in use, 1..8 with the 64-chunk lift (patch 3005, 2026-10-11); (MemAvailable - 8 GB) / 4.5 GB, 1..4
# with an 8-chunk one. Holds a build slot: work/locks/build.lock (slot 0) or build.N.lock (main checkout's work/locks;
# other worktrees see it writable through the sandbox).
set -euo pipefail
DIR="${1:?usage: build-kz3.sh BUILD_DIR [cmake --build args]}"; shift
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/tools/pyresolve.sh"   # PY: python3, else py -3, else python (never the Microsoft Store alias)
LOCKS="${KZ3_MAIN:-$ROOT}/work/locks"; mkdir -p "$LOCKS"
# Refuse to build from a patched toolkit older than the patch set (run tools/ps3recomp-patched.sh, or kz3-check.sh,
# first). KZ3_ALLOW_STALE_PATCHES=1 overrides, for builds that are meant to test an older set.
if [[ -d "$ROOT/work/ps3recomp-patched" && "${KZ3_ALLOW_STALE_PATCHES:-0}" != 1 ]]; then
  want="$(cat "$ROOT"/port/patches/*.patch | sha256sum | cut -c1-16)-$(git -C "$ROOT/third_party/ps3recomp" rev-parse --short HEAD 2>/dev/null)"
  have="$(cat "$ROOT/work/ps3recomp-patched/.kz3-patchset" 2>/dev/null)"
  # No stamp yet (a tree made before 2026-10-09): warn only, so open parallel builds keep building.
  if [[ -z "$have" ]]; then echo "build-kz3: warning: work/ps3recomp-patched has no patch-set stamp" >&2
  else [[ "$have" == "$want" ]] || { echo "build-kz3: work/ps3recomp-patched is stale (patch set changed): run tools/ps3recomp-patched.sh first" >&2; exit 3; }; fi
fi
# The lifted PPU code must match the override list: a host override (port/spu/kz3_ppu_overrides.cpp) is compiled only
# when the lift announced PPU_OVERRIDE_<addr>, so a newer ppu_overrides.txt without a relift builds fine with the new
# hooks silently left out (2026-10-11, SPU pass 32's logger hooks). Refuse; relifting takes minutes (tools/lift.sh).
# Content, not time (RSX pass 34, 2026-10-11: every rebase that touched ppu_overrides.txt refused a current lift by mtime):
# the addresses in the list must equal the PPU_OVERRIDE_<addr> announcements of the lift this build compiles
# (ppu_overrides.h since patch 3005, ppu_recomp.h before). No announcement file = no lift yet: the build says so itself.
# A build dir not configured yet has no CMakeCache.txt: say so and check the default lift (2026-10-12: the sed's
# exit 2 on the missing file ended this script silently under set -e, e.g. a remote build's guards in a fresh
# checkout; the remote build configures on the server).
# Which PPU and SPU lift this build dir compiles: its CMakeCache (RECOMP_DIR, KZ3_SPU_DIR) when configured, else the
# version's own pair from tools/spu/kz3ver.py (default 114: work/recomp + work/spu; KZ3_EBOOT_VER=100: work/recomp-100 +
# work/spu-100; the KZ3_RECOMP_DIR / KZ3_SPU_DIR environment overrides, as for tools/lift.sh) which the configure below
# passes. One pair per version, so a 1.14 and a 1.00 build dir sit side by side (re-data rounds 144, 149).
# KZ3_PRINT_LIFTS=1: print "VER RECOMP SPU LIFTVER" (the selected version, the pair this dir would use, the PPU lift's
# own version) and exit (tools/assets/test_build_kz3.sh).
recomp_l="" spu_l=""
KV="$(cd "$ROOT" && $PY tools/spu/kz3ver.py ver recomp spu)" || exit 2
read -r def_ver def_recomp def_spu <<<"$KV"
if [[ -f "$DIR/CMakeCache.txt" ]]; then
  recomp_l="$(sed -n 's/^RECOMP_DIR:[A-Z]*=//p' "$DIR/CMakeCache.txt")"
  spu_l="$(sed -n 's/^KZ3_SPU_DIR:[A-Z]*=//p' "$DIR/CMakeCache.txt")"
else
  echo "build-kz3: note: $DIR has no CMakeCache.txt (not configured yet): it is configured below for the lifts of EBOOT $def_ver" >&2
fi
if [[ -z "$recomp_l" ]]; then
  recomp_l="${KZ3_RECOMP_DIR:-$def_recomp}"
  [[ "$recomp_l" == /* ]] || recomp_l="$ROOT/$recomp_l"
fi
if [[ -z "$spu_l" ]]; then
  spu_l="${KZ3_SPU_DIR:-$def_spu}"
  [[ "$spu_l" == /* ]] || spu_l="$ROOT/$spu_l"
fi
# The two lifts must be the same EBOOT version (the SPU registration fingerprints the version's images): the PPU lift's
# stamp says which ELF it came from, the SPU lift's eboot_ver file which version it was made for. A 1.14 PPU lift built
# against the 1.00 SPU lift (or the reverse) links and then fails at run time.
ppu_ver="$(cd "$ROOT" && $PY tools/spu/kz3ver.py lift-ver "$recomp_l")" || exit 2
if [[ "${KZ3_PRINT_LIFTS:-0}" == 1 ]]; then echo "$def_ver $recomp_l $spu_l $ppu_ver"; exit 0; fi
if [[ -f "$recomp_l/ppu_recomp.h" && -f "$spu_l/eboot_ver" && "$(tr -d '[:space:]' < "$spu_l/eboot_ver")" != "$ppu_ver" ]]; then
  echo "build-kz3: the PPU lift $recomp_l is $ppu_ver but the SPU lift $spu_l holds $(tr -d '[:space:]' < "$spu_l/eboot_ver"):" \
       "run KZ3_EBOOT_VER=$ppu_ver tools/spu/lift_spu.sh (1.14 writes work/spu, 1.00 work/spu-100)," \
       "or point the build dir at the matching pair" >&2
  exit 3
fi
# Regenerate the SPU registration TU when its generator changed since it was written (2026-10-11: SPU pass 30's generator
# added kz3_spu_image_names, main's work/spu/gen was older, and kz3.exe failed to link after the merge). Content, not
# time: the generator stamps its own sha256 in gen/.generator-sha256 (a rebase only touches the mtime); a gen dir
# without the stamp (written before 2026-10-21) falls back to the mtime test.
gen="$spu_l/gen/kz3_spu_register.c"
gen_tool="$ROOT/tools/spu/gen_spu_register.py"
gen_stale=0
if [[ -f "$gen" ]]; then
  if [[ -f "$spu_l/gen/.generator-sha256" ]]; then
    [[ "$(tr -d '[:space:]' < "$spu_l/gen/.generator-sha256")" == "$(sha256sum "$gen_tool" | cut -d' ' -f1)" ]] || gen_stale=1
  elif [[ "$gen_tool" -nt "$gen" ]]; then gen_stale=1; fi
fi
if (( gen_stale )); then
  echo "build-kz3: regenerating $spu_l/gen (tools/spu/gen_spu_register.py changed)" >&2
  (cd "$ROOT" && KZ3_EBOOT_VER="$ppu_ver" $PY tools/spu/gen_spu_register.py --lift "$spu_l" --out "$spu_l/gen" >/dev/null)
fi
# The table that matches the lift being built (1.14 series step 4): 1.14 lifts are made from ppu_overrides-114.txt.
table="$("$ROOT/tools/assets/lift_table.sh" "$ROOT" "$recomp_l")"
ann="$recomp_l/ppu_overrides.h"; [[ -f "$ann" ]] || ann="$recomp_l/ppu_recomp.h"
if [[ -f "$ann" && "${KZ3_ALLOW_STALE_LIFT:-0}" != 1 ]]; then
  want="$(grep -oE '^[[:space:]]*[0-9A-Fa-f]{8}' "$table" | tr -d ' \t' | tr a-f A-F | sort -u)"
  have="$(grep -oE 'PPU_OVERRIDE_[0-9A-Fa-f]{8}' "$ann" | cut -c14- | tr a-f A-F | sort -u)"
  if [[ "$want" != "$have" ]]; then
    echo "build-kz3: the lift in $recomp_l does not announce the overrides of ${table#"$ROOT"/}" \
         "(missing: $(comm -23 <(echo "$want") <(echo "$have") | tr '\n' ' ')extra: $(comm -13 <(echo "$want") <(echo "$have") | tr '\n' ' '))" \
         "- run tools/lift.sh first (nix develop .#re --command tools/sandbox.sh -- tools/lift.sh), or KZ3_ALLOW_STALE_LIFT=1" >&2
    exit 3
  fi
fi
# KZ3_BUILD_GUARDS_ONLY=1: run only the guards above and stop (used by a developer's remote build wrapper).
[[ "${KZ3_BUILD_GUARDS_ONLY:-0}" == 1 ]] && exit 0
# Build slots (2026-10-11): KZ3_BUILD_SLOTS builds may run at once, one slot each (slot 0 = build.lock, slot 1 =
# build.1.lock; tools/agents/slotlock.sh); this build's job count is the free-memory formula below divided by the
# slots in use (this one included). The default stays 1 on this machine: a compiler on a lifted chunk peaks at
# 5.3 GB now (sampled 2026-10-11 over the day's builds; 4.5 GB on main's v1.00 chunks), so two builds at -j2 would
# hold 21 GB of the ~26 GB a quiet machine has, under 6 GB free before a Proton run (3.6 GB) joins; the lifts take
# every slot (they replace what a build globs). Set KZ3_BUILD_SLOTS=2 on a machine with more memory.
# shellcheck source=agents/slotlock.sh
source "$ROOT/tools/agents/slotlock.sh"
SLOTS="${KZ3_BUILD_SLOTS:-1}"; [[ "$SLOTS" =~ ^[1-9][0-9]*$ ]] || SLOTS=1
# A parent that already holds a build slot (tools/agents/lock.sh build -- ..., which exports KZ3_HOLDS_build=1 like the
# ninja shim reads) keeps it for this build: taking another slot here waited on the parent's own lock forever
# (2026-10-15: two maintainer chains wrapped this script in lock.sh build and sat 90 minutes at flock).
[[ "${KZ3_HOLDS_build:-0}" == 1 ]] || slot_take "$LOCKS" build "$SLOTS"
in_use=$(slot_held "$LOCKS" build "$SLOTS"); (( in_use < 1 )) && in_use=1
# Third OOM (2026-10-08 14:55): -j7 sized from memory free at start (7 cc1plus = ~21.5 GB) plus a Proton run and the
# desktop grew past 31 GB and the kernel killed VS Code (oom_score_adj 300) before the compilers (200). So: keep an
# 8 GB reserve, size the jobs by a per-job estimate, cap them, and make this build the kernel's first OOM victim (1000),
# so a runaway build fails instead of the editor.
# Per-job estimate (2026-10-11, patch 3005: 64 lifted chunks of 51-73k lines instead of 8 of 600k): a chunk's cc1plus
# peaks at 0.66-1.33 GB (RelWithDebInfo, 55 of 64 chunks measured), the largest SPU image at 1.31 GB, an old 600k-line
# chunk at 6.7 GB (378 s); so 1.5 GB per job, cap 8 (KZ3_BUILD_JOBS_MAX overrides the cap; KZ3_BUILD_JOB_MB the
# estimate). An 8-chunk lift (no ppu_recomp_table.cpp) keeps 4.5 GB / 4.
echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true
if [[ -r /proc/meminfo ]]; then
  avail_mb=$(( $(awk '/MemAvailable/{print $2}' /proc/meminfo) / 1024 ))
else   # macOS build hosts (docs/BUILDING.md): no /proc; take 60 % of physical memory as available
  avail_mb=$(( $(sysctl -n hw.memsize) * 6 / 10 / 1048576 ))
fi
job_mb="${KZ3_BUILD_JOB_MB:-1536}"; jobs_max="${KZ3_BUILD_JOBS_MAX:-8}"
# The lift this build compiles is the build dir's RECOMP_DIR (a worktree can point it at its own lift, e.g. a separate SPU lift:
# work/spu/ppu-recomp), not always work/recomp: an old 8-chunk lift there once ran 8 x 3.5 GB jobs (2026-10-11).
recomp="$recomp_l"   # read above (an unconfigured dir: the lift of KZ3_EBOOT_VER)
if [[ -f "$recomp/ppu_recomp.h" && ! -f "$recomp/ppu_recomp_table.cpp" && -z "${KZ3_BUILD_JOB_MB:-}" ]]; then
  job_mb=4608; jobs_max=4
fi
jobs=$(( (avail_mb - 8192) / job_mb / in_use )); (( jobs < 1 )) && jobs=1; (( jobs > jobs_max )) && jobs=$jobs_max
# Temporary cap while the machine is in use (work/locks/build-cap = "EXPIRY_EPOCH MAXJOBS", set by the maintainer; ignored once
# expired). Builds also run at nice 10 while it is active.
if [[ -r "$LOCKS/build-cap" ]] && read -r cap_until cap_jobs < "$LOCKS/build-cap" && (( $(date +%s) < cap_until )); then
  (( jobs > cap_jobs )) && jobs=$cap_jobs; renice -n 10 -p $$ >/dev/null 2>&1 || true
fi
avail_gb=$(( avail_mb / 1024 ))
echo "build-kz3: $DIR with -j$jobs (${avail_gb} GB available; build slot ${SLOT:-held by the caller}, $in_use of $SLOTS in use)" >&2
# ccache (in the .#win shell since 2026-10-09): a re-lift or a branch switch regenerates the 2.5-4.4 GB-per-job lifted
# chunks with mostly the same content; ccache serves those from work/ccache instead of recompiling. CCACHE_BASEDIR
# makes hits independent of the checkout path. KZ3_CCACHE=0 turns it off.
if command -v ccache >/dev/null && [[ "${KZ3_CCACHE:-1}" != 0 ]]; then
  export CCACHE_DIR="$ROOT/work/ccache" CCACHE_BASEDIR="$ROOT" CCACHE_MAXSIZE=40G CCACHE_COMPRESS=1
  grep -q '^CMAKE_CXX_COMPILER_LAUNCHER:.*=ccache' "$DIR/CMakeCache.txt" 2>/dev/null \
    || cmake -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache "$DIR" >/dev/null
elif grep -q '^CMAKE_CXX_COMPILER_LAUNCHER:.*=ccache' "$DIR/CMakeCache.txt" 2>/dev/null; then
  cmake -UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER "$DIR" >/dev/null   # no ccache here: plain compiler
fi
# A build dir that does not exist yet is configured here, for the lifts resolved above (2026-10-21: build-kz3.sh stopped at
# "cmake --build" on a new dir; the 1.14 dir had to be configured by hand with the right -DRECOMP_DIR/-DKZ3_SPU_DIR). The
# arguments are the documented ones (the testing notes, "KZ3 exe"); other options are cached by cmake.
if [[ ! -f "$DIR/CMakeCache.txt" ]]; then
  [[ -f "$recomp_l/ppu_recomp.h" ]] || { echo "build-kz3: no lift in $recomp_l (run tools/lift.sh; KZ3_EBOOT_VER=100 for 1.00)" >&2; exit 3; }
  [[ -f "$spu_l/gen/kz3_spu_register.c" ]] || { echo "build-kz3: no SPU lift in $spu_l (run tools/spu/lift_spu.sh)" >&2; exit 3; }
  echo "build-kz3: configuring $DIR (RECOMP_DIR=$recomp_l KZ3_SPU_DIR=$spu_l)" >&2
  cmake -S "$ROOT/port/kz3" -B "$DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE="$($PY -c 'import sys; print(sys.executable)')" \
        -DRECOMP_DIR="$recomp_l" -DKZ3_SPU_DIR="$spu_l" >/dev/null || exit 4
  cat "$ROOT/work/ps3recomp-patched/.kz3-patchset" > "$DIR/.kz3-toolkit-stamp" 2>/dev/null || true
fi
# Re-configure when the patched toolkit changed (queue 9m, SPU pass 46; KZ2's build-kz2.sh does the same): ps3recomp
# collects its sources with file(GLOB_RECURSE) without CONFIGURE_DEPENDS, so a patch that adds a source file is never
# linked from an old cache. The stamp is tools/ps3recomp-patched.sh's .kz3-patchset.
if [[ -f "$DIR/CMakeCache.txt" ]]; then
  pt_stamp="$(cat "$ROOT/work/ps3recomp-patched/.kz3-patchset" 2>/dev/null || true)"
  if [[ -n "$pt_stamp" && "$(cat "$DIR/.kz3-toolkit-stamp" 2>/dev/null || true)" != "$pt_stamp" ]]; then
    echo "build-kz3: patch set changed since $DIR was configured: re-running CMake" >&2
    cmake "$DIR" >/dev/null || exit 4
    echo "$pt_stamp" > "$DIR/.kz3-toolkit-stamp"
  fi
fi
t0=$(date +%s); rc=0
cmake --build "$DIR" -j"$jobs" "$@" || rc=$?
echo "build-kz3: done in $(( $(date +%s) - t0 )) s (-j$jobs, exit $rc)" >&2
exit $rc
