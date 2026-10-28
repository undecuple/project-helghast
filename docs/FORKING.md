# Copying or forking this project

For anyone who copies, forks or reuses this repository. The code is MIT-licensed, so you may; this page lists what
to change so your copy does not claim to be this project, and what to keep. Paths are relative to the repository root.

## 1. Names and branding

| What | Where | Change it to your own |
|---|---|---|
| Project name "Project Helghast", file names, tag line, owner and repository slug | `tools/release/names.py` (one place; the installer, launcher, zip and site read it) | yes |
| Window title (default text; the FPS suffix is off unless `KZ3_TITLE_STATS=1`) | `port/kz3/main.cpp`, `port/patches/5008-window-title-stats-switch.patch` | yes |
| Exe version block, icon and manifest | `port/kz3/app.rc`, `port/kz3/app.manifest`, `port/kz3/kz3-launcher.ico`, version in `port/kz3/CMakeLists.txt` | yes |
| Launcher version block and its error text | `installer/launcher/launcher.rc`, `installer/launcher/launcher.c` | yes |
| Installer script, art and wording | `installer/kz3.iss`, `installer/art/` | yes |
| Project site | `site/` (generated parts: `python3 tools/features.py`) | yes |
| Banner and social preview | `assets/` | yes |

"Helghast" is the game's own faction name. Keep the disclaimer in `NOTICE.md` that your project is a fan project, not
affiliated with or endorsed by the game's publisher or developer.

## 2. Identity and remotes

| What | Where |
|---|---|
| Author and maintainer (name, link) | `CITATION.cff`, `.recomp.json`, `.github/CODEOWNERS`, `NOTICE.md`, `LICENSE` (copyright line) |
| Repository URL | `README.md`, `CITATION.cff`, `.recomp.json`, `NOTICE.md`, `docs/*.md`, `site/index.html`, `.github/ISSUE_TEMPLATE/config.yml`, `.github/CONTRIBUTING.md`, and `tools/release/names.py` |
| Launcher's update-check URL (the releases-list API of the repository) | `tools/release/names.py` (`API_RELEASES`), used by `tools/release/version_check.py` (the installer's opt-in check runs it); a fork that keeps it would tell its users about this project's releases |
| Pages URL | `tools/release/names.py` (`SITE_URL`), `installer/launcher/launcher.rc`, `port/kz3/app.rc` |
| Funding link | `.github/FUNDING.yml` (delete or replace) |

Set your own git identity (`git config user.name`, `user.email`) before committing. Search the tree for the old owner
name and repository slug after editing; a plain `git grep` finds every leftover.

## 3. Versions and releases

- Tags are `vMAJOR.MINOR.PATCH`. While the major version is 0, mark every release as a pre-release on GitHub.
- Never move or reuse a tag; fix forward with a new patch version.
- Use the same version string everywhere: `port/kz3/CMakeLists.txt` (exe version block), the installer's
  `/DAppVersion=` value (`installer/kz3.iss`), the launcher's version block, and the tag. `CITATION.cff` carries the same
  version string too.
- Publish a SHA-256 checksum file next to every release asset (`Project-Helghast-setup.exe.sha256` and so on).
  The new-version check compares tags, so a tag that does not match a real release breaks it.

## 4. CI and GitHub settings

