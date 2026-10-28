# Glossary

Terms used in this repository's code and docs.

| Term | Meaning |
|---|---|
| **PPU** | The PS3's PowerPC core, which runs the game's main code (the EBOOT). |
| **SPU** | The Cell's six co-processors available to games, each with 256 KB of local store; they run jobs such as physics, animation, audio and post-processing. |
| **SPURS** | Sony's SPU task and job scheduler library. Killzone 3 runs its job modules (WWS jobs) and Havok tasks through it. |
| **RSX** | The PS3's GPU. The game drives it through command buffers built with the cellGcm library. |
| **EBOOT** | The game's main executable (`EBOOT.BIN`, encrypted on the disc; `EBOOT.elf` once decrypted). |
| **Lift** | Translating machine code into C source that the PC compiler builds (static recompilation). |
| **HLE** | High-level emulation: a PS3 library or system call answered by host code instead of running Sony's own module. |
| **Override** | A lifted PPU function replaced by host code, listed per game version in `re/data/overrides/` (v1.00: `port/spu/ppu_overrides.txt`). |
| **Live-draw** | The Direct3D 12 renderer that replays the game's RSX commands as they are written. |
| **PSARC** | The game's data archive format (`data_ps3.psarc`). |
| **.pspatch** | A binary patch from update 1.14, applied to a disc archive entry. |
| **Patch numbers** | Files in `port/patches/`, applied to ps3recomp in order: 0xxx general and lifter, 1xxx SPU and SPURS, 2xxx graphics, 3xxx lifter and system libraries, 4xxx online play, network, controllers and file system, 5xxx data, update and trophies, 9xxx port changes layered on a ps3recomp pull request that is not merged yet. |
| **Host test** | A test that runs on the build machine without the game and without a GPU (`tools/host-tests.txt`). |
| **Checkpoint** | A dated screenshot of something new on screen, kept with the development records; a chosen set is in [SCREENSHOTS.md](SCREENSHOTS.md). |
| **v1.00 / 1.14** | The disc version of the game and its last update. The port is built on 1.14 since 2026-10-22; v1.00 can still be built for comparison (`KZ3_EBOOT_VER=100`). |
| **Proton** | Valve's compatibility layer that runs Windows programs on Linux (part of Steam). The port's Windows build is tested under it. |
| **Bink** | The video format of the game's movies and cutscenes, played by the game's own Bink player. |
| **Medius** | The online service protocol the game's multiplayer client speaks. The port connects it to a server that players host themselves. |
