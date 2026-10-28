#!/usr/bin/env python3
"""Host test: the release version is one string in every place that carries it (release pass 44).

names.VERSION is the source; the exe's CMake project version (-> app.rc), the launcher's version resource,
the installer's AppVersion default and CITATION.cff must all match it. Public text (md, site html,
website.py; not CHANGELOG.md) may name no other version of this project as a release. No game data needed.
Run: python3 tools/release/test_version_agree.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import names  # noqa: E402


def read(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path) and rel.startswith("public/"):
        path = os.path.join(ROOT, rel[len("public/"):])   # the public export puts public/ at its root
    with open(path, encoding="utf-8") as f:
        return f.read()


def one(rel, pattern):
    m = re.search(pattern, read(rel), re.M)
    return m.group(1) if m else None


# Release mentions of OUR version in public text (item 3 of the version automation). A bare 0.x.y is not
# enough (ps3recomp v0.18.0 is another project's), so only these contexts count; the capture is the version.
MENTION = [re.compile(p) for p in (
    r"releases/tag/v(0\.\d+\.\d+)",
    r"\b[Vv]ersion v?(0\.\d+\.\d+)",
    r"\bv?(0\.\d+\.\d+)\b(?:</strong>)?,? pre-releases?\b",
    r"\b(?:latest|releases?) v(0\.\d+\.\d+)",
    r"\[v(0\.\d+\.\d+)\]\([^)]*releases",
)]


def stale_mentions(text, version):
    """Versions other than `version` that `text` names as a release of this project."""
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        for rx in MENTION:
            out += [(i, m.group(1)) for m in rx.finditer(line) if m.group(1) != version]
    return out


MARKER = re.compile(r"<!-- version -->(.*?)<!-- /version -->")   # filled by bump_version.py


def marker_versions(text):
    """Every X.Y.Z inside marker spans (bump_version.py fills them from names.VERSION)."""
    return [(i, v) for i, line in enumerate(text.splitlines(), 1)
            for m in MARKER.finditer(line) for v in re.findall(r"\d+\.\d+\.\d+", m.group(1))]


def public_files():
    pub = os.path.join(ROOT, "public")
    if not os.path.isdir(pub):
        pub = ROOT   # the public export has public/ at its root
    for dp, _, fs in os.walk(pub):
        for f in fs:
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, pub)
            if rel.startswith((".git" + os.sep, "licenses")) or f == "CHANGELOG.md":
                continue
            if f.endswith(".md") or (f.endswith(".html") and rel.startswith("site" + os.sep)):
                yield full
    web = os.path.join(HERE, "website.py")
    if os.path.exists(web):
        yield web


def main():
    v = names.VERSION
    nums = ",".join((v.split(".") + ["0"] * 4)[:4])
    cm = one("port/kz3/CMakeLists.txt", r"^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$")
    app = read("port/kz3/app.rc")
    got = {
        "port/kz3/CMakeLists.txt project VERSION": cm,
        "launcher.rc FILEVERSION": one("installer/launcher/launcher.rc", r"^FILEVERSION\s+(\S+)"),
        "launcher.rc PRODUCTVERSION": one("installer/launcher/launcher.rc", r"^PRODUCTVERSION\s+(\S+)"),
        "launcher.rc FileVersion": one("installer/launcher/launcher.rc", r'VALUE "FileVersion", "([^"]+)"'),
        "launcher.rc ProductVersion": one("installer/launcher/launcher.rc", r'VALUE "ProductVersion", "([^"]+)"'),
        "kz3.iss AppVersion default": one("installer/kz3.iss", r'#define AppVersion "([^"]+)"'),
        "CITATION.cff version": one("public/CITATION.cff", r"^version:\s*\"?([^\s\"]+)\"?\s*$"),
    }
    want = {k: (nums if "FILEVERSION" in k or "PRODUCTVERSION" in k else v) for k in got}
    bad = 0
    for k, val in got.items():
        ok = val == want[k]
        bad += not ok
        print(("ok   " if ok else "FAIL ") + f"{k}: {val!r} (want {want[k]!r})")
    # app.rc takes its numbers and string from the CMake definitions; check it still does
    ok = all(t in app for t in ("FILEVERSION KZ3_VER_MAJOR,KZ3_VER_MINOR,KZ3_VER_PATCH", "KZ3_VER_STR"))
    bad += not ok
    print(("ok   " if ok else "FAIL ") + "app.rc uses the KZ3_VER_* definitions")
    # negative controls: the rule must catch a stale release mention and ignore other projects' versions
    for text, fails in (("v0.1.0 pre-release, partly", True), ("Version 0.0.9 is a pre-release", True),
                        ("releases/tag/v0.1.0", True), ("(latest v0.1.0)", True),
                        ("**Download:** [v0.1.0](https://example.invalid/releases/tag/v0.1.1)", True),
                        (f"v{v} pre-release, releases/tag/v{v}, Version {v}", False),
                        ("pinned at ps3recomp v0.18.0, commit 4e8f3ca", False),
                        ("merged upstream in v0.18.0", False), ("gitleaks v8.24.0, Python 3.13.1", False),
                        ("## 0.1.0 - 2026-10-27 (CHANGELOG line)", False), ("v0.x = previews", False)):
        ok = bool(stale_mentions(text, v)) == fails
        bad += not ok
        print(("ok   " if ok else "FAIL ") + f"control {'must flag' if fails else 'must pass'}: {text!r}")
    nfiles = nmark = 0
    ok = marker_versions("a <!-- version -->v0.0.1<!-- /version --> b") == [(1, "0.0.1")]
    bad += not ok
    print(("ok   " if ok else "FAIL ") + "control: a marker span is read")
    for full in public_files():
        nfiles += 1
        with open(full, encoding="utf-8") as f:
            text = f.read()
        nmark += len(MARKER.findall(text))
        for line, ver in marker_versions(text):
            if ver != v:
                bad += 1
                print(f"FAIL {os.path.relpath(full, ROOT)}:{line}: version marker holds {ver}, current is {v}")
        for line, ver in stale_mentions(MARKER.sub(lambda m: m.group(1), text), v):
            bad += 1
            print(f"FAIL {os.path.relpath(full, ROOT)}:{line}: names release {ver}, current is {v}")
    ok = nmark >= 4
    bad += not ok
    print(("ok   " if ok else "FAIL ") + f"version markers found ({nmark}; README 2, BUILDING, ROADMAP)")
    ok = nfiles >= 5
    bad += not ok
    print(("ok   " if ok else "FAIL ") + f"public text scanned for stale release mentions ({nfiles} files)")
    print("PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
