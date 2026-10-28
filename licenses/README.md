# Licence texts

The full licence texts of what the installer and the portable zip ship, copied unchanged. The same files are installed
in the `licenses\` folder next to the game (the installed folder and the zip), under the names given here. The short
credits list is [NOTICE.md](../NOTICE.md). Nothing here covers the game itself, which is never shipped.

- `LICENSE.txt`: the MIT licence of this project's own code, tools and documentation (everything outside `third_party/`).
- `ps3recomp-LICENSE.txt`: MIT, ps3recomp, the recompiler and runtime source that is built on the player's machine (payload folder `ps3recomp\`).
- `at3_standalone-COPYING.LGPLv2.1.txt`: LGPL-2.1, the standalone ATRAC3/ATRAC3plus decoder inside ps3recomp's source (payload folder `ps3recomp\third_party\at3_standalone\`).
- `SDL3-LICENSE.txt`: zlib, SDL3 (`SDL3.dll` in `runtime\`, unmodified; gamepads, rumble and motion sensors).
- `Rajdhani-OFL.txt`: SIL Open Font License 1.1, the Rajdhani fonts (`fonts\`, and embedded in the installer's headings).
- `llvm-mingw/llvm-mingw-LICENSE.txt`: ISC, the llvm-mingw build scripts (bundled compiler, `toolchain\llvm-mingw\`).
- `llvm-mingw/llvm-mingw-tree-LICENSE.TXT`: Apache-2.0 WITH LLVM-exception, as shipped inside the llvm-mingw package (clang, lld, compiler-rt, libc++, libunwind).
- `llvm-mingw/LLVM-LICENSE.txt`: the same LLVM licence from the LLVM project at the pinned version.
- `llvm-mingw/mingw-w64-COPYING.txt`: MinGW-w64 headers and C runtime (ZPL-2.1 and public-domain parts; a few Wine-derived headers are LGPL-2.1+).
- `llvm-mingw/winpthreads-COPYING.txt`: winpthreads (MIT and BSD-3-Clause).
- `llvm-mingw/LGPL-2.1.txt`: the LGPL-2.1 text for those Wine-derived MinGW-w64 headers.
- `cmake/cmake-tree-doc_cmake_LICENSE.rst`: BSD-3-Clause, CMake (`toolchain\cmake\`).
- `ninja/ninja-COPYING.txt`: Apache-2.0, Ninja (`toolchain\ninja\`).
- `python/python-tree-LICENSE.txt`: PSF-2.0, the Python embeddable package (`toolchain\python\`), with the terms of the libraries it bundles.

FFmpeg (LGPL-2.1+) is not shipped: the port only loads it at run time if the player has it, so it has no text here.
`tools/release/test_licenses_match.py` checks that these files match what the payload assembly gathers.
