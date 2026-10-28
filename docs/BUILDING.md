# Building the port

The port's executable is generated from **your own copy** of the game: the game's code is lifted to C from your
decrypted EBOOT and compiled on your machine. Nothing of the game is in this repository, and nothing is downloaded.

There are two ways to get a build:

- **The installer or portable zip (Windows, or Linux with Proton), v0.1.1 pre-release.** It checks your disc dump and
  the 1.14 update, then lifts and compiles the game's code on your machine with a bundled compiler, behind one
  progress bar, with a full install log. Download it from the
  [releases page](https://github.com/undecuple/project-helghast/releases).
- **A developer build from source with Nix**, described below: on Linux (how the port is developed and tested
  today), on a Mac with Apple silicon, or on Windows inside WSL2. The build is the same `nix develop` steps on all
  three; only setup and running differ (see [Building on macOS](#building-on-macos) and
  [Building on Windows](#building-on-windows)).

## What you need

From your own copy of the game (details and sizes in [requirements.md](requirements.md)):

| Item | Notes |
|---|---|
| Killzone 3, US disc **BCUS98234**, dumped as a decrypted folder | `PS3_GAME/USRDIR/data_ps3.psarc` and the rest of the disc. Other regions are untested. |
| Update **1.14** | Your own copy of the update; the port's target version. The PS3 emulator RPCS3 and its documentation are the usual place to learn how a PS3 update is installed. |
| The **decrypted EBOOT** | `EBOOT.elf`, the decrypted form of the update's `EBOOT.BIN`, comes from your own copy of the game (this project provides no tools or instructions for it; the PS3 emulator RPCS3 and its documentation are the usual place people start). No PS3 firmware is needed. |

To build: Linux with [Nix](https://nixos.org/) (flakes enabled), 16 GB RAM or more (32 GB recommended), about 60 GB
free disk beyond the game dump. To run: an NVIDIA GPU with Direct3D 12 is the tested target; on Linux the Windows
build runs under Proton (Experimental or GE-Proton).

**Version note:** the port builds from the 1.14 update's EBOOT (the default since 2026-10-22). The disc's v1.00 EBOOT
can still be built for comparison: set `KZ3_EBOOT_VER=100` on every step, use `work/bin/EBOOT.elf` and the v1.00
paths noted in step 3 (no update needed).

## Toolchain

Everything comes from the Nix flake; nothing needs to be installed system-wide.

```sh
nix develop .#re     # analysis and lifting: Python with capstone/pyelftools, bubblewrap
nix develop .#win    # Windows x64 cross build: MinGW-w64, CMake, Ninja, SDL2 and SDL3 for Windows
nix develop          # everything, plus Xvfb and steam-run for test runs under Proton
nix develop .#ci     # what CI uses: lint and host tests
```

`tools/sandbox.sh [--gpu] -- CMD` runs a command with the game and the repository read-only, only `work/` and
`build/` writable, and no network. The steps below run inside it.

## Steps

```sh
git clone --recursive https://github.com/undecuple/project-helghast
cd project-helghast
git submodule update --init                          # third_party/ps3recomp, pinned

# 1. Point the repo at your game files (both stay outside git and are mounted read-only)
ln -s /path/to/your/disc-dump Game                   # the folder that holds PS3_GAME/
ln -s /path/to/rpcs3/dev_hdd0/game/BCUS98234 GameUpdate
mkdir -p work/bin && cp /path/to/decrypted/EBOOT.elf work/bin/EBOOT-114.elf   # the 1.14 update's EBOOT, decrypted

# 2. Patched recompiler: a fresh copy of the pinned ps3recomp with port/patches/*.patch applied
nix develop .#re --command tools/sandbox.sh -- tools/ps3recomp-patched.sh

# 3. Find the SPU programs and job modules in the EBOOT (v1.00: work/bin/EBOOT.elf, work/analysis/, --spu-dir work/bin/spu)
nix develop .#re --command tools/sandbox.sh -- python3 tools/jobmods.py work/bin/EBOOT-114.elf \
    --out work/analysis-114/jobmods --tsv work/analysis-114/jobmods.tsv
nix develop .#re --command tools/sandbox.sh -- python3 tools/elf_info.py work/bin/EBOOT-114.elf \
    --spu-dir work/analysis-114/spu_images

# 4. Lift the PPU code (work/recomp/) and the SPU code (work/spu/)
nix develop .#re --command tools/sandbox.sh -- tools/lift.sh
nix develop .#re --command tools/sandbox.sh -- tools/spu/lift_spu.sh

# 5. Configure and build kz3.exe (build-kz3.sh caps the compiler jobs by free memory)
nix develop .#win --command tools/sandbox.sh -- bash -c \
  'cmake -S port/kz3 -B build/kz3-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/mingw-w64.cmake" \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE="$(command -v python3)"'
nix develop .#win --command tools/sandbox.sh -- tools/build-kz3.sh build/kz3-win

# 6. Stage and run under Proton (hidden Xvfb display, audio drivers off; output in work/proton/NAME/). The stage
#    holds the EBOOT as PS3_GAME/USRDIR/EBOOT.elf and the update where a PS3 keeps it (dev_hdd0/game/BCUS98234).
tools/kz3-stage.sh
nix develop --command tools/proton-test.sh --seconds 60 --name boot work/kz3-stage/kz3.exe ./PS3_GAME/USRDIR/EBOOT.elf
```

The lift and the build are the slow parts: each of the 64 lifted PPU source chunks takes up to about 1.5 GB of
memory to compile. After a change to a patch, rerun step 2; after a change to the override list
(`re/data/overrides/ppu_overrides-114.txt`; `port/spu/ppu_overrides.txt` for v1.00), rerun the PPU lift (the build
refuses a lift that does not match the override list).

`tools/proton-test.sh` uses GE-Proton from Steam's `compatibilitytools.d` by default (`--proton DIR` picks
another). The program's output is in `work/proton/NAME/app/app_stdout.txt` and a screenshot in `work/shots/NAME.png`.
Environment switches for logging and diagnostics are documented in the source where they are read (`getenv`).

## Building on macOS

**Status: the macOS shells evaluate, but no build has been run on a Mac yet.** Reports are welcome (open an issue).

- **What you need:** a Mac with Apple silicon (M1 or newer; nixpkgs no longer supports Intel Macs), macOS 13 or
  newer, 16 GB of memory or more, and [Nix](https://nixos.org/download/) with flakes enabled
  (`experimental-features = nix-command flakes` in `~/.config/nix/nix.conf`). Xcode is not needed.
- **Game files:** the same files as above (disc dump, 1.14 update, decrypted EBOOT); RPCS3 has a macOS build for installing the update.
- **Steps:** the same as [Steps](#steps), 1 to 5, with `nix develop .#re` and `nix develop .#win` as written. On
  macOS `tools/sandbox.sh` runs each command directly (bubblewrap is Linux-only) and says so. The Windows
  cross-compiler is built by Nix the first time, which can take a while if the binary cache has no macOS copy.
- **Running:** `kz3.exe` is a Windows program and Proton does not run on macOS. The tested way is to copy
  `work/kz3-stage/` to a Windows PC with an NVIDIA GPU and run it there (copy with links resolved, as in step 5 of
  [Building on Windows](#building-on-windows), which includes the game data).
- **Trying it on the Mac itself (untested):** Apple's [Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/)
  and the tools built on its D3DMetal layer (CodeWeavers' CrossOver, commercial; [Mythic](https://github.com/MythicApp/Mythic),
  free, macOS 14 or newer) run Windows x86-64 programs on Apple silicon with
  Direct3D 12 translated to Metal and the CPU code translated by Rosetta 2. `kz3.exe` may start this way, but expect it
  to be much slower than on a Windows PC: the port is not yet at full speed on native x86-64 hardware, and both
  translation layers cost more. The first-run CPU check turns off FMA if Rosetta does not report it. Nobody has
  tried this yet. If you do, please open an issue with your Mac model, macOS version, the tool and its version,
  and how far the game got (the log in the game's folder helps).
- **Native macOS build:** on the wanted list (a native Apple-silicon build, no translation layers); not started.

## Building on Windows

Nix does not run on Windows itself, so the build runs inside **WSL2** (a Linux system that Windows runs), and the
result runs on Windows directly. **Status: the WSL2 build follows the Linux steps unchanged but has not been run
on WSL2 yet.** The port itself has run on native Windows 11 (built by the installer's first run, 2026-10-21); it
is tested mostly under Proton on Linux.

1. **Install WSL2 with Ubuntu** in an administrator terminal: `wsl --install -d Ubuntu`, then restart. In the
   Ubuntu terminal, install [Nix](https://nixos.org/download/) (multi-user install) and enable flakes as above.
2. **Give WSL2 enough memory:** create `%UserProfile%\.wslconfig` with `[wsl2]` and `memory=24GB` (at least 16 GB),
   then `wsl --shutdown`.
3. **Keep the repository inside the Linux file system** (for example `~/project-helghast`, not under `/mnt/c`):
   the build reads and writes many files and is much slower on the Windows drive. The game dump may stay on a
   Windows drive; link it as in step 1 (`ln -s /mnt/d/KZ3 Game`).
4. **Build:** the [Steps](#steps) 1 to 5, unchanged (bubblewrap works inside WSL2). Skip step 6 (Proton).
5. **Run on Windows:** `tools/kz3-stage.sh`, then copy `work/kz3-stage/` to a Windows folder, for example
   `cp -rL work/kz3-stage /mnt/c/Games/kz3`. `-L` turns the staged links into real files, so this copies the
   game's data too (about 32 GB): the port reads the game from that folder, laid out as on the disc. Then, in a
   Windows terminal in that folder: `kz3.exe ./PS3_GAME/USRDIR/EBOOT.elf`. Since the folder holds `PS3_GAME`,
   double-clicking `kz3.exe` does the same (see [Running](#running)). You need an NVIDIA GPU with Direct3D 12
   and a current driver.

The installer does all of this on Windows without WSL2 or Nix (a bundled compiler; see the releases page).

## Running

`kz3.exe` finds the game by itself, so a double-click or a Steam shortcut needs no launch options. In order:

1. a path given on the command line (`kz3.exe path\to\EBOOT.elf`; the test scripts use this);
2. `eboot = ...` in `kz3.ini` next to `kz3.exe`;
3. `PS3_GAME\USRDIR\EBOOT.elf` next to `kz3.exe` (the staged folder of step 6, or a dump folder with the decrypted
   EBOOT in it and `kz3.exe` dropped in).

If none of them has the EBOOT, a notice in the game's style says so and names the log file. `kz3.ini` is plain
text; the installer writes it, and you can write it by hand. A relative path is relative to the folder of `kz3.exe`;
an environment variable that is already set wins over its key.

```ini
[game]
; the decrypted EBOOT
eboot = D:\Games\BCUS98234\USRDIR\EBOOT.elf
; the folder holding PS3_GAME (PS3_VFS_ROOT); empty = found from the EBOOT: the folder above PS3_GAME\USRDIR,
; else the nearest folder above the EBOOT that holds PS3_GAME, else the working folder
vfs_root = D:\Games\KZ3
; the 1.14 update's USRDIR (KZ3_UPDATE_DIR)
update_dir =
; your PS3 firmware's dev_flash\data\font folder (KZ3_FONT_DIR), optional
font_dir =
; the game's language (KZ3_LANG: e.g. fr, de, ja or ru-RU), optional; empty = the system's
lang =
; the working folder, where saves go; empty = the current folder
data_dir =

[display]
; WxH or 720p/900p/1080p/1440p/2160p/4k (KZ3_WINDOW_SIZE); empty = 1280x720
window_size =
```

The installer also fills `hdd0_root`, `edat_cache` and `shader_cache` (`PS3_HDD0_ROOT`, `PS3_EDAT_CACHE`,
`RSX_SHADER_CACHE_DIR`). Lines starting with `;` or `#` are comments; a `;` after a value is part of the value.
An empty value counts as unset.

Display settings live in their own file, `display.ini`, beside the key bindings (`%APPDATA%\Project Helghast\` on
Windows; under Proton the same folder inside the prefix, `C:\users\steamuser\AppData\Roaming\Project Helghast\`).
Alt+Enter writes it, and you can edit it while the game is closed:

```ini
[display]
; windowed (default), borderless or fullscreen (exclusive); Alt+Enter switches between windowed and the last
; fullscreen mode
mode = windowed
; stretch (default): the picture fills the window; fit: the picture keeps its shape, with bars
scaling = stretch
; bilinear (default) or nearest
filter = bilinear
; on (default) or off: off shows each frame as soon as it is ready, which can tear (Options > Display > VSync)
vsync = on
```

`KZ3_DISPLAY=windowed|borderless|fullscreen` sets the mode for one run without saving it. `KZ3_ASPECT=16:9` (or
`off`) keeps the PS3's 16:9 view in gameplay instead of following the window's shape.

## Tests without the game

```sh
nix develop .#ci --command bash -c 'tools/ps3recomp-patched.sh && tools/run-host-tests.sh'
```

These are the checks CI runs on every push: shell lint, every patch applies to the pinned ps3recomp, and the host
tests in `tools/host-tests.txt` (runtime fixes, the SPU tools, the release tools). They need no game data and no GPU.
Graphics tests that need Direct3D 12 are built by `tools/smoke/build-host.sh` and run under Proton with
`tools/proton-test.sh`.

An index of the scripts and folders in `tools/` is in [tools/README.md](../tools/README.md).
