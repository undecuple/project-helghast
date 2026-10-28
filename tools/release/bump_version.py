#!/usr/bin/env python3
"""Bump the release version everywhere in one go (version automation items 1 and 2).

  python3 tools/release/bump_version.py [--check] [--date YYYY-MM-DD] NEW_VERSION

names.VERSION is the single source. This edits it and every mirror: launcher.rc (FILEVERSION and PRODUCTVERSION as
comma numbers, FileVersion and ProductVersion strings), public/CITATION.cff (version, and date-released if present),
port/kz3/CMakeLists.txt (project VERSION), installer/kz3.iss (#define AppVersion) and every marker span in the public
text (`<!-- version -->...<!-- /version -->`, on one line; every X.Y.Z inside a span becomes the new version, so a
span may hold a whole link). It adds a CHANGELOG stub `## [X.Y.Z] - DATE (pre-release)` with an empty "### Added"
above the newest entry (left alone when that version's section exists), regenerates the site (website.py) and runs
test_version_agree.py; exit 1 when that fails. Running it with the current version changes nothing. It refuses a
version that is not a newer X.Y.Z. --check reports what would change and writes nothing. Local tool: no network, no
git, no push; the result goes through the gate as one commit.
"""
import argparse
import datetime
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

MARKER = re.compile(r"(<!-- version -->)(.*?)(<!-- /version -->)")
SEMVER = re.compile(r"\d+\.\d+\.\d+")
PUBLIC_MARKER_FILES = ("public/README.md", "public/docs/BUILDING.md", "public/docs/ROADMAP.md")


def at(root, rel):
    """The file for a repo path: the public export puts public/ at its root (as test_version_agree.py reads it)."""
    if rel.startswith("public/") and not (root / "public").is_dir():
        return root / rel[len("public/"):]
    return root / rel


def parse(v):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", v)
    return tuple(int(x) for x in m.groups()) if m else None


def current_version(root):
    text = (root / "tools/release/names.py").read_text(encoding="utf-8")
    m = re.search(r'^VERSION\s*=\s*"(\d+\.\d+\.\d+)"', text, re.M)
    if not m:
        raise SystemExit("bump_version: names.VERSION not found")
    return m.group(1)


def fill_markers(text, version):
    return MARKER.sub(lambda m: m.group(1) + SEMVER.sub(version, m.group(2)) + m.group(3), text)


def sub1(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, flags=re.M)
    if n != 1:
        raise SystemExit(f"bump_version: expected exactly one {what}, found {n}")
    return new


def changelog_stub(text, version, date):
    if re.search(rf"^## \[{re.escape(version)}\]", text, re.M):
        return text
    if re.search(r"^## \[Unreleased\][ \t]*$", text, re.M):   # Keep a Changelog: the release takes the unreleased entries
        return re.sub(r"^## \[Unreleased\][ \t]*$", f"## [{version}] - {date} (pre-release)", text, count=1, flags=re.M)
    m = re.search(r"^## \[", text, re.M)
    if not m:
        raise SystemExit("bump_version: no '## [' entry in CHANGELOG.md to put the stub above")
    stub = f"## [{version}] - {date} (pre-release)\n\n### Added\n\n"
    return text[:m.start()] + stub + text[m.start():]


def edits(root, version, date):
    """{relative path: new text} for every file whose text changes."""
    out = {}

    def edit(rel, fn):
        p = at(root, rel)
        old = p.read_text(encoding="utf-8")
        new = fn(old)
        if new != old:
            out[rel] = new

    nums = ",".join((version.split(".") + ["0"])[:4])
    edit("tools/release/names.py",
         lambda t: sub1(t, r'^(VERSION\s*=\s*")\d+\.\d+\.\d+(")', rf"\g<1>{version}\2", "names.VERSION"))

    def rc(t):
        t = sub1(t, r"^(FILEVERSION\s+).*$", rf"\g<1>{nums}", "FILEVERSION")
        t = sub1(t, r"^(PRODUCTVERSION\s+).*$", rf"\g<1>{nums}", "PRODUCTVERSION")
        t = sub1(t, r'(VALUE "FileVersion", ")[^"]*(")', rf"\g<1>{version}\2", "FileVersion")
        return sub1(t, r'(VALUE "ProductVersion", ")[^"]*(")', rf"\g<1>{version}\2", "ProductVersion")
    edit("installer/launcher/launcher.rc", rc)

    def cff(t):
        t = sub1(t, r"^(version:\s*)\"?[^\s\"]+\"?\s*$", rf"\g<1>{version}", "CITATION version")
        if re.search(r"^date-released:", t, re.M) and version != current_version(root):
            t = re.sub(r"^(date-released:\s*).*$", rf"\g<1>\"{date}\"", t, flags=re.M)
        return t
    edit("public/CITATION.cff", cff)
    edit("port/kz3/CMakeLists.txt",
         lambda t: sub1(t, r"^(\s*VERSION\s+)\d+\.\d+\.\d+(\s*)$", rf"\g<1>{version}\2", "project VERSION"))
    edit("installer/kz3.iss",
         lambda t: sub1(t, r'^(\s*#define AppVersion ")[^"]*(")', rf"\g<1>{version}\2", "AppVersion"))
    for rel in PUBLIC_MARKER_FILES:
        edit(rel, lambda t: fill_markers(t, version))
    edit("public/CHANGELOG.md", lambda t: changelog_stub(t, version, date))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("version", help="the new version, X.Y.Z, newer than names.VERSION")
    ap.add_argument("--check", action="store_true", help="report what would change; write nothing")
    ap.add_argument("--date", default=datetime.date.today().isoformat(), help="CHANGELOG/CITATION date (default today)")
    ap.add_argument("--root", default=str(ROOT), help="repo root (tests use a temp copy)")
    a = ap.parse_args(argv)
    root = pathlib.Path(a.root)
    new = parse(a.version)
    if new is None:
        print(f"bump_version: {a.version!r} is not X.Y.Z", file=sys.stderr)
        return 2
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", a.date):
        print(f"bump_version: --date {a.date!r} is not YYYY-MM-DD", file=sys.stderr)
        return 2
    cur = current_version(root)
    if new < parse(cur):
        print(f"bump_version: {a.version} is older than the current {cur}", file=sys.stderr)
        return 2
    changed = edits(root, a.version, a.date)
    for rel in sorted(changed):
        print(("would change " if a.check else "changed ") + rel)
    if not changed:
        print(f"bump_version: nothing to change (already {a.version})")
    if a.check:
        return 0
    for rel, text in changed.items():
        at(root, rel).write_text(text, encoding="utf-8")
    if changed:
        r = subprocess.run([sys.executable, str(root / "tools/release/website.py"), "--root", str(root), "-q"])
        if r.returncode:
            print("bump_version: website.py failed", file=sys.stderr)
            return 1
    r = subprocess.run([sys.executable, str(root / "tools/release/test_version_agree.py")],
                       capture_output=True, text=True)
    tail = r.stdout.strip().splitlines()[-1:] or [""]
    print("test_version_agree: " + tail[0])
    if r.returncode:
        print(r.stdout[-2000:], file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
