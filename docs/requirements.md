# Requirements

What you need to play and to build the PC port of Killzone 3. Kept current as the port changes.
The port plays the campaign by hand but not yet at full speed (one chapter hand-played to its end, 2026-10-20), so the hardware figures below are provisional and marked as such;
measured values replace them as they come in.

## What you need from your own copy of the game

| Item | Required | Details |
|---|---|---|
| **Killzone 3 disc, US release** | yes | Title ID **BCUS98234** (disc VERSION 01.01, needs PS3 system 3.55). Other regions/IDs are untested. You make your own dump (decrypted folder, as made by a PS3 or a disc dumper); the port ships none of the game. Dump size: 41.4 GiB. |
| **Game update 1.14** | yes | The 1.14 update from your own console. The port is built on 1.14, the game's last version; it adds the option to turn off the mandatory SIXAXIS sections. |
| **Decrypted 1.14 EBOOT** | yes | The game's `EBOOT.BIN` is encrypted; the port needs `EBOOT.elf`, its decrypted form, made from your own copy of the 1.14 update (this project gives no decryption tools or instructions). No firmware is needed for this step. |
| **PS3 system firmware** | not currently (optional fonts) | Only for Chinese, Korean and Japanese text: an optional PS3 font folder from your own firmware files; Latin and Cyrillic use an open font. The port answers the PS3 system libraries itself (high-level emulation) and loads no firmware modules. If that changes (e.g. running Sony's SPURS module), the firmware file on the game disc (`PS3_UPDATE/PS3UPDAT.PUP`, 3.55) is enough. |
| **PlayStation Network account** | no | Single player needs no account. Online play runs on servers players host; no PSN account. |
| **DLC map packs** | optional (not supported yet) | Killzone 3's paid multiplayer map packs: **Retribution**, **Decimation** and **From the Ashes**. Update 1.14 looks for their levels (multiplayer levels 9 to 16) at startup and runs normally without them. Support is planned: the port would read your own installed packs the way it reads the update. Which pack holds which level is not yet checked. |
| **PlayStation Move, 3D TV** | no | Move + Sharpshooter is on the roadmap (a Move controller over USB/Bluetooth; a PS Eye or another camera for tracking). 3D output (side by side or top and bottom, from the game's own 3D mode) is on the roadmap too. |

## Hardware to play (provisional)

The port is a native Windows x64 program using Direct3D 12. Developed on Linux and tested mostly through Proton; first runs on native Windows since 2026-10-21.

| | Minimum (provisional) | Recommended (provisional) | Basis |
|---|---|---|---|
| **OS** | Windows 10 64-bit, or Linux with Proton | Windows 11, or a current Proton (Experimental / GE) | Tested mostly under Proton. On native Windows 11 the installer built the port and the game ran to the menus (2026-10-21); on a stock Windows 10 with no DirectX redistributable the exe starts and loads the menus (2026-10-22, on a test machine without a GPU, so with Windows' software renderer). A native Linux build is planned; Linux players would then need no Proton |
| **CPU** | 6 cores / 12 threads (SSE4.1 at minimum; the first-run build uses AVX + FMA3 when the CPU has them (Intel Haswell or AMD Piledriver and newer, faster SPU code, patch 1097) and builds without them otherwise) | 8+ cores / 16 threads | The PS3's CPU and six SPUs become host threads (main game thread, SPU job workers, graphics thread); per-thread speed matters most. Measured 2026-10-14 on an i7-12700KF (12 cores / 20 threads) with an RTX 4060: a busy level ran at about 11 frames/s then (about 19 on 2026-10-22), limited by the CPU side (the game's main thread and the five SPU worker threads), not the GPU (12-14 ms per frame at full clocks). Final figures once the level runs at speed. |
| **RAM** | 8 GB | 16 GB | The running game uses about 3.4 GB today (4 GB guest memory window plus the runtime). |
| **GPU** | Direct3D 12 (feature level 12_0), 4 GB VRAM | NVIDIA RTX 2060 or newer, 8 GB VRAM for 1440p/4K | Developed on an RTX 4060 (NVIDIA is the target). AMD and Intel GPUs are untested. Higher internal resolutions are planned (ROADMAP.md). |
| **Disk** | 45 GB | 60 GB on an SSD | 41.4 GiB game dump + 0.1 GiB update + the port (~0.8 GB exe). Level streaming reads the game archive. |
| **First-run build** (installer) | 8 GB RAM, about 2 GB disk for the install and the build (measured 1.7 GB: 0.55 GB installed files + 1.1 GB build folder), plus about 1.5 GB of game data unpacked while playing (measured 1.4 GB after four sessions), roughly 5-30 min depending on cores | 16 GB RAM, 8+ cores (about 6.5 min to build at 16 jobs, measured on a 20-core PC under Proton, 2026-10-12; 7 min for the whole installer run on a 16-core PC under native Windows 11, 2026-10-21; an optional shader warm-up adds about 17 min and is off by default because it does not yet carry over to the next session) | The installer lifts and compiles the game's code from your own EBOOT once with the bundled clang: about 0.75 GB per compile job and 2.3 GB for the lift, so 4 jobs fit in under 4 GB; jobs follow free memory and cores. Measured under Proton on an i7-12700KF (8 min at 4 jobs, 5 min at 16); slower CPUs and the 1.14 EBOOT (~8 % more code) are estimates. On PCs with 8 GB or less the installer picks a low-memory build (one compile job, chosen automatically): measured 2026-10-15 under Proton inside an 8 GB memory cap with no swap, it finished with a 2.8 GB peak in 49 min on 20 fast cores (the default 4-job build: about 3.6 GB of process memory, 10.6 min); slower PCs take longer. |
| **Controller** | keyboard and mouse | a gamepad (XInput; DualShock/DualSense through Steam Input) | Keyboard and mouse work in the menus and in levels (mouse look; sensitivity under Options > Mouse), except in a few dialogs. Button prompts follow the device in use: PS3, PS4/PS5, Xbox or keyboard keys (Options > Controller > Button prompts). Split-screen co-op needs a second controller for the second player; keyboard and mouse as one of the two players is planned. |

Current measured performance (2026-10-27, RTX 4060, under Proton): the menus run at the game's own 30 fps; levels
run at about 16-22 frames/s depending on the chapter (a busy Frozen Shores window about 22-23), so these rows are still provisional. They will be replaced by
measured numbers once the level runs at speed.

## Hardware to build the port yourself (if the release is source-only)

The port's executable is generated from the game's own code, so a public release may have to be "bring your own disc,
build it yourself" (see the end). Building needs more than playing:

| | Minimum | Recommended | Basis |
|---|---|---|---|
| **OS** | Linux with Nix (the toolchain is a Nix flake; NixOS is the developer's system) | same | The installer and portable zip build the port on Windows with a bundled compiler (native Windows 11, 2026-10-21); the Nix flake steps here are the Linux developer build |
| **RAM** | 16 GB (builds with 1-2 compiler jobs) | 32 GB | Each generated code chunk peaks at 2.4-2.8 GB while compiling; the build keeps 8 GB free and runs at most 4 jobs |
| **CPU / time** | any x86-64 | 8+ cores | Measured first-run build: about 6.5 min at 16 jobs (installer under Proton, 2026-10-12); incremental builds after a change 5-7 minutes at 4 jobs; the installer's whole run took 422 s (7 min) on a 16-core PC under native Windows 11 (2026-10-21) |
| **Disk** | 60 GB free beyond the game dump | 100 GB, SSD | Generated code 0.3 GB, build tree 1.7 GB, plus analysis data (optional extracted assets add ~31 GB) |

## Distribution and online play
- **Distribution model** (decided): sources, plus an installer that builds on the player's machine: the Inno
  Setup setup .exe (checks the disc dump, update and decrypted EBOOT) and a portable zip for Proton/Steam Deck
  ship our runtime, lifter and compiler but no game code, and lift and compile from the player's own EBOOT on first
  run. A pre-built exe would contain the game's code in translated form, so it stays out unless decided otherwise after
  legal advice. Licences (ps3recomp MIT; no RPCS3 code) allow either. The first-run build's time and RAM are in the
  "First-run build" row above.
- **Online play**: the game's own network code against a self-hosted Medius-compatible server that players or
  the community run; a self-hosted Nakama server (with instructions) is the alternative. The port does not connect
  to PS Rewired, the community service that keeps the original PS3 game online. Multiplayer is in progress.
- **Other regions** (European and other releases): untested; their EBOOTs differ.