| What | Notes |
|---|---|
| `.github/workflows/ci.yml` | lint, patches apply to the pinned ps3recomp, host tests (`tools/host-tests.txt`), secret scan; needs no secrets |
| `.github/workflows/codeql.yml` | scheduled code scanning |
| `.github/workflows/issue-labels.yml` | adds `needs-triage` plus one type label; it only adds labels that already exist, so create these in your repository: `needs-triage`, `bug`, `enhancement`, `feedback`, `performance`, `pipeline-template` |
| `.github/dependabot.yml` | dependency update PRs; workflow actions are pinned by commit SHA |
| Issue forms | `.github/ISSUE_TEMPLATE/`; the label script matches the forms' first headings, so edit both together (`pipeline_template.yml` is the form behind the `pipeline-template` label) |
| `.github/workflows/release.yml` | runs on a pushed `vX.Y.Z` tag that equals `VERSION` in `tools/release/names.py`; builds the installer and portable zip, creates a draft release titled with the tag (pre-release while the major version is 0) with a `SHA256SUMS` file, and never replaces an existing release. A manual run only builds. Optional secret `KZ3_SENTRY_DSN` (tag builds only) |
| `.github/workflows/pages.yml` | publishes `site/` and `docs/screenshots/` to GitHub Pages; both jobs run only when the repository variable `PAGES_ENABLED` is `true` |
| Secrets | none are needed to run the shipped workflows; `KZ3_SENTRY_DSN` is optional (your own Sentry-compatible tracker's address for crash reports; without it the installer shows crash reports as not available). Add none you do not use |
| Settings to check | Actions: require approval for workflows from outside collaborators (fork pull requests), and keep the default token read-only; Pages: source "GitHub Actions" if you publish `site/`; add a ruleset on the default branch (pull request required, no force push, no deleting) and on `v*` tags (no update, no delete) |

## 5. Baked-in paths and environment variables

The port reads its settings from `KZ3_*`, `LD_*` and `RSX_*` environment variables and an ini file; the main ones
are described in [BUILDING.md](BUILDING.md), and the rest are documented in the source where they are read
(`getenv`). A generated list of every switch is not part of this tree.

Scripts that hold a host or path you may need to adapt:

| Script | What it assumes |
|---|---|
| `tools/sandbox.sh` | the bubblewrap layout (repository read-only, writes only under `work/` and `build/`); the path of the shared `work/` of a main checkout |
| `tools/build-kz3.sh`, `tools/lift.sh`, `tools/kz3-stage.sh` | `work/` and `build/` under the repository; lock files under `work/locks` |
| `tools/proton-test.sh`, `tools/proton-inner.sh` | a Proton install and a prefix on the machine that runs them |
| `tools/release/names.py` | GitHub owner, repository and site URL |
| `tools/release/toolchain.json`, `installer/art/art.json` | download URLs and SHA-256 values of the bundled toolchain and fonts |

## 6. What the user supplies

Your users supply their own copy of the game: the US disc (BCUS98234) dumped as a folder and the 1.14 update. No
game files are shipped here or by a release. The EBOOT used for lifting comes from the player's own copy; for
dumping and EBOOT questions see [RPCS3](https://rpcs3.net) and its documentation. The port reads the dump in place
and never copies it. [requirements.md](requirements.md) lists what a player needs.

Keep it that way in a fork: do not commit disc files, an EBOOT, extracted assets, keys or a built game exe.

## 7. Licences and credits to keep

- `LICENSE` (MIT): keep the notice with any copied code; add your own line, do not remove the existing one.
- `NOTICE.md`: the credit lines for ps3recomp (MIT) and other referenced work, the third-party notices (fonts and
  the bundled toolchain with their licences), the trademark statement. Extend it
  when you add a dependency.
- ps3recomp is a git submodule (`third_party/ps3recomp`, pinned in `.gitmodules` and by commit); the patches in
  `port/patches/` apply to that pin. Keep its licence and credit its author.
- If you change what goes online (every item there is off until the player turns it on), change the "what goes online" list in `README.md` too.

## 8. Security checks

| Check | Where | Covers |
|---|---|---|
| Secret scan (Gitleaks, full history) | `.github/workflows/ci.yml`, `.gitleaks.toml` | default rules; the one allowlist matches only environment-switch assignments (a name starting with `KZ3_` followed by `=value`), so any other finding fails |
| Lint | `tools/lint.sh`, CI | shell scripts (shellcheck at warning level), Python (ruff, report only) |
| Patch check | CI | every patch in `port/patches/` applies to the pinned ps3recomp |
| Host tests | `tools/run-host-tests.sh`, `tools/host-tests.txt` | tests that need no game data and no GPU; add each new test to the list |
| Issue labeller | `.github/workflows/issue-labels.yml` | reads the issue text only as data, no checkout, token limited to issues, fixed label list |
| CodeQL | `.github/workflows/codeql.yml` | scheduled scan |
| Reporting | `.github/SECURITY.md` | change the contact to yours |

Review any pull request that touches workflows, `tools/release/` or `installer/` with extra care: they run on
people's machines or with repository permissions.

## 9. What the public tree leaves out

The public tree holds the port's code and patches, the build, lift and packaging tools, the data tables they read,
host tests and these docs. It leaves out local editor and tool settings, working
notes, run and measurement tools, and all game data. `tools/agents/` holds only the build and GPU slot locks that the build scripts use; `tools/sandbox.sh` can read an optional per-worktree policy file that is not part of the tree, and without it the sandbox runs its default profile. A fork does not need any of that: build from the files here
following [BUILDING.md](BUILDING.md). If you use an AI coding tool on your fork, `AGENTS.md` describes the repository
for it.

Comments in the code and scripts cite internal numbering (pass, round and plan items) from the development notes; they are labels, not links.
