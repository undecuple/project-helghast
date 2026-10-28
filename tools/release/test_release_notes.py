#!/usr/bin/env python3
"""Host test for release_notes.py: the real 0.1.3 section, refusals ([Unreleased] only, missing, empty), a heading
without (pre-release), both layouts (public/CHANGELOG.md and CHANGELOG.md at the root). No game data needed.
Run: python3 tools/release/test_release_notes.py
"""
import contextlib
import io
import os
import pathlib
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REAL = pathlib.Path(HERE).parents[1]
sys.path.insert(0, HERE)
import release_notes  # noqa: E402

bad = 0


def check(ok, what):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + what)


def run(root, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = release_notes.main(["--root", str(root), *args])
    return rc, out.getvalue(), err.getvalue()


def layout(base, text, public):
    p = pathlib.Path(base) / ("public" if public else "")
    p.mkdir(parents=True, exist_ok=True)
    (p / "CHANGELOG.md").write_text(text, encoding="utf-8")
    return pathlib.Path(base)


CL = """# Changelog

## [Unreleased]

### Added
- Not yet released.

## [1.2.3] - 2026-01-01
### Added
- First item.
- Second item.

### Fixed
- A fix.

## [1.2.2] - 2025-12-01 (pre-release)

### Added
- Old.
"""

# the real CHANGELOG (public/ in the repo, root in the export)
real = REAL
rc, out, err = run(real, "0.1.3")
check(rc == 0, "real CHANGELOG: 0.1.3 exits 0")
check(out.startswith("Project Helghast 0.1.3, a pre-release: partly playable, see docs/RELEASE-CRITERIA.md. New in 0.1.3:\n"), "first line")
check("**Added**\n\n- A disc dump whose game archive is split" in out and "\n**Fixed**\n\n- The game stopped a few seconds" in out, "Added then Fixed groups, items verbatim")
check(out.index("**Added**") < out.index("**Fixed**") and "A new page, docs/GETTING-FILES.md" in out, "order kept")
check("0.1.2" not in out.split("See CHANGELOG.md")[0], "no neighbouring section leaks in")
check(out.rstrip().endswith(release_notes.PARA2) and "See CHANGELOG.md for the full list.\n\n" + release_notes.PARA2 in out, "fixed second paragraph")

for public in (True, False):
    with tempfile.TemporaryDirectory() as d:
        root = layout(d, CL, public)
        tag = "public/" if public else "root"
        rc, out, _ = run(root, "1.2.3")
        check(rc == 0 and out.startswith("Project Helghast 1.2.3. New in 1.2.3:\n"), f"{tag}: non-pre-release heading drops the pre-release wording")
        check("- First item.\n- Second item.\n\n**Fixed**\n\n- A fix.\n" in out, f"{tag}: list grouped")
        rc, out, _ = run(root, "1.2.2")
        check(rc == 0 and "a pre-release: partly playable" in out and "Not yet" not in out, f"{tag}: pre-release heading")
        f = pathlib.Path(d) / "n.txt"
        rc, out, _ = run(root, "1.2.2", "--out", str(f))
        check(rc == 0 and out == "" and f.read_text().startswith("Project Helghast 1.2.2"), f"{tag}: --out writes the file")

with tempfile.TemporaryDirectory() as d:
    root = layout(d, "# Changelog\n\n## [Unreleased]\n\n### Added\n- Soon.\n", False)
    rc, out, err = run(root, "0.2.0")
    check(rc == 1 and out == "" and "Unreleased" in err, "only [Unreleased]: refused with a hint")
    rc, _, err = run(root, "Unreleased")
    check(rc == 1, "the name Unreleased is not a version")
with tempfile.TemporaryDirectory() as d:
    root = layout(d, CL, True)
    rc, _, err = run(root, "9.9.9")
    check(rc == 1 and "9.9.9" in err, "missing version: refused")
with tempfile.TemporaryDirectory() as d:
    root = layout(d, "# C\n\n## [0.3.0] - 2026-01-01 (pre-release)\n\n### Added\n\n## [0.2.0] - 2026-01-01\n- x\n", True)
    rc, _, err = run(root, "0.3.0")
    check(rc == 1 and "no items" in err, "empty section: refused")
with tempfile.TemporaryDirectory() as d:
    rc, _, err = run(d, "0.1.3")
    check(rc == 1, "no CHANGELOG file: refused")

print("test_release_notes: " + ("FAIL" if bad else "PASS"))
sys.exit(1 if bad else 0)
