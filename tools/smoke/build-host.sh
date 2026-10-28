#!/usr/bin/env bash
# Build the D3D12 smoke hosts for Windows/Proton/NVIDIA:
#   build/smoke/host_kz3.exe  tools/smoke/host_kz3.c: hand-built RSX command buffer -> cellGcm FIFO -> D3D12
#                             backend -> present, on a real 4 GB guest reservation. Draws four quads that test
#                             per-unit sampler state (patch 2001; see the RE notes).
#                             --live: through rsx_live_draw; --glyph: stencil-then-cover glyph cache (patch 2006).
#   build/smoke/test_gcm_reports.exe  tools/smoke/test_gcm_reports.c: patch 2013 report-context host test (run it
#                             under Proton with tools/proton-test.sh; exit 0 and "[test_gcm_reports] PASS").
#   build/smoke/test_tex_float.exe  tools/smoke/test_tex_float.c: patch 2014 host test, float textures and the F_X32
#                             surface through live-draw, read back raw and compared bit-exactly (Proton, GPU lock).
#   build/smoke/test_nv0039.exe  tools/smoke/test_nv0039.c: patch 2015 host test, NV0039 buffer copies (run it
#                             with proton-test.sh; exit 0 + "[test_nv0039] PASS").
#   build/smoke/test_guest_write.exe  tools/smoke/test_guest_write.c: patch 2017 host test, guest writes behind
#                             live-draw (Proton, GPU lock; exit 0 + "[test_guest_write] PASS").
#   build/smoke/test_wb_batch.exe  tools/smoke/test_wb_batch.c: patch 2018 host test + benchmark, KZ3-shaped main-memory
#                             write-backs batched into one GPU wait (LD_WRITEBACK_BATCH=0 = per surface; exit 0 + PASS).
#   build/smoke/test_surface_aa.exe  tools/smoke/test_surface_aa.c: patch 2016 host test, AA surfaces at sample
#                             resolution (Proton, GPU lock; exit 0 + "[test_surface_aa] PASS").
#   build/smoke/test_cond_render.exe  tools/smoke/test_cond_render.c: patch 2025 host test, SET_RENDER_ENABLE mode 2
#                             from ZPASS reports (occluded and empty reports skip their draws, a visible one draws;
#                             Proton, GPU lock; exit 0 + PASS; LD_COND_RENDER=0 must FAIL). Patch 2027: also checks
#                             the guest report values (A 16, B 0, C 0); LD_OQ_READBACK=0 must FAIL (A, B 0xFFFF).
#   build/smoke/test_depth_export.exe  tools/smoke/test_depth_export.c: patch 2021 host test, fragment depth export into a
#                             Z16 zeta and its X16 read with texel coordinates from a larger zeta resource (Proton,
#                             GPU lock; exit 0 + PASS; --no-export and LD_NO_ZALIAS_UNNORM_FIX=1 must FAIL).
#   build/smoke/test_zeta_reads.exe  tools/smoke/test_zeta_reads.c: patch 2022 host test, a zeta read as X16 by a pass
#                             that has it bound (depth write off), with a colour surface at the same address and a
#                             snapshot first sized by a smaller clip (Proton, GPU lock; exit 0 + PASS;
#                             LD_NO_ZSELF_SNAP=1, LD_NO_ZETA_PREFER=1 and LD_NO_ZSNAP_GROW=1 must FAIL).
#   build/smoke/test_vtx_cache.exe  tools/smoke/test_vtx_cache.c: patch 2047 host test, the vertex payload cache
#                             (LD_VTX_CACHE) against tracked, untracked and previous-frame writes (Proton, GPU
#                             lock; KZ3_VC_MODE=1|verify|0; exit 0 + "[test_vtx_cache] PASS").
#   build/smoke/test_zeta_argb.exe  tools/smoke/test_zeta_argb.c: patch 2044 host test, a Z24S8 zeta read as A8R8G8B8
#                             after a colour-masked pass made a colour surface at its address (depth wins), and after a
#                             real colour write (colour wins) (Proton, GPU lock; LD_NO_ZETA_ARGB_PREFER=1 must FAIL).
#   build/smoke/test_host_aa.exe  tools/smoke/test_host_aa.c: patch 2023 host test, the host FXAA pass before an NV0039
#                             scene copy: flat pixels bit-identical, alpha kept, >= 25 % of edge pixels changed (Proton,
#                             GPU lock; KZ3_HOST_AA=fxaa or --arm PASS; KZ3_HOST_AA=off and unset without --arm FAIL).
#   build/smoke/test_gcm_usercmd.exe  tools/smoke/test_gcm_usercmd.c: patch 1027 host test, GCM_SET_USER_COMMAND in
#                             the walker reaches the HLE cellGcmSetUserHandler handler. test_gcm_usercmd_no1027.exe is
#                             the same test linked against cellGcmSys.c with 1027 reverse-applied (expected FAIL).
#   build/smoke/test_gcm_iomap.exe  tools/smoke/test_gcm_iomap.c: patch 2019 host test, 40 MapMainMemory chunks of 1 MB
#                             map, translate both ways and resolve in the walker (exit 0 + PASS); test_gcm_iomap_16.exe
#                             is the same with the old 16-slot limit (expected FAIL).
#   build/smoke/crash/test_crash_dump.exe  tools/smoke/test_crash_dump.c: crash-handler minidump test (RSX pass 33), no
#                             runtime, no window: child "bug" = round-32 dump sequence (expected to die inside dbghelp),
#                             child "fix" = kz3_log_write_minidump (expected: dump + exit 70). proton-test.sh --seconds 40
#                             ... test_crash_dump.exe --all; exit 0 + "[test_crash_dump] PASS". No GPU lock needed.
#   build/smoke/host.exe      upstream runtime/host/host_main.c unchanged (64 MB guest; crashes in cellGcmInit).
#
# Source tree: PS3RECOMP_SRC (default work/ps3recomp-patched, made by tools/ps3recomp-patched.sh: the pinned
# submodule + port/patches/*.patch). The runtime library is configured and built into PS3RECOMP_LIB
# (default build/ps3recomp-patched-win) if needed. Run inside the sandbox from `nix develop .#win`:
#   nix develop .#win --command tools/sandbox.sh -- tools/smoke/build-host.sh
set -euo pipefail
S="${PS3RECOMP_SRC:-work/ps3recomp-patched}"
L="${PS3RECOMP_LIB:-build/ps3recomp-patched-win}"
O=build/smoke
[[ -f "$S/CMakeLists.txt" ]] || { echo "no ps3recomp tree at $S (run tools/ps3recomp-patched.sh)" >&2; exit 1; }
if [[ ! -f "$L/build.ninja" ]]; then
  cmake -S "$S" -B "$L" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw-w64.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS3RECOMP_BUILD_TESTS=OFF >/dev/null
