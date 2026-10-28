#!/usr/bin/env python3
"""Read-only check that a published GitHub release and the repository agree (release, version automation 4).

Run by public/.github/workflows/release-check.yml on `release: published`; stdlib only, so it is host-tested
(test_release_check.py). Names come from names.py (the same ones release.yml publishes), never a second copy.

  release_check.py --tag vX.Y.Z --release FILE|-  [--assets DIR] [--root DIR]

--release is the release JSON (GitHub REST: tag_name, name, draft, prerelease, body, assets[].name). Checks:
  1 tag = "v" + names.VERSION (names.py at the checked-out tag)
  2 published (not a draft), pre-release exactly while the version is 0.x, title = tag, body names the version
  3 assets are exactly: setup.exe + .sha256, <ver>-portable.zip + .sha256, SHA256SUMS
  4 (with --assets) every .sha256 matches its file and agrees with SHA256SUMS, and SHA256SUMS matches the files
  5 (with --root) README.md and site/index.html contain releases/tag/<tag>
Prints one line per problem; exit 0 only when there are none.
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import names  # noqa: E402

SUMS = "SHA256SUMS"


def expected_assets(version):
    setup = f"{names.SETUP_BASE}.exe"
    zipn = names.zip_name(version)
    return [setup, setup + ".sha256", zipn, zipn + ".sha256", SUMS]


def check_release(rel, tag, version=None):
    version = version or names.VERSION
    errs = []
    if tag != "v" + version:
        errs.append(f"tag {tag} is not v{version} (names.py VERSION)")
    if rel.get("tag_name") != tag:
        errs.append(f"release tag_name {rel.get('tag_name')!r} is not {tag!r}")
    if rel.get("draft"):
        errs.append("release is still a draft")
    want_pre = version.split(".")[0] == "0"
    if bool(rel.get("prerelease")) != want_pre:
        errs.append(f"prerelease is {bool(rel.get('prerelease'))} but version {version} needs {want_pre}")
    if rel.get("name") != tag:
        errs.append(f"release title {rel.get('name')!r} is not the tag {tag!r}")
    if version not in (rel.get("body") or ""):
        errs.append(f"release body does not name {version}")
    have = [a.get("name") for a in rel.get("assets", [])]
    want = expected_assets(version)
    errs += [f"missing asset {n}" for n in want if n not in have]
    errs += [f"unexpected asset {n}" for n in have if n not in want]
    dup = sorted({n for n in have if have.count(n) > 1})
    errs += [f"duplicate asset {n}" for n in dup]
    return errs


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_sums(path):
    """{name: hash} from 'HASH  NAME' / 'HASH *NAME' lines; unreadable lines become '?bad line' keys."""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+?)\r?\n?", line)
            if m:
                out[m.group(2)] = m.group(1).lower()
            elif line.strip():
                out["?bad line: " + line.strip()] = ""
    return out


def check_hashes(d, version=None):
    version = version or names.VERSION
    errs = []
    sums_path = os.path.join(d, SUMS)
    if not os.path.exists(sums_path):
        return [f"{SUMS} not downloaded"]
    sums = parse_sums(sums_path)
    errs += [f"{SUMS}: {k[1:]}" for k in sums if k.startswith("?")]
    setup = f"{names.SETUP_BASE}.exe"
    for n in (setup, names.zip_name(version)):
        p = os.path.join(d, n)
        if not os.path.exists(p):
            errs.append(f"{n} not downloaded")
            continue
        actual = sha256_file(p)
        if sums.get(n) != actual:
            errs.append(f"{SUMS} entry for {n} is {sums.get(n)!r}, file is {actual}")
        sp = os.path.join(d, n + ".sha256")
        if not os.path.exists(sp):
            errs.append(f"{n}.sha256 not downloaded")
            continue
        own = parse_sums(sp)
        if own.get(n) != actual:
            errs.append(f"{n}.sha256 says {own.get(n)!r}, file is {actual}")
        if own.get(n) != sums.get(n):
            errs.append(f"{n}.sha256 and {SUMS} disagree")
    return errs


def check_links(root, tag):
    errs = []
    for rel in ("README.md", os.path.join("site", "index.html")):
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            errs.append(f"{rel} missing at the tag")
            continue
        with open(p, encoding="utf-8") as f:
            if f"releases/tag/{tag}" not in f.read():
                errs.append(f"{rel} does not link releases/tag/{tag}")
    return errs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--release", required=True, help="release JSON file, or - for stdin")
    ap.add_argument("--assets", help="folder with the downloaded assets")
    ap.add_argument("--root", help="repository root at the tag (README, site)")
    a = ap.parse_args(argv)
    if a.release == "-":
        rel = json.load(sys.stdin)
    else:
        with open(a.release, encoding="utf-8") as f:
            rel = json.load(f)
    errs = check_release(rel, a.tag)
    if a.assets:
        errs += check_hashes(a.assets)
    if a.root:
        errs += check_links(a.root, a.tag)
    for e in errs:
        print("release-check: FAIL: " + e)
    if not errs:
        print(f"release-check: {a.tag} OK")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
