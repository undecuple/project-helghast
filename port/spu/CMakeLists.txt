# kz3_spu: KZ3's lifted SPU programs + their registration with ps3recomp's runtime (SPU).
#
# The C sources are generated from the game binary and are not committed. Make them first:
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/lift_spu.sh -j 8
# which lifts into work/spu/lift/<image>/ and runs tools/spu/gen_spu_register.py (work/spu/gen/).
#
# Use from port/kz3/CMakeLists.txt (after PS3RECOMP_DIR is set and the runtime is added):
#   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../spu" "${CMAKE_BINARY_DIR}/kz3_spu")
#   target_link_libraries(${PROJECT_NAME} PRIVATE kz3_spu)
#
# kz3_spu is an OBJECT library: the registration TU self-registers from a constructor and nothing references
# it by name, so as a static archive member it would be dropped by the linker. Linking an OBJECT library
# puts every object on the link line. kz3_spu_register_all() may also be called explicitly (idempotent).
#
# Standalone (compile check only):
#   cmake -S port/spu -B build/kz3-spu -DPS3RECOMP_DIR=<patched ps3recomp> [-DCMAKE_TOOLCHAIN_FILE=...]
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(kz3_spu LANGUAGES C)
endif()

if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../work/ps3recomp-patched" ABSOLUTE)
endif()
set(KZ3_SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../work/spu" CACHE PATH
    "tools/spu/lift_spu.sh output: lift/<image>/spu_recomp.c and gen/kz3_spu_register.c")
get_filename_component(KZ3_SPU_DIR "${KZ3_SPU_DIR}" ABSOLUTE)
set(KZ3_SPU_GEN "${KZ3_SPU_DIR}/gen")

foreach(f "${KZ3_SPU_GEN}/sources.txt" "${KZ3_SPU_GEN}/kz3_spu_register.c"
          "${PS3RECOMP_DIR}/runtime/spu/spu_context.h")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "kz3_spu: missing ${f}. Run tools/spu/lift_spu.sh (lift + gen_spu_register.py) "
                            "and tools/ps3recomp-patched.sh, or set KZ3_SPU_DIR / PS3RECOMP_DIR.")
    endif()
endforeach()

# Only the images the generated table registers (gen_spu_register.py --only can narrow it).
file(STRINGS "${KZ3_SPU_GEN}/sources.txt" KZ3_SPU_IMAGES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KZ3_SPU_GEN}/sources.txt")
set(KZ3_SPU_LIFTED "")
foreach(img IN LISTS KZ3_SPU_IMAGES)
    if(img)
        list(APPEND KZ3_SPU_LIFTED "${KZ3_SPU_DIR}/lift/${img}/spu_recomp.c")
    endif()
endforeach()
list(LENGTH KZ3_SPU_LIFTED KZ3_SPU_COUNT)
message(STATUS "kz3_spu:         ${KZ3_SPU_COUNT} lifted SPU image(s) from ${KZ3_SPU_DIR}")