fi
# Re-configure when the patch set changed (as tools/build-kz3.sh does): ps3recomp globs its sources without
# CONFIGURE_DEPENDS, so a patch that adds a file (2069's libs/input/pad_mouse.c, RSX pass 65 merge 2026-10-15) was never
# compiled from an old cache and the link failed. The stamp is tools/ps3recomp-patched.sh's .kz3-patchset.
pt_stamp="$(cat "$S/.kz3-patchset" 2>/dev/null || true)"
if [[ -n "$pt_stamp" && "$(cat "$L/.kz3-toolkit-stamp" 2>/dev/null || true)" != "$pt_stamp" ]]; then
  cmake "$L" >/dev/null
  echo "$pt_stamp" > "$L/.kz3-toolkit-stamp"
fi
cmake --build "$L" --target ps3recomp_runtime
mkdir -p "$O"
CC=x86_64-w64-mingw32-gcc
CFLAGS=(-std=c11 -O2 -I"$S/include" -I"$S/libs/video" -I"$S/runtime")
LIBS=("$L/libps3recomp_runtime.a"
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lgdi32 -luser32 -lole32 -lws2_32 -lbcrypt
  -lwinmm -lshlwapi -lsynchronization -lpsapi -ldbghelp)
"$CC" "${CFLAGS[@]}" -c "$S/runtime/host/host_stubs.c" -o "$O/host_stubs.o"
"$CC" "${CFLAGS[@]}" -c tools/smoke/host_shim.c -o "$O/host_shim.o"

