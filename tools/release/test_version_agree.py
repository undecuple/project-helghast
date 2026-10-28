#!/usr/bin/env python3
"""Host test: the release version is one string in every place that carries it (release pass 44).

names.VERSION is the source; the exe's CMake project version (-> app.rc), the launcher's version resource,
the installer's AppVersion default and CITATION.cff must all match it. No game data needed.
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
    print("PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
