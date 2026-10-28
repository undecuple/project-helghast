#!/usr/bin/env python3
"""Host test: licenses/ (the public copy of the licence texts) matches what the payload ships (release pass 49).

Checks, with no game data: every file under licenses/ is listed in licenses/README.md and the reverse; the payload's
fixed licence names (assemble_payload.py) and the toolchain manifest's license_file entries exist under licenses/;
each text is byte-identical to its source where that source is present (the repo's LICENSE, the ps3recomp
submodule, work/release/{sdl3,fonts}, the toolchain cache). A missing source is SKIP, not FAIL.
Run: python3 tools/release/test_licenses_match.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import assemble_payload as ap  # noqa: E402


def pub(rel):
    p = os.path.join(ROOT, "public", rel)
    return p if os.path.exists(p) else os.path.join(ROOT, rel)   # the public export puts public/ at its root


def same(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


def main():
    bad = 0

    def check(ok, msg):
        nonlocal bad
        bad += not ok
        print(("ok   " if ok else "FAIL ") + msg)

    lic = pub("licenses")
    have = sorted(os.path.relpath(os.path.join(d, n), lic).replace(os.sep, "/")
                  for d, _, ns in os.walk(lic) for n in ns if n != "README.md")
    with open(os.path.join(lic, "README.md"), encoding="utf-8") as f:
        listed = sorted(re.findall(r"^- `([^`]+)`", f.read(), re.M))
    check(have == listed, f"README.md lists exactly the {len(have)} files " + ("" if have == listed else
          f"(only on disk: {sorted(set(have) - set(listed))}, only listed: {sorted(set(listed) - set(have))})"))
    low = [h.lower() for h in have]
    check(len(low) == len(set(low)), "no two names differ only in case (Windows and macOS would merge them)")

    fixed = {"licenses/LICENSE.txt": pub("LICENSE"),
             "licenses/ps3recomp-LICENSE.txt": os.path.join(ROOT, "third_party", "ps3recomp", "LICENSE"),
             "licenses/at3_standalone-COPYING.LGPLv2.1.txt": os.path.join(
                 ROOT, "third_party", "ps3recomp", "third_party", "at3_standalone", "COPYING.LGPLv2.1"),
             ap.SDL3_DEST[1]: os.path.join(ROOT, "work", "release", "sdl3", ap.SDL3_SRC[1]),
             ap.FONT_LICENSE[1]: os.path.join(ROOT, "work", "release", "fonts", ap.FONT_LICENSE[0])}
    for dest, src in fixed.items():
        rel = dest[len("licenses/"):]
        check(rel in have, f"payload name {dest} exists in licenses/")
        if rel in have and os.path.isfile(src):
            check(same(os.path.join(lic, rel), src), f"{rel} is byte-identical to its source")
        elif rel in have:
            print(f"SKIP {rel}: source not present here")

    with open(os.path.join(HERE, "toolchain.json"), encoding="utf-8") as f:
        comps = json.load(f)["components"]
    cache = os.path.join(ROOT, "work", "release", "toolchain-cache", "trees")
    for c in comps:
        if c.get("license_file"):
            rel = f"{c['group']}/{c['license_file']}"
            check(rel in have, f"manifest licence {rel} exists in licenses/")
            src = os.path.join(cache, c["name"], c["license_file"])
            if rel in have and os.path.isfile(src):
                check(same(os.path.join(lic, rel), src), f"{rel} is byte-identical to the fetched text")
        elif c.get("license_globs"):
            got = [h for h in have if h.startswith(c["group"] + "/" + c["name"] + "-tree-")]
            check(bool(got), f"{c['name']}: a tree licence text ({c['license_globs']}) exists in licenses/")
    print("PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