"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/host_kz3.c -o "$O/host_kz3.o"
x86_64-w64-mingw32-g++ -static -o "$O/host_kz3.exe" "$O/host_kz3.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/host_kz3.exe (from $S)"

# Patch 2013 host test: SET_CONTEXT_DMA_REPORT + GET_REPORT through the FIFO walker, no graphics backend.
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_gcm_reports.c -o "$O/test_gcm_reports.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_gcm_reports.exe" "$O/test_gcm_reports.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_gcm_reports.exe"

# Patch 2014 host test: FP16 textures (W16Z16Y16X16_F, Y16_X16_F) and the F_X32 surface through live-draw, read back raw.
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_tex_float.c -o "$O/test_tex_float.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_tex_float.exe" "$O/test_tex_float.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_tex_float.exe"

# Patch 2015 host test: NV0039 buffer copies (main<->local, pitches, same-frame texture re-hash) through the walker.
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_nv0039.c -o "$O/test_nv0039.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_nv0039.exe" "$O/test_nv0039.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_nv0039.exe"
# Patch 2016 host test: AA surfaces at sample resolution (SQUARE_ROTATED_4 4x4 -> 8x8 host surface), bit-exact.
# Patch 2017 host test: guest writes behind live-draw (live surfaces and textures), hash validation and the hook.
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_guest_write.c -o "$O/test_guest_write.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_guest_write.exe" "$O/test_guest_write.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_guest_write.exe"

# Patch 2018 host test + benchmark: batched main-memory write-backs at a guest sync point.
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_wb_batch.c -o "$O/test_wb_batch.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_wb_batch.exe" "$O/test_wb_batch.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_wb_batch.exe"

"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_surface_aa.c -o "$O/test_surface_aa.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_surface_aa.exe" "$O/test_surface_aa.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_surface_aa.exe"
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_cond_render.c -o "$O/test_cond_render.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_cond_render.exe" "$O/test_cond_render.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_cond_render.exe"
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_depth_export.c -o "$O/test_depth_export.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_depth_export.exe" "$O/test_depth_export.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_depth_export.exe"
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_zeta_reads.c -o "$O/test_zeta_reads.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_zeta_reads.exe" "$O/test_zeta_reads.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_zeta_reads.exe"
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_zeta_argb.c -o "$O/test_zeta_argb.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_zeta_argb.exe" "$O/test_zeta_argb.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_zeta_argb.exe"
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_vtx_cache.c -o "$O/test_vtx_cache.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_vtx_cache.exe" "$O/test_vtx_cache.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_vtx_cache.exe"

"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_host_aa.c -o "$O/test_host_aa.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_host_aa.exe" "$O/test_host_aa.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_host_aa.exe"

# Patch 1027 host test, with and without 1027. The no-1027 variant compiles cellGcmSys.c as the series minus 1027 and
# links it ahead of the runtime archive, so the archive's cellGcmSys object is never pulled (1027 only touches it).
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_gcm_usercmd.c -o "$O/test_gcm_usercmd.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_gcm_usercmd.exe" "$O/test_gcm_usercmd.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
# cellGcmSys.c's include flags from build.ninja, as an array: with a space in the checkout path ("Killzone 3") cmake
# writes -I"/home/.../Killzone 3/...", and an unquoted expansion split them (pass 62). xargs undoes the quoting.
mapfile -d '' -t GCM_INC < <(awk '/cellGcmSys\.c\.obj: /{f=1} f&&/INCLUDES =/{sub(/^ *INCLUDES = /,""); print; exit}' \
  "$L/build.ninja" | xargs printf '%s\0')
