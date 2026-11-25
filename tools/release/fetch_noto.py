#!/usr/bin/env python3
"""Fetch Noto Sans Regular (SIL OFL 1.1) and its OFL text into work/release/fonts (the folder installer_art.py
--fetch-fonts fills with Rajdhani), for the payload (assemble_payload.py): the Cyrillic/Greek fallback of the game's
cellFont chain (patch 5005 loads NotoSans-Regular.ttf from <exe dir>/fonts; issue #60). The font is checked against the
SHA-256 in installer/art/fonts-extra.json; with an empty pin the hash is printed so it can be pinned (first fetch).
Separate from installer_art.py because that script's own hash is recorded in art.json.

    python3 tools/release/fetch_noto.py [--fonts DIR]      (needs network; the font is never committed)
"""
import argparse
import hashlib
import json
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
PINS = ROOT / "installer" / "art" / "fonts-extra.json"
FONT_FILE, LICENSE_FILE = "NotoSans-Regular.ttf", "OFL-notosans.txt"


def sources(pins_path=PINS):
    j = json.loads(pathlib.Path(pins_path).read_text(encoding="utf-8"))
    return j["sources"][FONT_FILE], j["sources"][LICENSE_FILE], j.get("sha256", {}).get(FONT_FILE, "")


def fetch(fonts_dir, pins_path=PINS, opener=urllib.request.urlopen):
    """-> the font's SHA-256. Writes nothing when the hash differs from a non-empty pin."""
    font_url, lic_url, pin = sources(pins_path)
    with opener(font_url, timeout=60) as r:
        font = r.read()
    h = hashlib.sha256(font).hexdigest()
    if pin and pin != h:
        raise ValueError(f"{FONT_FILE}: SHA-256 {h} differs from the pin {pin} in installer/art/fonts-extra.json "
                         "(upstream changed it; check the change, then update the pin)")
    with opener(lic_url, timeout=60) as r:
        lic = r.read()
    d = pathlib.Path(fonts_dir)
    d.mkdir(parents=True, exist_ok=True)
    (d / FONT_FILE).write_bytes(font)
    (d / LICENSE_FILE).write_bytes(lic)
    return h


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fonts", default=str(ROOT / "work" / "release" / "fonts"))
    a = ap.parse_args(argv)
    try:
        h = fetch(a.fonts)
    except (OSError, ValueError, KeyError) as e:
        print(f"fetch_noto: failed: {e}", file=sys.stderr)
        return 1
    print(f"fetch_noto: {FONT_FILE} {h}" + ("" if sources()[2] else "  (no pin yet: put it in installer/art/fonts-extra.json)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