add_library(kz3_spu OBJECT
    ${KZ3_SPU_LIFTED}
    "${KZ3_SPU_GEN}/kz3_spu_register.c"
    kz3_spu_skip.c   # KZ3_SPU_SKIP_OVL / KZ3_CULL_AB -> runtime patch 1043 (SPU pass 30)
    kz3_spu_cull.c   # KZ3_CULL_AB=empty44 entry hook -> runtime patch 1044 (SPU pass 31)
    kz3_gameplay.c   # KZ3_SP_UNLOCK / KZ3_GAMEPLAY_TRACE / Motion Sensor default helpers (SPU pass 33)
    kz3_fiber_stall.c     # fiber stall classification + breaker sequence (SPU pass 34, host-tested)
    kz3_fiber_stall_rt.c  # its runtime readers: per-SPU pc / resident job module via spu_pm_ctx_peek (patch 1011)
    kz3_cull_probe.c      # KZ3_CULL_PROBE=1: visibility result counts at the sync-word clear / +2 ms / rebuild (SPU pass 35)
    kz3_hud_probe.c       # KZ3_HUD_PROBE=1: HUD render call counts and gates (SPU pass 35)
    kz3_cull_planes.c     # KZ3_CULL_PLANES=1: dilated occlusion planes check at job 48 (SPU pass 41)
    kz3_arena_probe.c     # KZ3_ARENA_PROBE=1: frame-arena chunk reuse vs the block chain (RE pass 74, SPU pass 35)
    kz3_audio_probe.c     # KZ3_AUDIO_TICK=1: brbDriver tick / mix step / ring slot write times (SPU pass 37)
    kz3_mixjob_probe.c    # KZ3_AUDIO_VOICE=1: mix-pass jobs / taken voices / node ages per unit (SPU pass 71)
    kz3_stream_probe.c    # KZ3_STREAM_PROBE=1: Asset Streaming idle-wait dump at the quit hang (RE pass 106, SPU pass 48)
    kz3_cam_probe.c       # KZ3_CAM_PROBE=1: follow-camera / FOV helper dump (K2 black screen, RE pass 109 s.3, SPU pass 50)
    kz3_pkt_probe.c       # KZ3_PKT_PROBE=1: stream packet make/release balance (quit hang, RE pass 109 s.1, SPU pass 50)
    kz3_voice_probe.c     # KZ3_PKT_PROBE=1: stream voice ring head/tail/consumed + SPU DMA watch (quit hang, RE pass 112 s.4, SPU pass 51)
    kz3_mount_probe.c     # KZ3_MOUNT_PROBE=1: gunner mount chain counts and gates (K2 black, RE pass 112 s.2, SPU pass 51)
    kz3_ama_probe.c       # KZ3_AMA_PROBE=1: Amadeus mixer job-group wait / push dump (quit hang, RE pass 114 s.1, SPU pass 52)
    kz3_pool_probe.c      # KZ3_POOL_PROBE=1: GPU-pool handle registration / allocation (K2 unready ids, SPU pass 54)
    kz3_frame_trace.c     # KZ3_FRAME_TRACE=FROM,TO: event timeline file for tools/spu/frame_trace.py (patch 1085, SPU pass 66)
    kz3_jobhook.c         # KZ3_JOB_CAPTURE / KZ3_NATIVE_JOB: job I/O capture + native-job hook (patch 1141, SPU pass 120)
    kz3_native_post.c     # Gfx.Post native kernels (SPU pass 122)
    kz3_native_dof.c      # DepthOfField native kernels (SPU pass 122)
    kz3_native_lp.c       # LightProbes native kernels (SPU pass 123)
    kz3_native_jobs.c     # the job hook's KZ3 table (job manager LS layout, registered native jobs; empty)
    kz3_cam_watch.c       # KZ3_CAM_WATCH=1: page-guard write watch on the K2 render camera's matrix (SPU pass 56)
    kz3_hkquit_probe.c    # KZ3_JOIN2_PROBE=1: Havok pool teardown / JoinTask2 stall dump (T7 quit, SPU pass 57)
    kz3_ts_interp.c       # KZ3_TS_INTERP=1: TextureStreaming job (module 42) interpreted, lift vs inputs (K2, SPU pass 55)
    kz3_stall_diag.c      # KZ3_STALL_DIAG=1: SPU runs / 0x220A78 PPU waits not returned for 3 s (patch 1081, SPU pass 63)
    kz3_cpu_check.c       # KZ3_SPU_FMA: refuses to start on a CPU without AVX/FMA3, with a message (SPU pass 72)
    kz3_rail_probe.c      # KZ3_RAIL_PROBE=1: path-mover SetPath / Update / LogicStep counts (K2 Hammer rail, RE pass 114 s.2, SPU pass 52)
    # Indirect-branch fast path (musttail). The runtime library excludes it on purpose; the port compiles it
    # with its lifted SPU code (ps3recomp templates/project does the same). GCC >= 15 accepts musttail in C.
    "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c"
)
target_include_directories(kz3_spu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/include"
)
# SPU_USE_DISPATCH_MT routes the lifted code's indirect branches through spu_indirect_branch_mt.
target_compile_definitions(kz3_spu PRIVATE SPU_USE_DISPATCH_MT)
set_target_properties(kz3_spu PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Generated code: warnings off; no FP contraction (SPU fma/fms are explicit); -O2 like the template's
    # SPU sources. KZ3_SPU_OPT overrides the level (e.g. -O1 for faster iteration).
    set(KZ3_SPU_OPT "-O2" CACHE STRING "optimisation flag for the lifted SPU sources")
    # -msse4.1 (SPU pass 22, patch 1035): the helpers' pshufb shufb (was the 16-iteration byte loop, an
    # out-of-line call per shufb) and SSE4.1 lane ops. x86-64-v2, below the documented AVX2 minimum.
    # KZ3_SPU_ISA overrides the base flags; -mfma comes from KZ3_SPU_FMA below.
    set(KZ3_SPU_ISA "-msse4.1" CACHE STRING "ISA flags for the lifted SPU sources")
    # -mfma (SPU pass 72, patch 1097): spu__fma4 becomes one hardware fused multiply-add per vector (same bits as the
    # double path, tools/spu/test_spu_cg72.sh / test_spu_fastcg.sh / test_spu_xr.sh with -mfma), and the compiler uses
    # AVX encodings in the kz3_spu target. K2: audio synth job 127 -> 89 us (the RE notes Pass 72). An option of its
    # own so existing build trees (whose KZ3_SPU_ISA cache holds the old default) pick it up. Needs AVX + FMA3
    # (Haswell / Piledriver or newer); kz3_cpu_check.c (built without them) says so instead of crashing.
    option(KZ3_SPU_FMA "build the lifted SPU code with -mfma (needs AVX + FMA3)" ON)
    set(KZ3_SPU_ISA_ALL ${KZ3_SPU_ISA})
    if(KZ3_SPU_FMA)
        list(APPEND KZ3_SPU_ISA_ALL -mfma)
        target_compile_definitions(kz3_spu PRIVATE KZ3_SPU_NEEDS_FMA)
    endif()
    # -mcmodel=small (SPU pass 74): MinGW-w64 GCC defaults to the medium model on x86-64 PE and reaches every
    # extern variable through a .refptr pointer slot (one extra load per access, kept for the auto-import of DLL
    # data). The lifted code's per-hop gates (spu_hop_ok, spu_ib_guess_ok, spu_ls_dbg_armed) read ~15 runtime
    # globals per hop. kz3.exe is one image well below 2 GiB and this target reads no DLL data, so the small model
    # (direct RIP-relative loads) is exact. KZ3_SPU_CMODEL_SMALL=OFF builds the old code (A/B).
    option(KZ3_SPU_CMODEL_SMALL "build the lifted SPU code with -mcmodel=small on MinGW (no .refptr loads)" ON)
    if(KZ3_SPU_CMODEL_SMALL AND MINGW)
        target_compile_options(kz3_spu PRIVATE -mcmodel=small)
    endif()
    target_compile_options(kz3_spu PRIVATE ${KZ3_SPU_OPT} ${KZ3_SPU_ISA_ALL} -w -ffp-contract=off -fno-strict-aliasing)
    set_source_files_properties(kz3_cpu_check.c PROPERTIES COMPILE_OPTIONS "-mno-avx;-mno-fma")
endif()

# Host replacements for lifted PPU functions (patch 1018, ppu_lifter.py --overrides port/spu/ppu_overrides.txt).
# kz3_ppu_overrides.cpp is empty unless the lift's ppu_recomp.h announces PPU_OVERRIDE_<addr>.
if(DEFINED RECOMP_DIR AND EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    target_sources(kz3_spu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/kz3_ppu_overrides.cpp")
    # Host coroutines for the PPU fiber scheduler overrides (pass 13).
    target_sources(kz3_spu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/guest_fiber.c")
    # port/kz3 for kz3_log.h (pass 32: the HardExit / link-miss hook macros).
    # port/net for kz3_net_hooks.h (pass 43: the region label hook); KZ3_WITH_NET is declared later by port/kz3
    # (option, default ON), so "not defined yet" counts as on, like the option's default.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/kz3_ppu_overrides.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${RECOMP_DIR};${CMAKE_CURRENT_SOURCE_DIR}/../kz3;${CMAKE_CURRENT_SOURCE_DIR}/../net")
    if(NOT DEFINED KZ3_WITH_NET OR KZ3_WITH_NET)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/kz3_ppu_overrides.cpp" APPEND PROPERTY COMPILE_DEFINITIONS KZ3_WITH_NET)
    endif()
endif()

# Stdio redirect for the PPU override sources (SPU pass 115). Their fprintf(stderr) lines ([fiber], [heap],
# [lump-stage], [label], ...) went to the CRT's stderr, which ps3_stdio_init() makes fully buffered (64 KB); the
# runtime's flush thread calls fflush(stderr) from a redirected TU, where it is the logger's no-op kz3_log_fflush.
# MinGW GCC builds still showed them on stdout because C++ fprintf there is __mingw_fprintf (one fputc per character
# into msvcrt); llvm-mingw builds (UCRT fprintf: the release pipeline, players' builds) kept them in the buffer until
# a real fflush (kz3_stall_diag.c) or lost them when the process was killed, and no build put them in the log file.
# Redirected, they go through kz3_log like the runtime's lines (log file, stdout echo, crash ring; [tag] -> level
# table in kz3_log.c). Only these two files: they print only from guest calls, after kz3_log_init. The probes'
# install lines run from the SPU registration constructor before kz3_log_init and would reach only the ring
# (see the RE notes "Pass 115"). Same switch as port/kz3 (KZ3_LOG_REDIRECT, declared there after this file;
# "not defined yet" counts as its default ON).
set(KZ3_SPU_LOG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../kz3/kz3_log.h")
if(DEFINED RECOMP_DIR AND EXISTS "${RECOMP_DIR}/ppu_recomp.h" AND (NOT DEFINED KZ3_LOG_REDIRECT OR KZ3_LOG_REDIRECT)
   AND EXISTS "${KZ3_SPU_LOG_HEADER}")
    set(KZ3_SPU_LOG_REDIRECTED "${CMAKE_CURRENT_SOURCE_DIR}/kz3_ppu_overrides.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/guest_fiber.c")
    set_property(SOURCE ${KZ3_SPU_LOG_REDIRECTED} APPEND PROPERTY COMPILE_DEFINITIONS KZ3_LOG_REDIRECT_STDIO)
    set_property(SOURCE ${KZ3_SPU_LOG_REDIRECTED} APPEND PROPERTY COMPILE_OPTIONS -include "${KZ3_SPU_LOG_HEADER}")
endif()