(( ${#GCM_INC[@]} > 0 )) || { echo "no INCLUDES for cellGcmSys.c in $L/build.ninja" >&2; exit 1; }
# Independent of later patches: reverse, newest first, the cellGcmSys.c part of every patch from 1027 on (in the
# series order of tools/ps3recomp-patched.sh), so the file ends as "series up to but excluding 1027".
cp "$S/libs/video/cellGcmSys.c" "$O/cellGcmSys_no1027.c"
# A file section starts at its "--- a/PATH" line (some patches have "diff --git" headers, some not).
gcm_part() { awk '/^diff --git /{f=0; next} /^--- a\//{f=($2=="a/libs/video/cellGcmSys.c")} f' "$1"; }
mapfile -t _later < <(printf '%s\n' port/patches/*.patch | awk '/\/1027-/{f=1} f' | sort -r)
for p in "${_later[@]}"; do
  part="$(gcm_part "$p")"; [[ -n "$part" ]] || continue
  patch -s -R "$O/cellGcmSys_no1027.c" <<<"$part" || { echo "cellGcmSys.c part of $p does not reverse" >&2; exit 1; }
done
# ...then re-apply the later ones (oldest first), so the file is the full series minus 1027 (other runtime objects
# call functions later patches add, e.g. 2007's rsx_perf_*; without them the archive's cellGcmSys gets pulled).
for ((k=${#_later[@]}-1; k>=0; k--)); do
  p="${_later[k]}"; [[ "$p" == *"/1027-"* ]] && continue
  part="$(gcm_part "$p")"; [[ -n "$part" ]] || continue
  # A later part that instruments 1027's own code (1029's frame-chain trace) cannot exist without it: skip it.
  if ! patch -s --dry-run "$O/cellGcmSys_no1027.c" <<<"$part" >/dev/null 2>&1; then
    echo "no-1027 control: cellGcmSys.c part of $p depends on 1027, left out"; continue; fi
  patch -s "$O/cellGcmSys_no1027.c" <<<"$part"
done
rm -f "$O/cellGcmSys_no1027.c.orig"
! grep -q gcm_walker_user_command "$O/cellGcmSys_no1027.c" || { echo "1027 did not reverse" >&2; exit 1; }
"$CC" -O2 -std=gnu17 -fno-strict-aliasing -ffp-contract=off -msse4.1 -w -iquote "$S/libs/video" "${GCM_INC[@]}" \
  -c "$O/cellGcmSys_no1027.c" -o "$O/cellGcmSys_no1027.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_gcm_usercmd_no1027.exe" "$O/test_gcm_usercmd.o" "$O/cellGcmSys_no1027.o" \
  "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_gcm_usercmd.exe and $O/test_gcm_usercmd_no1027.exe"
# Patch 2019: many MapMainMemory chunks. _16 = cellGcmSys.c (and the test) with the old 16-slot limit (expected FAIL).
"$CC" "${CFLAGS[@]}" -Wall -c tools/smoke/test_gcm_iomap.c -o "$O/test_gcm_iomap.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_gcm_iomap.exe" "$O/test_gcm_iomap.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
"$CC" "${CFLAGS[@]}" -Wall -DCELL_GCM_MAX_IO_MAPPINGS=16 -c tools/smoke/test_gcm_iomap.c -o "$O/test_gcm_iomap_16.o"
"$CC" -O2 -std=gnu17 -fno-strict-aliasing -ffp-contract=off -msse4.1 -w -iquote "$S/libs/video" "${GCM_INC[@]}" \
  -DCELL_GCM_MAX_IO_MAPPINGS=16 -c "$S/libs/video/cellGcmSys.c" -o "$O/cellGcmSys_16.o"
x86_64-w64-mingw32-g++ -static -o "$O/test_gcm_iomap_16.exe" "$O/test_gcm_iomap_16.o" "$O/cellGcmSys_16.o" \
  "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/test_gcm_iomap.exe and $O/test_gcm_iomap_16.exe"

# host_main.c predates the runtime defining g_ps3_guest_caller (cellSysutil.c); rename its copy.
"$CC" "${CFLAGS[@]}" -Dg_ps3_guest_caller=kz3_host_unused_guest_caller -c "$S/runtime/host/host_main.c" -o "$O/host_main.o"
x86_64-w64-mingw32-g++ -static -o "$O/host.exe" "$O/host_main.o" "$O/host_stubs.o" "$O/host_shim.o" "${LIBS[@]}"
echo "built $O/host.exe"

# crash-handler minidump test (RSX pass 33): kz3_log + an unhandled-exception filter, no runtime. Own directory so
# proton-test.sh copies only this exe.
mkdir -p "$O/crash"
"$CC" -std=gnu11 -Wall -Wextra -Wno-format-truncation -O1 -g -Iport/kz3 -static -o "$O/crash/test_crash_dump.exe" \
  tools/smoke/test_crash_dump.c port/kz3/kz3_log.c
echo "built $O/crash/test_crash_dump.exe"
