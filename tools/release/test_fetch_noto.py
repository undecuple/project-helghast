#!/usr/bin/env python3
"""Host test for tools/release/fetch_noto.py (no network: the opener is a stub): the pin is enforced, nothing is
written on a mismatch, an empty pin returns the hash, only the two pinned URLs are asked for. Also: the committed
fonts-extra.json names Noto Sans Regular from notofonts (SIL OFL) and the runtime's file name."""
import hashlib
import io
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_noto as fn  # noqa: E402

FAILS = []


def check(c, w):
    print(("ok   " if c else "FAIL ") + w)
    if not c:
        FAILS.append(w)


class Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def main():
    with tempfile.TemporaryDirectory() as t:
        pins = os.path.join(t, "p.json")
        seen = []

        def opener(url, timeout):
            seen.append(url)
            return Resp(b"FONT" if url.endswith(".ttf") else b"LICENCE")
        h = hashlib.sha256(b"FONT").hexdigest()

        def write_pin(v):
            json.dump({"sources": {fn.FONT_FILE: "https://x/a.ttf", fn.LICENSE_FILE: "https://x/OFL.txt"},
                       "sha256": {fn.FONT_FILE: v}}, open(pins, "w"))
        out = os.path.join(t, "f")
        write_pin("")
        check(fn.fetch(out, pins, opener) == h and open(os.path.join(out, fn.FONT_FILE), "rb").read() == b"FONT"
              and open(os.path.join(out, fn.LICENSE_FILE), "rb").read() == b"LICENCE", "empty pin: hash returned, files written")
        check(seen == ["https://x/a.ttf", "https://x/OFL.txt"], "only the two pinned URLs are fetched")
        write_pin(h)
        check(fn.fetch(out, pins, opener) == h, "matching pin accepted")
        out2 = os.path.join(t, "g")
        write_pin("0" * 64)
        try:
            fn.fetch(out2, pins, opener)
            check(False, "a font that differs from the pin is refused")
        except ValueError:
            check(not os.path.exists(out2), "a font that differs from the pin is refused and nothing is written")
        j = json.load(open(fn.PINS))
        check(j["sources"][fn.FONT_FILE].startswith("https://github.com/notofonts/") and fn.FONT_FILE == "NotoSans-Regular.ttf",
              "committed sources: notofonts, the file name the runtime loads (patch 5005)")
    print("FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
