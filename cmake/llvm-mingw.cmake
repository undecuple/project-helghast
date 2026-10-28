# Build the Windows x64 exe with llvm-mingw (clang + lld + libc++/libunwind, MinGW-w64 UCRT): the release's
# bundled compiler and the llvm-mingw trial. Mirrors cmake/mingw-w64.cmake (the
# verified GCC 16.2 cross build). Works with a Windows-hosted llvm-mingw (the payload's toolchain\llvm-mingw,
# firstrun.py passes -DLLVM_MINGW_ROOT) and with a Linux-hosted llvm-mingw release (cross, same flags).
#
#   cmake -S port/kz3 -B build/x -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DLLVM_MINGW_ROOT=DIR
#
# Differences from mingw-w64.cmake (pass 8 survey of port/kz3, port/spu and the toolkit's CMakeLists):
#   - compilers are the target-prefixed clang wrappers; lld is their default linker; the archiver is llvm-ar.
#   - "-static -static-libgcc -static-libstdc++" becomes "-static": there is no libgcc/libstdc++ here, -static
#     alone links libc++, libunwind, compiler-rt and winpthreads statically (no DLLs next to kz3.exe).
#     ASSUMPTION until the trial: the clang mingw driver needs nothing else for a fully static exe.
#   - windres is llvm-windres behind the x86_64-w64-mingw32-windres wrapper (app.rc: icon, version, manifest).
#   - TLS: GCC on mingw uses emulated TLS (__emutls); clang on mingw uses native TLS. Same source (__thread,
#     thread_local), different code; the trial's boot check covers it.
# Every compile flag the build adds (-Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffp-contract=off
# -msse4.1 -O2 -w -include FILE, C17, C++20, __attribute__((musttail)) in C, __builtin_ia32_pause, __int128,
# __atomic_*) is accepted by clang; none is GCC-only. clang is stricter about musttail (caller and callee
# prototypes must match exactly): spu_dispatch_mt.c and the lifted SPU code are the first place to look if the
# trial fails to compile.

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_SYSTEM_NAME Windows)      # cross from Linux; on a Windows host the host system is already right
endif()
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "llvm-mingw folder (holds bin/); empty = the compilers on PATH")
# try_compile projects re-read this file: pass the root on to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)

set(_kz3_tc_prefix "x86_64-w64-mingw32-")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(_kz3_tc_exe ".exe")
else()
    set(_kz3_tc_exe "")
endif()
if(LLVM_MINGW_ROOT)
    set(_kz3_tc_bin "${LLVM_MINGW_ROOT}/bin/")
else()
    set(_kz3_tc_bin "")
endif()

set(CMAKE_C_COMPILER   "${_kz3_tc_bin}${_kz3_tc_prefix}clang${_kz3_tc_exe}")
set(CMAKE_CXX_COMPILER "${_kz3_tc_bin}${_kz3_tc_prefix}clang++${_kz3_tc_exe}")
set(CMAKE_RC_COMPILER  "${_kz3_tc_bin}${_kz3_tc_prefix}windres${_kz3_tc_exe}")
set(CMAKE_AR           "${_kz3_tc_bin}llvm-ar${_kz3_tc_exe}" CACHE FILEPATH "archiver")
set(CMAKE_RANLIB       "${_kz3_tc_bin}llvm-ranlib${_kz3_tc_exe}" CACHE FILEPATH "ranlib")

if(LLVM_MINGW_ROOT)
    set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Windows 10 API level, as the nixpkgs mingw-w64 the GCC build uses defaults to: llvm-mingw's _mingw.h defaults
# _WIN32_WINNT to 0x601 (Windows 7), which hides GetCurrentThreadStackLimits (0x602+) used by
# runtime/spu/spu_workload.c (stage 0, 2026-10-12: "call to undeclared function"). The player needs Windows 10 anyway.
set(CMAKE_C_FLAGS_INIT   "-D_WIN32_WINNT=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00")

# Ship a self-contained exe: libc++, libunwind and winpthreads linked in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
