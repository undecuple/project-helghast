# Instructions for AI coding agents

Read this before changing anything in this repository. It applies to every automated contributor (Claude Code, Codex, Copilot,
Cursor and the like) and to the people who run them. Human contributors: the same rules
are in [CONTRIBUTING.md](.github/CONTRIBUTING.md).

## The project

Project Helghast is an unofficial PC port of Killzone 3 (PS3) made by **static recompilation**: the game's PPU and
SPU code is lifted to C ahead of time with [ps3recomp](https://github.com/sp00nznet/ps3recomp), compiled into a
Windows x64 program, and the PS3 system software it calls is reimplemented; graphics go through Direct3D 12. The
repository holds only the port. Each player lifts and builds the game's code from their own copy of the game.
Overview: [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md). Status: [docs/FEATURES.md](docs/FEATURES.md),
[docs/ROADMAP.md](docs/ROADMAP.md).

## Repository map

| Path | What it holds |
|---|---|
| `port/kz3/` | the program: `main.cpp`, HLE overrides, archive VFS, logging, `kz3.ini` reader |
| `port/spu/` | SPU scheduling, SPURS work, PPU function overrides (`ppu_overrides.txt`) |
| `port/vfs/`, `port/net/` | the 1.14 update overlay; the online client side (opt-in) |
| `port/patches/` | numbered patches to ps3recomp: 0xxx general and lifter, 1xxx SPU and SPURS, 2xxx graphics, 3xxx lifter and system libraries, 4xxx network, 5xxx data, update, trophies, fonts and language |
| `third_party/ps3recomp/` | the recompiler, a pinned submodule (never edited in place) |
| `tools/` | lift, build, stage and run scripts (`lift.sh`, `build-kz3.sh`, `kz3-stage.sh`, `proton-test.sh`, `sandbox.sh`), data tools (`psarc.py`, `elf_info.py`, `jobmods.py`) |
| `tools/spu/`, `tools/smoke/`, `tools/upstream/` | the SPU lifter and host tests; graphics and runtime host tests |
| `tools/release/`, `installer/` | installer (Inno Setup), portable zip, first-run build, launcher, page generators |
| `re/data/`, `re/spu/` | tables the tools read (overrides, SPU maps per game version); numbers and names only |
| `docs/` | public documentation; `docs/features.tsv` is the data behind `docs/FEATURES.md` |
| `cmake/`, `flake.nix` | the MinGW-w64 toolchain file; the Nix shells |

## Rules for everyone

- **No game material, ever.** Never commit, attach, paste, request or link to: files from the disc or the update,
  decrypted binaries (`EBOOT.elf`, SPU images), lifted or decompiled game code (`work/recomp/`, `work/spu/`),
  extracted assets, saves, keys or firmware. Numbers, names, structure layouts and formulas recovered from the game
  are fine. `Game/`, `GameUpdate/`, `work/` and `build/` stay out of git (`.gitignore`); do not weaken it.
- **No circumvention.** The repository ships no decryption or copy-protection circumvention tools, no keys and no
  instructions for decrypting game files. Do not add any, and do not explain how to make the decrypted EBOOT.
- **ps3recomp stays pristine.** Never edit `third_party/ps3recomp/` or move the pin as part of a fix. Every change
  to it is a new numbered patch in `port/patches/`, in its area's number block, applied by
  `tools/ps3recomp-patched.sh` (which fails on a patch that does not apply).
- **No GPL code.** RPCS3 (GPL-2.0) may be read as a reference for behaviour and data layouts; never copy, translate
  or closely paraphrase its code. Write behaviour from your own understanding and say so in the change.
- **Gameplay stays original by default.** Mark features the PS3 game does not have `ENHANCEMENT:`; anything that
  changes gameplay is opt-in. Mark values not yet recovered from the binary `ASSUMPTION:` and never restate one as
  fact. Keep original function addresses in comments (`// @0x00123abc`).
- **Online is opt-in.** Nothing may contact the network unless the player chose it; a new connection must be named
  in the README's "What goes online" list.

## Licence, attribution and citation

- The port's code, tools and docs are MIT ([LICENSE](LICENSE)); ps3recomp is MIT; other components keep the terms
  listed in [NOTICE.md](NOTICE.md). Killzone 3 itself is covered by no licence here.
- A new dependency, bundled tool or font needs a licence compatible with MIT distribution and a row in NOTICE.md in
  the same change. No GPL code linked into the program.
- **Credit what you absorb.** Work taken from an upstream pull request, fork or other source is credited in the
  patch header, in [NOTICE.md](NOTICE.md) (Upstream contributors) and in the commit message, for example
  `adapted from sp00nznet/ps3recomp PR #287 by NoahBPeterson`. Never remove or shorten existing credits.
- **Cite, do not paraphrase silently.** Facts taken from external references (talks, the PS3 Developer Wiki, the
  Cell documentation) name their source next to the fact; public sources are thanked in NOTICE.md (References and
  thanks). Check a fact against the binary before relying on it.
- Citing the project: [CITATION.cff](CITATION.cff). Reuse terms for AI systems: NOTICE.md, "Reuse and attribution".

## Build and test

Wherever you work: **never write into the game dump**, and **keep the game's files out of the repository**. That is
what the maintainers' sandbox enforces; without it, keep to both by hand. CI is the gate for everyone:
`.github/workflows/ci.yml` runs the full list on every pull request, with no game data and no GPU. Build steps:
[docs/BUILDING.md](docs/BUILDING.md).

- **Before every pull request** run what CI runs, if you can (it needs no game data, no GPU):
  `nix develop .#ci --command bash -c 'git ls-files "*.sh" | xargs shellcheck -S warning && tools/ps3recomp-patched.sh && tools/run-host-tests.sh'`.
  Without Nix, open the pull request and let CI run it.
- **Host tests are self-contained.** A runtime fix comes with a focused host test (examples in `tools/upstream/`,
  `tools/smoke/`, `tools/spu/`) that fails on the unpatched code and passes with the patch; list it in
  `tools/host-tests.txt`. A test builds from the patched ps3recomp alone, defines the runtime globals it needs, and
  prints SKIP (not FAIL) when it truly needs game data. When a patch changes what an old test checks, update that
  test in the same change.
- Agents without the game can still work on the tools, patches with host tests, docs and the installer; say in the
  pull request that game runs were not done.

### The maintainers' setup (Linux with Nix; not a requirement)

How the maintainers run things; one way, not a requirement. Everything comes from the Nix flake; nothing is
installed system-wide.

```sh
nix develop .#re     # analysis and lifting
nix develop .#win    # Windows x64 cross build (MinGW-w64, CMake, Ninja)
nix develop          # everything, plus Xvfb and steam-run for Proton runs
nix develop .#ci     # lint and host tests, as CI runs them
```

- **Anything that reads the game** runs inside `tools/sandbox.sh [--gpu] -- CMD` (bubblewrap, Linux only: game and
  repository read-only, only `work/` and `build/` writable, no network). Build the exe with
  `tools/build-kz3.sh build/DIR`: it caps compiler jobs by free memory (a lifted chunk takes up to about 1.5 GB to
  compile); do not run `cmake --build -jN` on the lifted code.
- **Windows runs** on Linux go through `tools/proton-test.sh` (hidden Xvfb display, audio drivers off); output in
  `work/proton/NAME/app/app_stdout.txt`, a screenshot in `work/shots/NAME.png`. Look at the screenshot before you
  claim anything about the picture.

### Without Nix (for example native Windows)

- **A native Windows build goes through the installer** (or the portable zip): it brings the patched toolkit, the
  port's sources and a bundled toolchain (llvm-mingw, CMake, Ninja, an embeddable Python), and `firstrun.py` is the
  route it runs. Players and testers use that; see the README's Windows quick start.
- **A rebuild from a checkout with that toolchain is not a tested route.** `firstrun.py --root <checkout>` looks
  for the patched toolkit in `work/ps3recomp-patched`, which only `tools/ps3recomp-patched.sh` (a shell script,
  run in the maintainers' setup) makes, and the toolkit must come from the checkout's own `port/patches/` (the
  installer's copy matches the release it came with, not your branch). The lift and build scripts (`lift.sh`,
  `tools/spu/lift_spu.sh`, `build-kz3.sh`) expect the Nix shells.
- **Works without the game and without Nix:** the release tools and their tests use Python 3 with the standard
  library only, run one file at a time, e.g. `python3 tools/release/test_firstrun_check.py` (CI runs them in the
  Nix shell; they have not been run on Windows). Docs, `docs/features.tsv` and the installer script
  (`installer/kz3.iss`, needs Inno Setup to compile) can be worked on the same way.
- Keep the dump outside the repository and read-only; do not run anything that writes to it.

## Docs travel with code

- Update the docs a change affects in the same commit: build steps in `docs/BUILDING.md`, requirements in
  `docs/requirements.md`, the pipeline in `docs/HOW-IT-WORKS.md`, credits and dependencies in `NOTICE.md`.
- `docs/FEATURES.md` is generated from `docs/features.tsv` by `tools/features.py`; never edit the page by hand.
  Change `docs/features.tsv` with its evidence (a run, a log line, a screenshot), run `python3 tools/features.py`
  and commit both; `python3 tools/features.py --check` must pass. A status changes only with evidence.
- Never write that something works unless you ran it, and say how. CI enforces lint, the patch series and the host
  tests; the rest is checked in review.
- Environment switches are documented where they are read (`getenv`), with their default.

## Commits and pull requests

- One topic per commit; short, plain, descriptive messages (what changed and why). Credits go in the message as
  above; no tool-generated footers or advertising lines.
- The pull request says what was run (host tests, a Proton run, a game build) and what was seen. Use the templates
  in `.github/`.
- Never add personal paths, user names, tokens or other secrets; CI runs a secret scan on the full history.

## Fixes that belong upstream

A patch that is not specific to Killzone 3 (a lifter bug, a library that behaves differently from the PS3, a
runtime limit) is a candidate for [ps3recomp](https://github.com/sp00nznet/ps3recomp). Keep it free of anything
game-specific, with its focused test. Propose it there as an issue with symptom, cause, fix and test, and follow the
maintainer's lead on the pull request. The patch stays here until upstream takes it.

## Security and rights holders

- Report vulnerabilities privately ([SECURITY.md](.github/SECURITY.md)): GitHub's "Report a vulnerability" on the
  Security tab, never a public issue.
- Rights holders use the same private channel; formal notices follow [NOTICE.md](NOTICE.md) (Rights holders). Do
  not open issues or pull requests about such reports.
