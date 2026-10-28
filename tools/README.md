# tools/

Index of the scripts and folders in this directory. Build and test details are in [docs/BUILDING.md](../docs/BUILDING.md).

## Entry points

| Script | What it does |
|---|---|
| `build-kz3.sh` | Configures and builds the Windows exe (`kz3.exe`) with the MinGW-w64 cross toolchain. |
| `lift.sh` | Lifts the PPU code of the player's own EBOOT to C (static recompilation). |
| `kz3-stage.sh` | Stages a runnable folder: the built exe next to the player's own game files, read in place. |
| `sandbox.sh` | Runs a command with the game and the repository read-only; only `work/` and `build/` are writable. |
| `proton-test.sh` | Runs a Windows program under Proton on Linux, hidden and muted (used for tests on Linux). |
| `lint.sh` | Lints the files a commit touches: ruff, shellcheck and clang-format. |
| `run-host-tests.sh` | Runs every host test listed in `host-tests.txt`; no game data and no GPU needed. |
| `host-tests.txt` | The list of host tests CI runs. |
| `ps3recomp-patched.sh` | Applies the project's patches to the pinned ps3recomp checkout. |
| `safe_rm.sh` | Deletes files inside the repository only. |

## Folders

| Folder | Contents |
|---|---|
| `spu/` | SPU lift (`lift_spu.sh`, `lift_spu.py`), version tables and the SPU host tests. |
| `smoke/` | Host-side test harness: small C programs and Python checks that run without a GPU or game data. |
| `release/` | Installer, portable zip and first-run build: payload assembly, toolchain fetch, disc and update checks, launcher. |
| `upstream/` | Host tests for the general ps3recomp fixes the patches carry. |
| `pad/` | Scripted pad-input files (`.pad`) that drive the menus and levels in automated runs. |
| `assets/` | `profile_edit.py` (reads and edits the profile save) and `lift_table.sh` (checks the lift is current). |
| `agents/` | Optional build and GPU slot locking (`lock.sh`, `slotlock.sh`): sourced by the build and lift scripts so two jobs do not compete for the machine. Not needed for a single-user build. |

Single-file helpers: `disc_info.py` (disc layout), `psarc.py` (archive list and extract), `elf_info.py` and `jobmods.py` (EBOOT inspection), `features.py` (generates `docs/FEATURES.md`).

Comments cite internal numbering (pass, round, plan items) from the development notes; they are labels, not links.
