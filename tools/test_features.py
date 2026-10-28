#!/usr/bin/env python3
"""Host test for tools/features.py on a small fixture (no game data, no work/): the page is generated from the TSV,
--check passes on a fresh page and fails on a stale page, a missing enhancements.md entry, an unknown checkpoint,
a link outside the public export, a 'works' row without evidence and a port row naming no entry; in a public tree
(no checkpoint log) cp:NAME renders as the plain name and --check passes. Exit 0 = PASS.

    python3 tools/test_features.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import features  # noqa: E402

ENH = """# Enhancements

Everything not in the original game.

**Facts from the RPCS3 wiki page (pasted).** Not a feature.

**Resolution scaling (plan).** Host surfaces scaled.

Implemented:
- **Host PSARC VFS** (default on): reads served by the host. `KZ3_PSARC_VFS=0` turns it off.
  - a nested bullet with **bold** that is not an entry

## Native Linux build (planned)
After the Windows build.

## Wanted (backlog, not scheduled)
- **VR (SteamVR headsets).** Opt-in.
"""
CP = """# Screenshot checkpoints

| Time (UTC) | Checkpoint | Commit | What it shows | How it was produced |
|---|---|---|---|---|
| 2026-10-08 04:07:32 | [kz3-main-menu](20261008-040732Z_kz3-main-menu.png) | a950f50 | MAIN MENU | run.sh |
"""
TSV = """# fixture
kind\tarea\tfeature\tdetail\tstatus\tevidence\tnote
ps3\tMenus\tMain menu\tMain menu\tworks\tcp:kz3-main-menu, [requirements.md](docs/requirements.md)\t
ps3\tMenus\tPause menu\tSTART\tunverified\tnever opened\tsoon
ps3\tMove\tMove\tMotion play\tdeclined\tdecided\t
port\tAvailable now\tArchive reads\tHost PSARC VFS\tavailable\ton\tKZ3_PSARC_VFS=0 turns it off (cp:kz3-main-menu)
port\tPlanned\tResolution scale\tResolution scaling\tplanned\ton\t
port\tPlanned\tLinux build\tNative Linux build\tplanned\t-\t
port\tPlanned\tVR\tVR\tplanned\toff\t
port\tPlanned\tSettings page\t(prose item)\tplanned\t-\t
"""


def w(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    with tempfile.TemporaryDirectory() as root:
        w(root, features.ENH, ENH)
        w(root, features.CP_README, CP)
        w(root, features.DATA, TSV)
        page_path = os.path.join(root, features.PAGE)

        expect(features.main(["--root", root, "-q"]) == 0, "generate returns 0")
        page = open(page_path, encoding="utf-8").read()
        expect(page.startswith(features.HEADER + "\n"), "generated header line first")
        expect("| works | verified in a run | 1 |" in page, "works count 1")
        expect("| not yet verified | present in code, never exercised | 1 |" in page, "unverified count 1")
        expect("| declined | will not be done | 1 |" in page, "declined count 1")
        expect("Port-only features: 1 available now, 4 planned." in page, "port-only counts")
        expect("[kz3-main-menu](checkpoints/20261008-040732Z_kz3-main-menu.png)" in page, "checkpoint link resolved (relative to docs/)")
        expect("**not yet verified**: soon" in page, "status then note")
        expect("| VR | planned | off |" in page, "planned row carries the word planned")
        expect(features.main(["--root", root, "--check", "-q"]) == 0, "check passes on a fresh page")
        expect(features.enhancement_entries(features.pathlib.Path(root)) ==
               ["Resolution scaling (plan).", "Host PSARC VFS", "Native Linux build (planned)", "VR (SteamVR headsets)."],
               "enhancement entries parsed (ignored paragraph, nested bullet and the Wanted header skipped)")

        # Negative controls: each must make --check fail; the data is restored afterwards.
        with open(page_path, "a", encoding="utf-8") as f:
            f.write("\nhand edit\n")
        expect(features.main(["--root", root, "--check", "-q"]) == 1, "stale page caught")
        features.main(["--root", root, "-q"])

        w(root, features.ENH, ENH + "\n## Ray tracing (wanted backlog)\nShadows first.\n")
        expect(features.main(["--root", root, "--check", "-q"]) == 1, "missing enhancements.md entry caught")
        w(root, features.ENH, ENH)

        for bad, what in (
            (TSV.replace("cp:kz3-main-menu, [req", "cp:no-such-shot, [req"), "unknown checkpoint caught"),
            (TSV.replace("(docs/requirements.md)", "(docs/decisions.md)"), "link outside the public export caught"),
            (TSV.replace("\tworks\tcp:kz3-main-menu, [requirements.md](docs/requirements.md)\t", "\tworks\t\t"),
             "'works' without evidence caught"),
            (TSV.replace("\tHost PSARC VFS\t", "\tNo such entry\t"), "port row naming no entry caught"),
            (TSV.replace("\tunverified\t", "\tmaybe\t"), "unknown status caught"),
        ):
            w(root, features.DATA, bad)
            expect(features.main(["--root", root, "--check", "-q"]) == 1, what)
        w(root, features.DATA, TSV)
        expect(features.main(["--root", root, "--check", "-q"]) == 0, "check passes again after the controls")

    # Public tree (as tools/export-public.sh writes it): no checkpoint log, no enhancements.md, public/ copied to the
    # root (page at docs/FEATURES.md), checkpoint links turned into plain names. cp:NAME renders as NAME, an unknown
    # name is no error there, and the page generated from the data with plain names is the same page.
    with tempfile.TemporaryDirectory() as root:
        w(root, features.DATA, TSV)
        w(root, "docs/FEATURES.md", "old\n")
        expect(features.main(["--root", root, "-q"]) == 0, "public tree: generate returns 0")
        page = open(os.path.join(root, "docs/FEATURES.md"), encoding="utf-8").read()
        expect("| kz3-main-menu, [requirements.md](requirements.md) |" in page, "public tree: cp:NAME as plain NAME")
        expect("checkpoints/" not in page, "public tree: no checkpoint links")
        expect(not os.path.exists(os.path.join(root, features.PAGE)), "public tree: page written at docs/, not public/")
        expect(features.main(["--root", root, "--check", "-q"]) == 0, "public tree: check passes")
        w(root, features.DATA, TSV.replace("cp:kz3-main-menu", "kz3-main-menu"))
        expect(features.main(["--root", root, "--check", "-q"]) == 0, "public tree: plain names give the same page")
        w(root, features.DATA, TSV.replace("cp:kz3-main-menu", "cp:no-such-shot"))
        expect(features.main(["--root", root, "--check", "-q"]) == 1, "public tree: changed data still makes the page stale")

    if fails:
        print("FAIL test_features:")
        for f in fails:
            print("  - " + f)
        return 1
    print("PASS test_features (generate, check, 7 negative controls, public tree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
