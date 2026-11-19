#!/usr/bin/env python3
"""Host test for tools/release/draw_dividers.py and the README's images (no game data, no work/): the committed PNGs
equal a fresh render (--check), a render is deterministic, files are 900 px wide RGBA, under 20 KB, with a transparent
corner and some opaque red; --check fails on a changed file; every relative image in public/README.md exists.
Exit 0 = PASS.

    python3 tools/release/test_draw_dividers.py
"""
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import draw_dividers as dd  # noqa: E402

ROOT = dd.SITE
fails = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        fails.append(msg)


a, b = dd.build(), dd.build()
check(a == b and len(a) >= 3, "render is deterministic, at least 3 images")
for name, d in a.items():
    w, h, depth, ctype = struct.unpack(">IIBB", d[16:26])
    check((w, ctype, depth) == (900, 6, 8) and h <= 40, f"{name}: 900 px wide RGBA, small height ({w}x{h})")
    check(len(d) <= dd.MAX_BYTES, f"{name}: {len(d)} bytes under 20 KB")
    check(b"game" not in d.lower(), f"{name}: no stray text")
check(dd.main(["--check"]) == 0, "committed PNGs match a fresh render")
with tempfile.TemporaryDirectory() as t:
    dd.main(["--out", t])
    check(dd.main(["--check", "--out", t]) == 0, "--check passes on a fresh render")
    p = os.path.join(t, "divider-rule.png")
    open(p, "ab").write(b"x")
    check(dd.main(["--check", "--out", t]) == 1, "--check fails on a changed file")
readme = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
paths = re.findall(r'src="([^"#:]+)"', readme) + re.findall(r"!\[[^\]]*\]\(([^)#:\s]+)\)", readme)
rel = [p for p in paths if "://" not in p]
check(any("divider" in p for p in rel), "README uses the dividers")
for p in rel:
    check(os.path.exists(os.path.join(ROOT, p)), f"README image resolves: {p}")
sys.exit(1 if fails else 0)
