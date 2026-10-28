# How to port a PS3 game with ps3recomp (outline)

Status: outline. Each section lists what it will cover; the text is written from this port's experience as the
sections are filled in. It is meant for people and for coding agents: every step names its inputs, its outputs and
how to tell that it worked.

This guide assumes you own the game and dump it yourself. Nothing from a game (code, data, keys, a built exe) belongs
in a public repository; a port ships only its own code and recovers everything else on the player's machine.

## 1. Is the game a good candidate?

- What static recompilation can and cannot do: the code is translated once, ahead of time; self-modifying code,
  JIT-style loaders and hand-written stack switching need host replacements.
- Quick checks on a dump: the executable's size and function count, the imported system libraries (how many of them
  [ps3recomp](https://github.com/sp00nznet/ps3recomp) already implements), the number of embedded SPU programs and
  SPURS job modules, the data archive format.
- Running the game in [RPCS3](https://rpcs3.net) first: its logs and frame captures are the reference for what the
  real game does. Read them; do not copy its code (GPL-2) into an MIT port.
- Rough cost: what took longest here (SPU jobs, the GPU command stream, file system details) and why.

## 2. Setting up

- A pinned, reproducible toolchain (this project uses a Nix flake: Python, a Windows cross compiler, CMake, Ninja).
- ps3recomp as a pinned submodule; your fixes as numbered patch files applied to a fresh copy before each build, so
  the upstream tree stays clean and each fix can be offered upstream on its own.
- A sandbox for anything that reads game data or runs the game: game files read-only, no network, writes only to a
  scratch folder.
- Where game data lives during development (outside the repository) and how the build finds it.

## 3. Inputs: the dump, the update, the executable

- Disc or digital dump layout (`PS3_GAME/PARAM.SFO`, `USRDIR/EBOOT.BIN`, data archives), and game updates.
- The executable is an encrypted SELF; the port works from a decrypted copy that the player makes from their own game
  (the project doesn't provide decryption tools or instructions; the PS3 emulator RPCS3 and its documentation are the usual place people start). Background: the [PS3 Developer Wiki](https://www.psdevwiki.com/ps3/) pages on SELF and PARAM.SFO.
- Recording facts (sizes, hashes, title id, version) so the build can check that a player's dump matches.

## 4. Lifting the PPU code

- Loading the ELF, finding functions, the TOC and the import stubs (ps3recomp's loader and function finder).
- Lifting to C: the register model, big-endian memory, how calls and indirect calls are resolved.
- Splitting the output into compile units by address so a fix recompiles one unit, not the whole game.
- Overrides: replacing a guest function by host code (context switches, hot loops, broken spots), keeping the
  original address in a comment.
- Checking the lift: coverage reports, functions reached only through pointers, code ranges cut short.
- References: the PowerPC and Cell PPU ABI documents published by IBM and Sony (Cell Broadband Engine
  documentation).

## 5. Lifting the SPU code

- Finding SPU images: embedded ELFs, SPURS job modules, overlays; lifting every executable segment.
- Registering images by a fingerprint of their code so uploads at run time find the matching compiled C.
- SPURS, tasksets and job chains on host threads; DMA, mailboxes and atomic reservations.
- References: the SPU ISA, SPU ABI and SPU language extension documents in IBM's Cell documentation.

## 6. The runtime: system calls and libraries

- What ps3recomp provides (lv2 system calls, cellFs, cellGcm, cellSpurs, cellSaveData, cellPad, cellAudio and more)
  and how a missing function shows up (unresolved import, null call, wrong argument count).
- Game-specific glue in its own folder, apart from the runtime: archive file system, update overlay, the game's own
  quirks.
- Finding a function's arguments and behaviour: the import's NID, the game's calls to it, RPCS3's logs, the PS3
  Developer Wiki.
- Fixing the runtime in general (an upstream candidate) versus working around one game (glue).

## 7. Graphics

- Walking the RSX command stream as the game writes it, and replaying it on Direct3D 12.
- Translating vertex and fragment programs; textures, render targets and the formats that need care (sRGB, depth,
  swizzled layouts).
- Write-back of GPU results the SPUs read from memory (post-processing chains).
- Shader caches: why a first session stutters and how a warm cache is made on the player's machine.

## 8. Testing without a console

- Headless runs: a hidden display, muted audio, scripted pad input, a time limit, a screenshot.
- Metrics from the log (frames presented, files opened, last frame time) compared with the last good run.
- Small focused tests for each runtime fix, run against the unpatched and the patched runtime.
- Recording run provenance (exe hash, patch set, environment) so two runs are only compared when comparable.
- Running a Windows build under Proton on Linux (vkd3d-proton maps Direct3D 12 to Vulkan).

## 9. Common traps (from this port)

- Thread-local storage under MinGW (`__declspec(thread)` ignored; emulated TLS cost).
- Fixed-size runtime tables (SPU contexts, IO mappings) that one game outgrows.
- Memory a game maps where the runtime placed its own windows.
- Races in emulated atomics; call-depth guards that leak on fibers; first-boot sequences that cannot be skipped.
- Build memory: lifted units are large; cap parallel compile jobs.

## 10. Shipping a port legally and safely

- The player's dump is read in place; the port lifts and compiles on the player's machine with a bundled toolchain.
- An installer that checks the dump and update, shows one progress bar and keeps a log; nothing goes online unless
  the player turns it on.
- Naming the game descriptively only; crediting tools, fonts and references with their licences.

## 11. Working with coding agents

- Split the work by area (SPU, graphics, data/reverse engineering, release), one worktree and one set of owned paths
  each, least-privilege permissions, and a sandbox for game data.
- Facts first: a short facts file per area, decoded facts in notes, every claim backed by a run that can be repeated.
- Shared resources (GPU, build memory) behind locks; long runs in the background.

## Sources

- ps3recomp: <https://github.com/sp00nznet/ps3recomp> (MIT)
- RPCS3: <https://rpcs3.net> (GPL-2; used as a behavioural reference only)
- PS3 Developer Wiki: <https://www.psdevwiki.com/ps3/>
- IBM Cell Broadband Engine documentation (CBE architecture, PPU and SPU ABI, SPU ISA, SPU language extensions)
- rizin and rz-ghidra (per-function decompilation): <https://rizin.re>
- Proton and vkd3d-proton: <https://github.com/ValveSoftware/Proton>, <https://github.com/HansKristian-Work/vkd3d-proton>
