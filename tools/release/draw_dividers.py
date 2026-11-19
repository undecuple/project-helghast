#!/usr/bin/env python3
"""Draw the README divider images (original art, no game material) with a pure-Python PNG writer.

Three transparent 900-px strips in the site's vocabulary (palette red d61c14, amber d9a21b, chevron emblem of
public/site/index.html, HUD corner brackets). Colours are mid-tone so they read on GitHub's light and dark themes.
Output is deterministic (fixed supersampling, zlib level 9, no timestamps).

  draw_dividers.py            write the PNGs to public/docs/assets/
  draw_dividers.py --check    exit 1 if a file on disk differs from a fresh render or is over 20 KB
  draw_dividers.py --out DIR  write elsewhere
"""
import argparse, os, struct, sys, zlib

W = 900
SS = 4                      # supersampling per axis
MAX_BYTES = 20 * 1024
RED, RED_HI, AMBER = (214, 28, 20), (255, 59, 48), (217, 162, 27)
DIM = (150, 52, 48)         # dim red for the long thin rule
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SITE = os.path.join(ROOT, "public") if os.path.isdir(os.path.join(ROOT, "public")) else ROOT   # repo or public export
DEFAULT_OUT = os.path.join(SITE, "docs", "assets")

E1 = [(0, 42), (50, 0), (100, 42), (72, 42), (50, 23.5), (28, 42)]      # the site's two-polygon emblem
E2 = [(0, 70), (50, 28), (100, 70), (72, 70), (50, 51.5), (28, 70)]


def in_poly(x, y, pts):
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]; x2, y2 = pts[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


def emblem(cx, cy, h):
    """Shape fn for the emblem of overall height h (viewBox 100x70) centred at cx, cy."""
    s = h / 70.0
    ox, oy = cx - 50 * s, cy - 35 * s
    def f(x, y):
        u, v = (x - ox) / s, (y - oy) / s
        if in_poly(u, v, E1): return RED
        if in_poly(u, v, E2): return RED_HI
        return None
    return f


def render(h, shapes):
    """shapes: list of fn(x, y) -> colour or None, later ones on top. Returns RGBA rows (bytes)."""
    rows = []
    n = SS * SS
    for py in range(h):
        row = bytearray()
        for px in range(W):
            acc = [0, 0, 0]; cov = 0
            for sy in range(SS):
                for sx in range(SS):
                    x, y = px + (sx + .5) / SS, py + (sy + .5) / SS
                    c = None
                    for fn in shapes:
                        r = fn(x, y)
                        if r is not None: c = r
                    if c is not None:
                        cov += 1
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
            if cov:
                row += bytes((acc[0] // cov, acc[1] // cov, acc[2] // cov, cov * 255 // n))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(h, rows):
    raw = b"".join(b"\0" + r for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rect(x0, y0, x1, y1, col):
    return lambda x, y: col if x0 <= x < x1 and y0 <= y < y1 else None


def brackets(h, inset, arm, t, col):
    """HUD corner brackets at both ends: each a vertical and a horizontal arm."""
    out = []
    for xl in (inset, W - inset):
        sgn = 1 if xl == inset else -1
        xa, xb = sorted((xl, xl + sgn * t))
        out.append(rect(xa, 0, xb, h, col))
        for y0 in (0, h - t):
            xa, xb = sorted((xl, xl + sgn * arm))
            out.append(rect(xa, y0, xb, y0 + t, col))
    return out


def rule():
    h, cy = 28, 14.0
    gap = 46
    def line(x, y):                     # long thin rule, fades toward the ends (alpha via blend with none is
        if abs(y - cy) > 0.75: return None   # not available per-shape, so it simply stops short of the emblem)
        if 60 <= x <= 450 - gap or 450 + gap <= x <= W - 60: return DIM
        return None
    def core(x, y):                     # brighter red section near the emblem
        if abs(y - cy) > 1.25: return None
        if 450 - gap - 120 <= x <= 450 - gap or 450 + gap <= x <= 450 + gap + 120: return RED
        return None
    return h, [line, core, emblem(450, cy, 22)]


def hazard():
    h = 14
    x0, x1 = 330, 570
    def band(x, y):
        if not (x0 <= x < x1): return None
        k = int((x + y * 1.0 - x0) // 10) % 2   # 45 degree stripes, 10 px
        return AMBER if k == 0 else RED
    # slanted ends: clip a 45 degree chamfer of 3 px at each end
    def clip(x, y):
        c = band(x, y)
        if c is None: return None
        if x - x0 < 0 or x1 - x < 0: return None
        return c
    return h, [clip]


def framed():
    h = 28
    def line(x, y):
        if abs(y - h / 2) <= 0.5 and 40 <= x <= W - 40: return DIM
        return None
    def ticks(x, y):                    # three small amber ticks in the middle, like the HUD readouts
        for cx in (438, 450, 462):
            if abs(x - cx) <= 2 and abs(y - h / 2) <= 4: return AMBER
        return None
    return h, [line] + brackets(h, 10, 22, 2, RED) + [ticks]


IMAGES = {"divider-rule.png": rule, "divider-hazard.png": hazard, "divider-frame.png": framed}


def build():
    out = {}
    for name, fn in IMAGES.items():
        h, shapes = fn()
        out[name] = png(h, render(h, shapes))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args(argv)
    imgs = build()
    bad = []
    for name, data in imgs.items():
        p = os.path.join(a.out, name)
        if len(data) > MAX_BYTES: bad.append(f"{name}: {len(data)} bytes over {MAX_BYTES}")
        if a.check:
            if not os.path.exists(p) or open(p, "rb").read() != data: bad.append(f"{name}: differs from a fresh render")
        else:
            os.makedirs(a.out, exist_ok=True)
            open(p, "wb").write(data)
            print(f"wrote {p} ({len(data)} bytes)")
    for b in bad: print("FAIL", b, file=sys.stderr)
    if a.check and not bad: print("dividers ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
