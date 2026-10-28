# Contributing

Contributions are welcome: bug reports, fixes, feedback and questions, and ideas for features. Use the issue templates
(bug report, performance report, feature request, feedback or question) or open a pull request. A fix pull request is
checked by CI (lint, every patch applies to the pinned ps3recomp, the host tests, a secret scan) and by a read of the
change; run `tools/run-host-tests.sh` yourself first. General recompiler fixes go upstream to ps3recomp (see below).

Pull requests and feature requests are welcome; for big work open an issue first. There is no promise of a review time or
of merging every request. Before reporting, look at [KNOWN_ISSUES.md](../docs/KNOWN_ISSUES.md).

## Ground rules

- **No game material, ever:** no files from the disc or the update, no decrypted or lifted game code, no extracted
  assets, saves, keys or firmware, and no links to where they can be downloaded. Numbers, names, structure layouts
  and formulas worked out from the game are fine.
- **No GPL code:** RPCS3 is a reference to read, not code to copy. Write behaviour from your own understanding.
  Contributed code must be MIT-compatible; anything under GPL or LGPL needs the maintainer's agreement first.
- **Say if an AI tool helped:** AI-assisted work is disclosed in the pull request (the template asks); you remain
  responsible for what you submit.
- **Recompiler fixes go upstream:** a fix that is not specific to Killzone 3 belongs in
  [ps3recomp](https://github.com/sp00nznet/ps3recomp). Here it lives as a patch in `port/patches/` until upstream
  takes it.

## Reporting a problem

Use the issue templates. Say which build you ran (commit or release), whether the 1.00 disc or the 1.14 update, your GPU
and driver, Windows or Proton (and which Proton), the chapter or menu, and attach the run's log (`app_stdout.txt`, or the log
file next to the exe) and a screenshot if something looks wrong. Do not attach save files, dumps of game memory or
anything else that contains game data. Remove your user name and folder paths from logs before attaching them. Security problems: see [SECURITY.md](SECURITY.md), not an issue.

## Suggesting another game

Want a different PS3 game ported this way? Post it, or upvote an existing post, in
[Game requests](https://github.com/undecuple/project-helghast/discussions/categories/game-requests) under Discussions, not as an issue. Never link to game files or copies of games.

## Making a change

- Build instructions: [docs/BUILDING.md](../docs/BUILDING.md). An overview of the code: [docs/HOW-IT-WORKS.md](../docs/HOW-IT-WORKS.md).
- Using an AI coding agent? Point it at [AGENTS.md](../AGENTS.md) (the same rules, written for agents); you remain
  responsible for what it submits.
- Port code lives in `port/` (`kz3` the program, `spu` SPU scheduling and PPU overrides, `vfs` the update overlay).
- Changes to ps3recomp: a new numbered patch in `port/patches/` (`git diff` style against the pinned submodule, in
  the number block of its area); `tools/ps3recomp-patched.sh` applies them all and fails on one that does not apply.
- Keep the original function addresses in comments (`// @0x00123abc`) and mark guesses `ASSUMPTION:`. New
  non-original features are marked `ENHANCEMENT:`; anything that changes gameplay stays optional.
- Commits: short, descriptive messages.

## Testing

- **Without the game:** run what CI runs before you open a pull request:
  `nix develop .#ci --command bash -c 'git ls-files "*.sh" | xargs shellcheck -S warning && tools/ps3recomp-patched.sh && tools/run-host-tests.sh'`.
  A runtime fix should come with a host test (see `tools/upstream/` for examples) that fails on the unpatched code
  and passes with the fix; add it to `tools/host-tests.txt`.
- **With the game:** changes that need the game are tested with your own copy under Proton or Windows
  (`tools/proton-test.sh`). Say in the pull request what you ran and what you saw; a screenshot helps.

## Proposing a fix to ps3recomp

If a patch here fixes something general (a lifter bug, a library that behaves differently from the PS3, a runtime
limit), it can go to ps3recomp: open an issue there describing the symptom, the cause and the fix, with the focused
test, and follow the maintainer's lead on how they want the pull request. Keep it free of anything specific to
Killzone 3.
