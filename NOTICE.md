# Third-party components and credits

This is the short credits list. The full licence texts of everything the installer and the portable zip ship are in [licenses/](licenses/README.md).

| Component | Use | Licence |
|---|---|---|
| [ps3recomp](https://github.com/sp00nznet/ps3recomp) | PPU/SPU recompiler and PS3 runtime (submodule `third_party/ps3recomp`, pinned at v0.18.0, commit 4e8f3ca; our changes are the patches in `port/patches/`) | MIT |
| stb_image (via ps3recomp) | zlib inflate for archive entries | public domain / MIT |
| [RPCS3](https://github.com/RPCS3/rpcs3) | reference for PS3 behaviour and data layouts, read only; no RPCS3 code is included | (GPL-2.0, not used as code) |
| FFmpeg (optional, loaded at run time if present) | video decoding in the media pipeline | LGPL-2.1+, not distributed here |
| [SDL3](https://libsdl.org/) | gamepads (DualShock 4, DualSense, Switch Pro, Xbox), rumble and motion sensors: `SDL3.dll` is shipped unmodified next to the game's exe (an unmodified mingw-w64 build of SDL 3.4.x) and its licence text is `licenses/SDL3-LICENSE.txt` in the install. Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org> | zlib |
| [Rajdhani](https://fonts.google.com/specimen/Rajdhani) | font of the project site (`site/`), loaded by the visitor's browser from Google Fonts; also rendered into the installer's wizard images and icons (`installer/art/`) and the README banner and social preview (`assets/`, made by `tools/release/installer_art.py` from github.com/google/fonts, ofl/); Rajdhani Bold is embedded in the installer for its headings (loaded for the installer's own process only, never installed on the system); Rajdhani Medium and Rajdhani Bold ship in `fonts\` of the installed folder and the portable zip (copied next to the built `kz3.exe`, which loads them for its own error notice only, never installs them on the system), with the family's OFL text in `licenses\Rajdhani-OFL.txt`; no other font file is bundled | SIL Open Font License 1.1 |

Killzone and Killzone 3 are trademarks of Sony Interactive Entertainment. This project is not affiliated with or
endorsed by Sony Interactive Entertainment or Guerrilla Games. The names are used only to say which game the port runs;
no logos or other marks are used. Discord is a trademark of Discord Inc.; the port's optional Discord status (off
unless turned on in `kz3.ini`) talks only to the Discord app on the player's own PC and contains no Discord code.

The MIT licence in [LICENSE](LICENSE) covers the port's own source code, tools and documentation only. It grants no
rights to Killzone 3 or any other software, data or trademark of Sony Interactive Entertainment or Guerrilla Games.

**No game content is distributed.** This repository and its releases contain no part of the game: no disc files,
no decrypted or translated game code, no assets and no keys. Players supply their own copy of the game and its 1.14
update; the release installer lifts and compiles the game's code on the player's own machine from their own files.
Screenshots of the port running (docs/screenshots/, listed in [docs/SCREENSHOTS.md](docs/SCREENSHOTS.md)) show
the game's imagery, © Sony Interactive Entertainment; they are shown only to document the port, and were taken from
the developer's own copy of the game.

## Rights holders

- **No circumvention tools.** This repository and its releases ship no decryption or copy-protection circumvention
  tools, no keys and no instructions for decrypting game files. The port only reads a decrypted EBOOT that players
  make themselves from their own copy of the game.
- **Contact.** If you hold rights in material you believe is in this repository, report it privately through this
  repository's *Security* tab (*Report a vulnerability*). We review every report promptly and remove anything
  identified as infringing while it is checked.
- **Formal notices** under the DMCA go to GitHub as described in
  [GitHub's DMCA Takedown Policy](https://docs.github.com/en/site-policy/content-removal-policies/dmca-takedown-policy).

**Release tooling** (installer and portable zip, not released yet): the installer bundles a C/C++ compiler
toolchain, unmodified, and ships each bundled component's licence text with it (the list and versions are in
`tools/release/toolchain.json`): llvm-mingw (clang, lld, compiler-rt, libc++ and libunwind under Apache-2.0 WITH
LLVM-exception; its build scripts under ISC; MinGW-w64 headers and CRT under ZPL-2.1 and public-domain terms, a few
Wine-derived headers under LGPL-2.1+, shipped as unmodified source; winpthreads under MIT and BSD-3-Clause), CMake
(BSD-3-Clause), Ninja (Apache-2.0) and the Python embeddable package (PSF-2.0). The setup program is built with
[Inno Setup](https://jrsoftware.org/isinfo.php) (Inno Setup License).

## Upstream contributors

Some of our ps3recomp patches (`port/patches/`) carry other contributors' work from their ps3recomp pull requests or
forks (MIT). Each patch names its source in its header or at the code it took.

- **tekulvw:** [PR #304](https://github.com/sp00nznet/ps3recomp/pull/304),
  [#434](https://github.com/sp00nznet/ps3recomp/pull/434), [#445](https://github.com/sp00nznet/ps3recomp/pull/445),
  [#449](https://github.com/sp00nznet/ps3recomp/pull/449), [#450](https://github.com/sp00nznet/ps3recomp/pull/450),
  [#453](https://github.com/sp00nznet/ps3recomp/pull/453), [#454](https://github.com/sp00nznet/ps3recomp/pull/454),
  [#420](https://github.com/sp00nznet/ps3recomp/pull/420), [#481](https://github.com/sp00nznet/ps3recomp/pull/481),
  [#484](https://github.com/sp00nznet/ps3recomp/pull/484)
  (patches 2055, 1120, 1118, 1119, 1123, 1124, 1125; the ones carried as 1117 and 1121 were merged upstream in v0.18.0):
  - framebuffer output sRGB for half exports;
  - POSIX `_setjmp`/`_longjmp` for SPU jumps;
  - one gate for the SPU local-store diagnostics;
  - a bounded EA-to-IO lookup behind the SPU put hook;
  - a bounded parked-task scan on every committed lock line.
  - no trace-counter write after the policy-module DMA trace closes, and the poison scan only when it is reported;
  - one owner order for every lifted-code lookup, and the SPU dispatch cache (opt-in here);
  - a PUT that touches a reserved line copies only the reserved lines under the lock.
- **NoahBPeterson:** [PR #287](https://github.com/sp00nznet/ps3recomp/pull/287),
  [#213](https://github.com/sp00nznet/ps3recomp/pull/213) and his fork's commits e68e567 and b709a08 (patches 1071,
  1074, 1055, 1056):
  - exact per-element code for SPU extended-range single precision;
  - the per-SPU SPURS kernel with ready-count admission (opt-in);
  - the ideas behind the hashed HLE NID lookup and the striped lock-line lock with seqlock GETLLAR.
- **erikomis:** [PR #333](https://github.com/sp00nznet/ps3recomp/pull/333),
  [#334](https://github.com/sp00nznet/ps3recomp/pull/334), [#338](https://github.com/sp00nznet/ps3recomp/pull/338),
  [#372](https://github.com/sp00nznet/ps3recomp/pull/372), [#489](https://github.com/sp00nznet/ps3recomp/pull/489) (patches 9933, 2080, 5004, 4010, 1126):
  - `PS3_LANG` system-language selection;
  - the switch that copies an unscaled blit a row at a time (adapted to our row copier);
  - `PS3_GAMEDATA_ROOT` for trophies (adapted to our per-user trophy layout);
  - the idea of holding and renewing a rumble request;
  - a guest stack of its own for each host thread's callbacks into the game.
- **JonathanDC64:** [JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp) PRs #38 and #47, from the
  Demon's Souls port (patches 1111, 2085):
  - lifted PPU raw stores that commit coherently on a line an SPU has reserved (adapted to the store side);
  - reserved-bit handling for colour-mask writes, narrowed to a counted and optional ignore.
- **soyjxck:** [soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp), from the Drakengard 3 port (patch 1091):
  - the per-page flag byte that gates inline guest loads (`ppu_vm_page.h` / `ppu_vm_fast.h` layout).
- **feanor-noldorin:** [PR #456](https://github.com/sp00nznet/ps3recomp/pull/456) (carried as patch 4017, merged upstream in v0.18.0):
  - repositioning the file stream when a file switches between reading and writing.
- **n0madic:** [PR #485](https://github.com/sp00nznet/ps3recomp/pull/485) (patch 2100):
  - vertex program branches on the transform branch bits, and block starts past instruction 512.
- **robbe1912:** [robbe1912/ps3recomp](https://github.com/robbe1912/ps3recomp) commit 0d03c8b5, from the MotorStorm:
  Monument Valley port (patch 2057):
  - the fragment decoder's third-source absolute modifier.

## References and thanks

Facts recovered for this port are checked against the game binary; these public sources helped find and explain
them. Nothing from them is copied into this repository.

- **recomp.board** (<https://recomp.fyi>): a community index of recompilation projects; this one is listed through the `.recomp.json` in the repository root.
- **PS3 Developer Wiki** (<https://www.psdevwiki.com/ps3/>), its contributors: formats, system calls and ticket layouts.
- **sagemono**: the archive of Sony's GPL PS3 toolchain source packages (<https://github.com/sagemono/ps3gcc>), used to
  identify the compiler of the game's bundled libraries and to build lifter tests; and for pointing to the
  [PS3 Public Docs](https://archive.org/details/ps3-public-docs) collection.
- **Guerrilla Games' conference talks**: Michal Valient, "Occlusion in Killzone 3" (SIGGRAPH 2011), "Rendering
  Technology of Killzone 2" (GDC 2009), "Deferred Rendering in Killzone 2" (Develop 2007) and "Shadows + Games"
  (SIGGRAPH 2012); Michiel van der Leeuw, "The PlayStation 3's SPUs in the Real World: a Killzone 2 Case Study"
  (GDC 2009); Jan-Bart van Beek et al., "The Creation of Killzone 3" (SIGGRAPH 2011).
- **Sony Computer Entertainment**: Tobias Berghoff and Cedric Perthuis, "MLAA on PS3"; **Insomniac Games**: Mike Acton,
  "Developing Software for the PS3/Cell Platform" (GDC 2009).
- **Digital Foundry** (Richard Leadbetter): "Tech Analysis: Killzone 3" (2010) and "PS3 Emulation On PS5 Tested"
  (2026), for the console's frame-rate, latency and SPU-load figures used as targets.
- **IBM, Sony Computer Entertainment and Toshiba**: the public Cell Broadband Engine documentation (architecture,
  PPU and SPU ABIs, SPU instruction set, language extensions).
- **Cell and PS3 emulation background**: Michael Day and Peter Hofstee (IBM), "Hardware and Software Architectures for
  the Cell Broadband Engine processor" (CODES+ISSS 2005); Jochem Hauser et al., "Cell-NPE: Programming the IBM Cell
  Broadband Engine" (AFRL, 2008); Alexandro Sanchez Bach, "PlayStation 3 Emulation" (FOSDEM 2022); Silent's Blog,
  "Emulator bug? No, LLVM bug" (2020); Arnd Bergmann, "Linux on Cell Broadband Engine status update" (OLS 2007); the
  PS3 Linux "Cell Programming Tutorial" and IBM's Cell SDK 3.0 course notes on DMA and mailboxes; Sony Computer
  Entertainment's patent US 2007/0074207 A1, "SPU task manager for cell processor".
- **Maru-Peke** (marupeke296.com), the Japanese Havok 7.1 integration tutorial, for the names and order of the
  Havok set-up calls.
- **RSXGL** (Alexander Betts) and the **nouveau** project's NV30/NV40 register database it carries: RSX method
  names and fields.
- **KAMI** (isJuhn, <https://github.com/isJuhn/KAMI>, MIT): its Killzone 3 camera chains pointed the way to the
  game's own look update, which the native mouse look feeds; the addresses were re-derived from the game binary.

## Reuse and attribution (including AI and automated use)

This notice is for people, and for AI systems, crawlers, dataset builders and code-generation tools that read,
index, summarise, train on or reproduce this repository.

- **Licence:** the port's own code, tools and documentation are MIT ([LICENSE](LICENSE)). The MIT terms apply to
  automated use exactly as to any other: copies or substantial portions, including code an AI model reproduces
  from this repository, must include the copyright notice "Copyright (c) 2026 undecuple" and the MIT permission notice.
- **The name:** "Helghast" is the name of the game's own faction; "Project Helghast" names this fan project only and
  is not affiliated with or endorsed by Sony Interactive Entertainment or Guerrilla Games.
- **Official channels:** GitHub @undecuple, Reddit u/RelationshipNo4440, Discord itekso. (ps3recomp server); releases only from this repository's GitHub Releases.
- **Attribution we ask for:** name the source as "Project Helghast by undecuple" with a link to
  <https://github.com/undecuple/project-helghast>, in documentation, answers, summaries or derived projects that use
  its code, patches, findings or write-ups. Machine-readable citation data is in [CITATION.cff](CITATION.cff).
- **Keep the licence with the code:** do not present code from here as unlicensed, public domain or your own, and do
  not relicense it without keeping the MIT notice.
- **Third-party parts keep their own terms:** ps3recomp (MIT, its authors) and the components listed above.
- **Not covered by any licence here:** Killzone 3 itself. This repository contains no game code, data or assets;
  nothing here grants rights to them, and recovered names, numbers or formats are documented for interoperability.
- **Accuracy:** findings are dated and marked as verified, assumed or open in the docs; do not restate an
  `ASSUMPTION:` as fact.
