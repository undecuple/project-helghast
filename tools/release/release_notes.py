#!/usr/bin/env python3
"""The GitHub release body for a version, built from the CHANGELOG section (release.yml writes notes.txt with it).

  python3 tools/release/release_notes.py VERSION [--root R] [--out FILE]

Reads `## [VERSION] - DATE (pre-release)` from public/CHANGELOG.md (or CHANGELOG.md at the root of the public export;
--root defaults to the repo or export root) and prints: a first paragraph ("Project Helghast VERSION, a pre-release:
partly playable, see docs/RELEASE-CRITERIA.md. New in VERSION:") with the section's items as a Markdown list grouped
under its ### headings in order (item text verbatim), "See CHANGELOG.md for the full list.", then the fixed second
paragraph (installer, inputs, sums). "a pre-release: partly playable" is left out when the heading lacks
"(pre-release)". Exit 1 with a message when the section is missing, empty, or the version is only [Unreleased]: the
release must never go out with generic text silently. Local tool: no network, no git.
"""
import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

PARA2 = ("Windows installer and portable zip. You need your own Killzone 3 disc dump (a folder or a decrypted ISO), "
         "the 1.14 update and a decrypted EBOOT from your own copy; none of them is included or downloaded. "
         "SHA-256 sums: SHA256SUMS (and the .sha256 files). See the README for what it does and what goes online.")


def at(root, rel):
    """The file for a repo path: the public export puts public/ at its root."""
    if rel.startswith("public/") and not (root / "public").is_dir():
        return root / rel[len("public/"):]
    return root / rel


class NotesError(Exception):
    pass


def section(text, version):
    """(heading line, body lines) of the `## [version]` section."""
    lines = text.splitlines()
    head = re.compile(r"^## \[" + re.escape(version) + r"\](?!\S)")
    for i, ln in enumerate(lines):
        if head.match(ln):
            body = []
            for nxt in lines[i + 1:]:
                if nxt.startswith("## "):
                    break
                body.append(nxt)
            return ln, body
    raise NotesError(f"no '## [{version}]' section in the CHANGELOG"
                     + (" (only [Unreleased]: bump_version.py turns it into the release)"
                        if re.search(r"^## \[Unreleased\]", text, re.M) else ""))


def build(text, version):
    heading, body = section(text, version)
    groups, cur = [], None
    for ln in body:
        if ln.startswith("### "):
            cur = (ln[4:].strip(), [])
            groups.append(cur)
        elif ln.startswith("- "):
            if cur is None:
                cur = ("Changes", [])
                groups.append(cur)
            cur[1].append(ln)
        elif ln.strip() and cur and cur[1] and ln.startswith((" ", "\t")):
            cur[1][-1] += "\n" + ln          # a continuation line of the previous item
    groups = [g for g in groups if g[1]]
    if not groups:
        raise NotesError(f"the '## [{version}]' section has no items")
    pre = "(pre-release)" in heading
    first = f"Project Helghast {version}" + (", a pre-release: partly playable, see docs/RELEASE-CRITERIA.md" if pre else "")
    out = [first + f". New in {version}:", ""]
    for name, items in groups:
        out += [f"**{name}**", ""] + items + [""]
    out += ["See CHANGELOG.md for the full list.", "", PARA2]
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("version")
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        print(f"release_notes: '{a.version}' is not X.Y.Z", file=sys.stderr)
        return 1
    path = at(pathlib.Path(a.root), "public/CHANGELOG.md")
    try:
        notes = build(path.read_text(encoding="utf-8"), a.version)
    except (OSError, NotesError) as e:
        print(f"release_notes: {e}", file=sys.stderr)
        return 1
    if a.out:
        pathlib.Path(a.out).write_text(notes, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
