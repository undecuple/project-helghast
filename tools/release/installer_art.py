#!/usr/bin/env python3
"""Installer artwork and GitHub cover images, drawn from the project site's look (plan 4.9, 4.10).

Everything is drawn here from shapes, stripes and text: the plain two-line wordmark of names.WORDMARK set in Rajdhani Bold,
small text in Rajdhani (both SIL OFL 1.1, from Google Fonts' own repository github.com/google/fonts, ofl/; the
fonts are not committed or shipped, only these images, which NOTICE.md lists), the project URL, and the site's
palette (public/site/style.css). No game logo, symbol or game art. Needs Pillow (the .#re shell).

    python3 tools/release/installer_art.py --fetch-fonts   # download the three .ttf + OFL.txt into --fonts (network;
                                                          # refuses fonts whose SHA-256 differs from art.json's)
    python3 tools/release/installer_art.py                 # render installer/art/* + art.json (needs the fonts)
    python3 tools/release/installer_art.py --check         # exit 1 when an image is missing, changed or stale
    python3 tools/release/installer_art.py --preview F.png # also write a contact sheet of every image (not shipped)
    python3 tools/release/installer_art.py --stand-in-fonts --out DIR --public-out DIR2   # Pillow's built-in font
                                                     # instead (host test; never into installer/art or public/assets)

Outputs (installer/art/): wizard-{100,125,150,200}.bmp (Welcome/Finish panel, 24-bit), header-{...}.bmp (the small
top-right image), kz3-setup.ico, kz3-launcher.ico, kz3-uninstall.ico (16/24/32/48/64 as 32-bit DIB, 256 as PNG)
and art.json (SHA-256 of every file, of this script and of the fonts; Pillow and FreeType versions).
Outputs (public/assets/, exported to the public repo, art.json key "public_files"): social-preview.png (1280x640,
GitHub's repository social preview: under 1 MB, every mark inside a 40 px safe margin) and banner.png (1280x320) +
banner@2x.png (2560x640), the README banner: an opaque dark panel with transparent chamfers, so it reads the same on
GitHub's light and dark page. --public-out DIR moves them (tests); stand-in fonts never write into public/assets.
--check: with the fonts present it renders again and compares bytes; without them it checks the files against
art.json and that art.json was made by this exact script (an edited generator = stale). The palette must still be
in style.css (the site and the installer move together). Deterministic: same script + fonts + Pillow = same bytes.
ASSUMPTION: Inno 6's documented image sizes for 100/125/150/200 % (164x314 ... 328x604, 55x55 ... 110x106; the
help's WizardImageFile topic); Inno picks the closest one per DPI. Checked when the installer is compiled.
"""
import argparse
import hashlib
import io
import json
import pathlib
import re
import struct
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "installer" / "art"
# Both layouts: this repo keeps the public files under public/; the public repo has them at its root.
PUB = ROOT / "public" if (ROOT / "public" / "site").is_dir() else ROOT
PUBLIC_OUT = PUB / "assets"
FONTS = ROOT / "work" / "release" / "fonts"
CSS = PUB / "site" / "style.css"
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import names  # noqa: E402  (product name, wordmark, links: tools/release/names.py)
URL_TEXT = names.SITE_TEXT   # the site (live from the public flip)
TAG_TEXT = "UNOFFICIAL PC PORT"         # the wizard panel (164 px wide at 100 %: no room for the game name)
TAG_WIDE = names.TAG.upper()            # banner + social preview: "KILLZONE 3 // UNOFFICIAL PC PORT" (pass 21)
REPO_TEXT = names.REPO_URL.split("://", 1)[1]   # the banner's right column: github.com/OWNER/REPO_SLUG
WORDMARK = names.WORDMARK   # (white word, red word)
BLURB = ("BUILT ON YOUR PC", "FROM YOUR OWN GAME")

# Google Fonts' official repository (OFL folders). Pinned by SHA-256 in art.json after the first fetch.
GF = "https://raw.githubusercontent.com/google/fonts/main/ofl/"
FONT_FILES = {"head": ("rajdhani", "Rajdhani-Bold.ttf"),
              "small": ("rajdhani", "Rajdhani-Medium.ttf"),
              "small-bold": ("rajdhani", "Rajdhani-Bold.ttf")}

# The art's palette, which the site shares (public/site/style.css); --check fails when one of these leaves the
# stylesheet. red/red-hi/amber-hi/white are the ui-style.md tokens (game red, bright red, gold, text on red).
PAL = {"bg": "#0a0909", "line": "#4a1212", "red": "#d61c14", "red-hi": "#ff3b30", "text": "#e8dfdc",
       "dim": "#a59a97", "amber": "#d9a21b", "amber-hi": "#f0c04a", "glow": "#2a0707", "white": "#f0f0f0"}

# Inno Setup 6 sizes per DPI (ASSUMPTION, docstring): large wizard image, small header image.
WIZARD = {100: (164, 314), 125: (192, 386), 150: (246, 459), 200: (328, 604)}
HEADER = {100: (55, 55), 125: (64, 68), 150: (83, 80), 200: (110, 106)}
ICON_SIZES = (16, 24, 32, 48, 64, 256)
ICONS = ("setup", "launcher", "uninstall")
ICON_LABEL = "KZ3"   # shortcut icons only
SS = 4   # supersampling: draw at 4x, scale down once (anti-aliased shapes and text)
SOCIAL = (1280, 640)
SOCIAL_SAFE = 40            # GitHub crops the preview on some surfaces: every mark stays this far from the edge
SOCIAL_MAX_BYTES = 1 << 20  # GitHub refuses a social preview of 1 MB or more
BANNER = {"banner.png": (1280, 320), "banner@2x.png": (2560, 640)}
BANNER_MAX_BYTES = 400 * 1024   # "light enough for the README" (our own limit, per file)


def rgb(name):
    h = PAL[name].lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def script_hash():
    return sha256(pathlib.Path(__file__).read_bytes())


# ------------------------------------------------------------------------------------------------------- fonts
class Fonts:
    """The three faces; `stand_in` = Pillow's built-in scalable font (tests only)."""

    def __init__(self, fonts_dir, stand_in=False):
        from PIL import ImageFont
        self.IF, self.stand_in, self.paths, self.hashes = ImageFont, stand_in, {}, {}
        if stand_in:
            return
        for role, (_, fn) in FONT_FILES.items():
            p = pathlib.Path(fonts_dir) / fn
            if not p.is_file():
                raise FileNotFoundError(f"font {p} missing (run --fetch-fonts, or pass --fonts DIR)")
            self.paths[role], self.hashes[fn] = p, sha256(p.read_bytes())

    def get(self, role, px):
        px = max(1, int(px))
        if self.stand_in:
            return self.IF.load_default(px)
        return self.IF.truetype(str(self.paths[role]), px, layout_engine=self.IF.Layout.BASIC)


def missing_fonts(fonts_dir):
    return [fn for _, fn in FONT_FILES.values() if not (pathlib.Path(fonts_dir) / fn).is_file()]


def fetch_fonts(fonts_dir, pinned, opener=urllib.request.urlopen):
    """Download the fonts and their OFL.txt; refuse a font whose hash differs from the pinned one."""
    d = pathlib.Path(fonts_dir)
    d.mkdir(parents=True, exist_ok=True)
    got = {}
    for folder, fn in sorted(set(FONT_FILES.values())) + [("rajdhani", "OFL.txt")]:
        with opener(GF + f"{folder}/{fn}", timeout=30) as r:
            data = r.read()
        name = fn if fn != "OFL.txt" else f"OFL-{folder}.txt"
        if fn != "OFL.txt":
            h = sha256(data)
            if pinned.get(fn) and pinned[fn] != h:
                raise ValueError(f"{fn}: SHA-256 {h} differs from art.json's {pinned[fn]} (upstream changed it; "
                                 "check the change, then delete the pin from art.json to accept it)")
            got[fn] = h
        (d / name).write_bytes(data)
    return got


# ----------------------------------------------------------------------------------------------------- drawing
def text_w(font, text, track):
    """Width of `text` with `track` extra em per letter gap (the site's letter-spacing)."""
    if not text:
        return 0
    return sum(font.getlength(c) for c in text) + track * font.size * (len(text) - 1)


def draw_text(draw, xy, text, font, fill, track=0.0):
    """Letter by letter (tracking); y is the top of the caps (font ascender offset removed)."""
    x, y = xy
    top = font.getbbox("H")[1]
    for c in text:
        draw.text((x, y - top), c, font=font, fill=fill)
        x += font.getlength(c) + track * font.size


def fit(fonts, role, text, max_w, max_h, track=0.0):
    """The largest size whose caps fit max_w x max_h; -> (font, width, cap height)."""
    lo, hi = 1, max(2, int(max_h * 2))
    while lo < hi:
        mid = (lo + hi + 1) // 2
        f = fonts.get(role, mid)
        b = f.getbbox("H")
        if text_w(f, text, track) <= max_w and (b[3] - b[1]) <= max_h:
            lo = mid
        else:
            hi = mid - 1
    f = fonts.get(role, lo)
    b = f.getbbox("H")
    return f, text_w(f, text, track), b[3] - b[1]


def ground(w, h, glow_ry=0.6):
    """Dark ground with the site's red glow from the top (radial-gradient(ellipse at top, glow, bg 60%))."""
    from PIL import Image
    rx, ry = max(1, int(w * 0.9)), max(1, int(h * glow_ry))
    g = Image.radial_gradient("L").resize((2 * rx, 2 * ry), Image.Resampling.BILINEAR)
    mask = Image.new("L", (w, h), 255)
    mask.paste(g.crop((rx - w // 2, ry, rx - w // 2 + w, ry + min(h, ry))), (0, 0))   # below the ellipse: ground
    mask = mask.point(lambda v: min(255, v * 2))     # 128 = the ellipse's edge in radial_gradient
    return Image.composite(Image.new("RGB", (w, h), rgb("bg")), Image.new("RGB", (w, h), rgb("glow")), mask)


def scanlines(img, step=3, add=4):
    """The site's faint horizontal lines, at the final size (1 px every `step`)."""
    from PIL import Image, ImageChops
    from PIL import ImageDraw
    ov = Image.new("RGB", img.size, (0, 0, 0))
    d = ImageDraw.Draw(ov)
    for y in range(0, img.size[1], step):
        d.line((0, y, img.size[0] - 1, y), fill=(add, add, add))
    return ImageChops.add(img, ov)


def brackets(draw, x0, y0, x1, y1, length, thick, color):
    """HUD frame corners (.frame::before/::after): top-left and bottom-right."""
    draw.rectangle((x0, y0, x0 + length, y0 + thick - 1), fill=color)
    draw.rectangle((x0, y0, x0 + thick - 1, y0 + length), fill=color)
    draw.rectangle((x1 - length, y1 - thick + 1, x1, y1), fill=color)
    draw.rectangle((x1 - thick + 1, y1 - length, x1, y1), fill=color)


def hazard(draw, x0, y0, x1, y1, period, color):
    """Diagonal stripes in a band (drawn clipped: polygons, then the band's edges are repainted by the caller)."""
    hgt = y1 - y0
    x = x0 - hgt
    while x < x1:
        draw.polygon([(x, y1), (x + hgt, y0), (x + hgt + period / 2, y0), (x + period / 2, y1)], fill=color)
        x += period


def wizard_image(fonts, w, h):
    """The Welcome/Finish panel: tag, wordmark, blurb, stripes, URL, inside a HUD frame."""
    from PIL import Image, ImageDraw
    W, H = w * SS, h * SS
    u = W / 164.0                                   # 1 px at 100 %, in drawing units
    img = ground(W, H)
    d = ImageDraw.Draw(img)
    m = round(14 * u)
    d.rectangle((round(5 * u), round(5 * u), W - round(5 * u) - 1, H - round(5 * u) - 1),
                outline=rgb("line"), width=max(1, round(u)))
    brackets(d, round(4 * u), round(4 * u), W - round(4 * u) - 1, H - round(4 * u) - 1, round(22 * u),
             max(1, round(2 * u)), rgb("red-hi"))
    # tag (.tag: white on red, heading face; UI review 24)
    y = round(0.07 * H)
    f, tw, ch = fit(fonts, "head", TAG_TEXT, W - 2 * m - round(16 * u), round(9 * u), 0.06)
    d.rectangle((m, y, m + tw + round(16 * u), y + ch + round(10 * u)), fill=rgb("red"))
    draw_text(d, (m + round(8 * u), y + round(5 * u)), TAG_TEXT, f, rgb("white"), 0.06)
    # wordmark: names.WORDMARK, white word over red word
    y += ch + round(30 * u)
    _, wh = wordmark_block(d, fonts, m, y, W - 2 * m, round(44 * u))
    y += wh + round(18 * u)
    d.rectangle((m, y, W - m, y + max(1, round(2 * u)) - 1), fill=rgb("red"))
    # blurb (.note voice: short, direct)
    y += round(12 * u)
    for line in BLURB:
        f, tw, ch = fit(fonts, "small-bold", max(BLURB, key=len), W - 2 * m, round(12 * u), 0.08)
        draw_text(d, (m, y), line, f, rgb("text"), 0.08)
        y += ch + round(7 * u)
    # the emblem in the space left above the stripes (pasted after the downscale, like the social preview's)
    es = min(W - 2 * m, round(0.80 * H) - round(16 * u) - y - round(14 * u))
    ey = y + round(14 * u) + (round(0.80 * H) - round(16 * u) - y - round(14 * u) - es) // 2
    # stripes band, then the URL (Rajdhani, light text, two lines; UI review 23: it was small and grey)
    by0, by1 = round(0.80 * H), round(0.80 * H + 12 * u)
    band = Image.new("RGB", (W - 2 * m, by1 - by0), rgb("bg"))
    hazard(ImageDraw.Draw(band), 0, 0, band.size[0], band.size[1], round(14 * u), rgb("red"))
    img.paste(band, (m, by0))
    d.rectangle((m, by0 - round(4 * u), W - m, by0 - round(4 * u) + max(1, round(u)) - 1), fill=rgb("amber"))
    y = by1 + round(12 * u)
    lines = [URL_TEXT.split("/")[0], "/" + URL_TEXT.split("/", 1)[1]]
    f, tw, ch = fit(fonts, "small-bold", max(lines, key=len), W - 2 * m, round(12 * u))
    for line in lines:
        draw_text(d, (m, y), line, f, rgb("text"))
        y += ch + round(6 * u)
    out = scanlines(img.resize((w, h), Image.Resampling.LANCZOS))
    if es >= 32 * SS:
        em = tile(fonts, es // SS, es // SS, alpha=True)
        out.paste(em, (m // SS, ey // SS), em)
    return out


def tile(fonts, w, h, badge=None, alpha=False, bar=True, label=None):
    """Square-ish emblem used by the header image and the icons: no letters (a name-free mark, so a rename changes
    no icon): two stacked chevrons (red over light) above a short red bar, HUD corners. badge: None, "setup"
    (amber, down arrow) or "uninstall" (grey, minus). bar=False (social preview): no bar. label (the
    installer/uninstaller shortcut icons only): "KZ3" over a red PC bar instead of the chevrons, as the Killzone 2
    port's icons are laid out; the header images and every other logo stay name-free."""
    from PIL import Image, ImageDraw
    W, H = w * SS, h * SS
    s = min(W, H)
    img = ground(W, H, glow_ry=0.9).convert("RGBA")
    d = ImageDraw.Draw(img)
    t = max(SS, round(0.05 * s))
    if alpha:   # chamfered corners (.btn clip-path), transparent
        c = round(0.16 * s)
        d.polygon([(W - c, 0), (W, 0), (W, c)], fill=(0, 0, 0, 0))
        d.polygon([(0, H - c), (0, H), (c, H)], fill=(0, 0, 0, 0))
    else:
        d.rectangle((0, 0, W - 1, H - 1), outline=rgb("line"), width=max(1, SS))
    small = min(w, h) <= 24
    brackets(d, 0, 0, W - 1, H - 1, round(0.3 * s), t, rgb("red-hi"))
    mx = round(0.14 * W)
    # The bar ends before the badge's column; the launcher (no badge) uses the same short bar so the icons match.
    bsz = round((0.5 if small else 0.42) * s)            # badge square, bottom-right
    gap = max(SS, round(0.05 * s))
    bar_x1 = W - bsz - gap - 1
    if label:
        # Shortcut icons (the same layout as the Killzone 2 port's, in this port's colours): "KZ" light and the
        # number red over a red PC bar; small sizes keep the number alone over the bar; "PC" from 48 px (at 32 px
        # it is a 4 px smear, so the bar alone).
        mxl = round(0.12 * W)
        pad = round(0.04 * s)
        if small:
            bar0, bar1 = round(0.62 * H), round(0.86 * H)
            f, tw, cap = fit(fonts, "head", label[-1], W - 2 * mxl, round(0.42 * H))
            draw_text(d, (mxl, round(0.10 * H)), label[-1], f, rgb("text"))
        else:
            bar0, bar1 = round(0.64 * H), round(0.86 * H)
            f, tw, cap = fit(fonts, "head", label, W - 2 * mxl, round(0.32 * H), 0.04)
            x, y = mxl, round(0.18 * H)
            draw_text(d, (x, y), label[:-1], f, rgb("text"), 0.04)
            draw_text(d, (x + text_w(f, label[:-1], 0.04) + 0.04 * f.size, y), label[-1], f, rgb("red-hi"))
        d.rectangle((mxl, bar0, bar_x1, bar1), fill=rgb("red"))
        if not small and min(w, h) >= 48:
            f, tw, cap = fit(fonts, "head", "PC", bar_x1 - mxl - 2 * pad, round(0.6 * (bar1 - bar0)), 0.1)
            draw_text(d, (mxl + pad, bar0 + (bar1 - bar0 - cap) / 2), "PC", f, rgb("bg"), 0.1)
    else:
        # chevrons: an upward V of stroke `th`, the second one below the first (red, then light); one at small sizes
        th = round((0.15 if small else 0.11) * s)
        cw = W - 2 * mx
        ch = round(0.42 * cw)
        rows = (rgb("red"),) if small else (rgb("red"), rgb("text"))
        y = round((0.14 if small else 0.12) * H)
        for col in rows:
            cx = mx + cw / 2
            d.polygon([(mx, y + ch), (cx, y), (mx + cw, y + ch), (mx + cw - th * 1.25, y + ch), (cx, y + th * 1.55),
                       (mx + th * 1.25, y + ch)], fill=col)
            y += round(th * 1.6)
        if bar:
            bar0, bar1 = (round(0.70 * H), round(0.86 * H)) if small else (round(0.70 * H), round(0.84 * H))
            d.rectangle((mx, bar0, bar_x1, bar1), fill=rgb("red"))
    if badge:
        x0, y0 = W - bsz, H - bsz
        col = rgb("amber-hi") if badge == "setup" else rgb("dim")
        d.rectangle((x0, y0, W - 1, H - 1), fill=col)
        cx, cy, r = x0 + bsz / 2, y0 + bsz / 2, bsz * 0.32
        if badge == "setup":
            d.polygon([(cx - r, cy - r * 0.1), (cx + r, cy - r * 0.1), (cx, cy + r)], fill=rgb("bg"))
            d.rectangle((cx - r * 0.38, cy - r, cx + r * 0.38, cy - r * 0.1), fill=rgb("bg"))
        else:
            d.rectangle((cx - r, cy - r * 0.28, cx + r, cy + r * 0.28), fill=rgb("bg"))
    out = img.resize((w, h), Image.Resampling.LANCZOS)
    return out if alpha else out.convert("RGB")


def stripes(w, h, period, color, ground_col):
    """A band of diagonal stripes (the wizard's hazard band) as its own image, clipped to w x h."""
    from PIL import Image, ImageDraw
    band = Image.new("RGB", (w, h), ground_col)
    hazard(ImageDraw.Draw(band), 0, 0, w, h, period, color)
    return band


def finish_rgba(img, w, h, chamfer):
    """Scale an RGB drawing down to w x h, add the scanlines, cut the top-right and bottom-left chamfers
    (.btn clip-path) as transparent, anti-aliased."""
    from PIL import Image, ImageDraw
    W, H = img.size
    mask = Image.new("L", (W, H), 255)
    md = ImageDraw.Draw(mask)
    md.polygon([(W - chamfer, 0), (W, 0), (W, chamfer)], fill=0)
    md.polygon([(0, H - chamfer), (0, H), (chamfer, H)], fill=0)
    out = scanlines(img.resize((w, h), Image.Resampling.LANCZOS)).convert("RGBA")
    out.putalpha(mask.resize((w, h), Image.Resampling.LANCZOS))
    return out


def wordmark_line(d, fonts, x, y, cap, max_w):
    """WORDMARK on one line: first word white, second red, one cap height; -> (width, cap height)."""
    a, b = WORDMARK
    gap = round(0.32 * cap)
    fa = fit(fonts, "head", a, max_w, cap, 0.06)
    fb = fit(fonts, "head", b, max_w, cap, 0.06)
    if fa[1] + fb[1] + gap > max_w:    # shrink the whole line to the width
        return wordmark_line(d, fonts, x, y, int(cap * max_w / (fa[1] + fb[1] + gap)), max_w)
    draw_text(d, (x, y), a, fa[0], (255, 255, 255), 0.06)
    draw_text(d, (x + fa[1] + gap, y), b, fb[0], rgb("red"), 0.06)
    return fa[1] + gap + fb[1], max(fa[2], fb[2])


def wordmark_block(d, fonts, x, y, max_w, cap):
    """WORDMARK as two stacked lines of one cap height (the smaller of the two fits): white over red;
    -> (width, height)."""
    a, b = WORDMARK
    c = min(fit(fonts, "head", a, max_w, cap, 0.04)[2], fit(fonts, "head", b, max_w, cap, 0.04)[2])
    fa, wa, ca = fit(fonts, "head", a, max_w, c, 0.04)
    fb, wb, cb = fit(fonts, "head", b, max_w, c, 0.04)
    draw_text(d, (x, y), a, fa, (255, 255, 255), 0.04)
    y2 = y + ca + round(0.22 * cb)
    draw_text(d, (x, y2), b, fb, rgb("red"), 0.04)
    return max(wa, wb), y2 + cb - y


def tag(d, fonts, x, y, cap, max_w, text=TAG_WIDE, ink="bg"):
    """A filled red tag box: TAG_WIDE in dark ink on the banner and the social preview (pass 21, the sister
    port's "GAME // UNOFFICIAL PC PORT" form; 3.8:1, large bold text only); -> (width, height)."""
    f, tw, ch = fit(fonts, "head", text, max_w, cap, 0.06)
    padx, pady = round(0.9 * ch), round(0.55 * ch)
    d.rectangle((x, y, x + tw + 2 * padx, y + ch + 2 * pady), fill=rgb("red"))
    draw_text(d, (x + padx, y + pady), text, f, rgb(ink), 0.06)
    return tw + 2 * padx, ch + 2 * pady


def social_preview(fonts, w=SOCIAL[0], h=SOCIAL[1]):
    """GitHub's repository social preview: text column left, the launcher emblem right, all inside the safe
    margin (frame and brackets included). Opaque: GitHub shows it on cards of either theme."""
    from PIL import Image, ImageDraw
    ss = max(1, 5120 // w)
    W, H = w * ss, h * ss
    u = W / 1280.0
    img = ground(W, H, glow_ry=0.7)
    d = ImageDraw.Draw(img)
    s0 = round(SOCIAL_SAFE * u)                         # frame: the safe area's edge
    d.rectangle((s0 + round(6 * u), s0 + round(6 * u), W - s0 - round(6 * u) - 1, H - s0 - round(6 * u) - 1),
                outline=rgb("line"), width=max(1, round(2 * u)))
    brackets(d, s0, s0, W - s0 - 1, H - s0 - 1, round(90 * u), round(6 * u), rgb("red-hi"))
    m = round(88 * u)                                   # text column's left edge
    emb = round(300 * u)                                # emblem square, right
    ex, ey = W - m - emb, round(150 * u)
    col_w = ex - m - round(56 * u)
    y = round(92 * u)
    _, th = tag(d, fonts, m, y, round(22 * u), col_w)
    y += th + round(40 * u)
    # the wordmark takes what the fixed rows below it leave above the hairline (two lines + 0.22 gap)
    room = (H - s0 - round(112 * u) - round(18 * u)) - y - round(132 * u)
    ww, wh = wordmark_block(d, fonts, m, y, col_w, min(round(112 * u), int(room / 2.22)))
    y += wh + round(30 * u)
    fb, _, cb = fit(fonts, "small-bold", max(BLURB, key=len), col_w, round(26 * u), 0.08)
    for line in BLURB:
        draw_text(d, (m, y), line, fb, rgb("text"), 0.08)
        y += cb + round(14 * u)
    y += round(12 * u)
    # the rule spans the stripes' width (UI review 25: it stopped at the text column)
    d.rectangle((m, y, W - m, y + round(4 * u) - 1), fill=rgb("red"))
    # amber hairline, stripes band, URL (bottom of the safe area)
    y0 = H - s0 - round(112 * u)
    d.rectangle((m, y0 - round(8 * u), W - m, y0 - round(8 * u) + round(2 * u) - 1), fill=rgb("amber"))
    img.paste(stripes(W - 2 * m, round(22 * u), round(28 * u), rgb("red"), rgb("bg")), (m, y0))
    fu, _, cu = fit(fonts, "small", URL_TEXT, col_w, round(24 * u))
    draw_text(d, (m, y0 + round(22 * u) + round(26 * u)), URL_TEXT, fu, rgb("dim"))
    out = scanlines(img.resize((w, h), Image.Resampling.LANCZOS))
    # emblem without its bar (UI review 25)
    em = tile(fonts, emb // ss, emb // ss, alpha=True, bar=False)
    out.paste(em, (ex // ss, ey // ss), em)
    return out


def banner(fonts, w, h):
    """The README banner: tag + one-line wordmark left, blurb + repository URL right, stripes along the bottom, HUD frame;
    an opaque dark panel (legible on GitHub's light and dark pages) with transparent chamfers."""
    from PIL import ImageDraw
    ss = max(1, 5120 // w)
    W, H = w * ss, h * ss
    u = H / 320.0
    img = ground(W, H, glow_ry=1.2)
    d = ImageDraw.Draw(img)
    ins = round(8 * u)
    d.rectangle((ins, ins, W - ins - 1, H - ins - 1), outline=rgb("line"), width=max(1, round(2 * u)))
    brackets(d, round(4 * u), round(4 * u), W - round(4 * u) - 1, H - round(4 * u) - 1, round(64 * u),
             round(5 * u), rgb("red-hi"))
    m = round(56 * u)
    right_w = round(360 * u)
    y = round(48 * u)
    tag(d, fonts, m, y, round(18 * u), W - 2 * m)
    y = round(104 * u)
    ww, ch = wordmark_line(d, fonts, m, y, round(100 * u), W - 2 * m - right_w - round(40 * u))
    y += ch + round(26 * u)
    d.rectangle((m, y, m + ww, y + round(4 * u) - 1), fill=rgb("red"))
    # right column: blurb (bright) over the URL (dim), right-aligned to the frame
    rx1 = W - m
    fb, _, cb = fit(fonts, "small-bold", max(BLURB, key=len), right_w, round(22 * u), 0.08)
    by = round(116 * u)
    for line in BLURB:
        draw_text(d, (rx1 - text_w(fb, line, 0.08), by), line, fb, rgb("text"), 0.08)
        by += cb + round(12 * u)
    fu, uw, cu = fit(fonts, "small", REPO_TEXT, right_w, round(20 * u))
    draw_text(d, (rx1 - uw, by + round(10 * u)), REPO_TEXT, fu, rgb("dim"))
    # stripes along the bottom with the amber hairline over them
    sy = H - round(56 * u)
    d.rectangle((m, sy - round(8 * u), W - m, sy - round(8 * u) + round(2 * u) - 1), fill=rgb("amber"))
    img.paste(stripes(W - 2 * m, round(16 * u), round(24 * u), rgb("red"), rgb("bg")), (m, sy))
    return finish_rgba(img, w, h, round(26 * u))


# --------------------------------------------------------------------------------------------------- encoding
def bmp_bytes(img):
    buf = io.BytesIO()
    img.convert("RGB").save(buf, format="BMP")
    return buf.getvalue()


def png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=False, compress_level=9)
    return buf.getvalue()


def dib(img):
    """One ICO entry as a 32-bit BI_RGB DIB: header with double height, BGRA rows bottom-up, then the AND mask."""
    w, h = img.size
    px = img.convert("RGBA").tobytes()
    rows = []
    for y in range(h - 1, -1, -1):
        r = px[y * w * 4:(y + 1) * w * 4]
        rows.append(b"".join(bytes((r[i + 2], r[i + 1], r[i], r[i + 3])) for i in range(0, len(r), 4)))
    stride = ((w + 31) // 32) * 4
    mask = []
    for y in range(h - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(w):
            if px[(y * w + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask.append(bytes(bits))
    body = b"".join(rows) + b"".join(mask)
    return struct.pack("<IiiHHIIiiII", 40, w, 2 * h, 1, 32, 0, len(body), 0, 0, 0, 0) + body


def ico_bytes(images):
    """ICONDIR + entries; 256 px as PNG (Vista+), the rest as DIBs (every Windows reader takes those)."""
    blobs = [png_bytes(im) if im.size[0] >= 256 else dib(im) for im in images]
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    for im, b in zip(images, blobs):
        w, h = im.size
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(b), off)
        off += len(b)
    return out + b"".join(blobs)


def parse_ico(data):
    """-> [(w, h, kind)] for the host test and --check."""
    _, typ, n = struct.unpack_from("<HHH", data, 0)
    if typ != 1:
        raise ValueError("not an icon")
    out = []
    for i in range(n):
        w, h, _, _, _, bpp, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        kind = "png" if data[off:off + 8] == b"\x89PNG\r\n\x1a\n" else "dib"
        out.append((w or 256, h or 256, kind))
    return out


# ------------------------------------------------------------------------------------------------------- render
def render(fonts):
    """-> {file name: (bytes, (w, h))}, every output but art.json."""
    out = {}
    for pct, (w, h) in WIZARD.items():
        out[f"wizard-{pct}.bmp"] = (bmp_bytes(wizard_image(fonts, w, h)), (w, h))
    for pct, (w, h) in HEADER.items():
        out[f"header-{pct}.bmp"] = (bmp_bytes(tile(fonts, w, h)), (w, h))
    for kind in ICONS:
        badge = None if kind == "launcher" else kind
        imgs = [tile(fonts, s, s, badge=badge, alpha=True, label=ICON_LABEL) for s in ICON_SIZES]
        out[f"kz3-{kind}.ico"] = (ico_bytes(imgs), (256, 256))
    return out


def render_public(fonts):
    """-> {file name: (bytes, (w, h))} for public/assets/."""
    out = {"social-preview.png": (png_bytes(social_preview(fonts)), SOCIAL)}
    for name, (w, h) in BANNER.items():
        out[name] = (png_bytes(banner(fonts, w, h)), (w, h))
    return out


def versions():
    import PIL
    from PIL import features
    return {"pillow": PIL.__version__, "freetype": features.version("freetype2") or "-"}


def manifest(files, fonts, public=None):
    def ent(fs):
        return {n: {"size": list(sz), "bytes": len(b), "sha256": sha256(b)} for n, (b, sz) in sorted(fs.items())}
    return {"format": 1, "generator_sha256": script_hash(), "fonts_sha256": dict(sorted(fonts.hashes.items())),
            "fonts_source": GF, **versions(), "files": ent(files), "public_files": ent(public or {})}


def size_problems(public):
    """The limits GitHub and the README set: the social preview under 1 MB, banners under BANNER_MAX_BYTES."""
    probs = []
    for name, (data, _) in public.items():
        lim = SOCIAL_MAX_BYTES if name == "social-preview.png" else BANNER_MAX_BYTES
        if len(data) >= lim:
            probs.append(f"{name} is {len(data)} bytes, the limit is under {lim}")
    return probs


def palette_problems(css_path):
    try:
        css = pathlib.Path(css_path).read_text(encoding="utf-8").lower()
    except OSError:
        return [f"{css_path} not readable (the palette's source)"]
    return [f"palette {k} {v} is not in {pathlib.Path(css_path).name} any more" for k, v in PAL.items()
            if not re.search(re.escape(v.lower()) + r"\b", css)]


def contact_sheet(files, path, stand_in=False):
    """Every image at 1:1 on the wizard's white, then the 16/24/32 icons enlarged 6x (nearest) to judge them."""
    from PIL import Image, ImageDraw
    ims, zoom = [], []
    for name, (data, _) in sorted(files.items()):
        im = Image.open(io.BytesIO(data))
        if name.endswith(".ico"):
            sizes = sorted(im.info.get("sizes", {(256, 256)}))
            for s in sizes:
                im.size = s
                ims.append(im.copy().convert("RGBA"))
                if s[0] <= 32:
                    zoom.append(ims[-1].resize((s[0] * 6, s[1] * 6), Image.Resampling.NEAREST))
        else:
            ims.append(im.convert("RGBA"))
    row1 = max(i.size[1] for i in ims) + 20
    W = max(sum(i.size[0] + 10 for i in r) for r in (ims, zoom)) + 10
    H = row1 + max(i.size[1] for i in zoom) + 40
    sheet = Image.new("RGBA", (W, H), (255, 255, 255, 255))   # the wizard's white page around the images
    x = 10
    for i in ims:
        sheet.alpha_composite(i, (x, 10))
        x += i.size[0] + 10
    x = 10
    for i in zoom:
        sheet.alpha_composite(i, (x, row1))
        x += i.size[0] + 10
    note = ("STAND-IN FONT (Pillow built-in): NOT Rajdhani; layout preview only" if stand_in
            else "font: Rajdhani (OFL)")
    ImageDraw.Draw(sheet).text((10, H - 22), note, fill=(200, 22, 29, 255) if stand_in else (0, 0, 0, 255))
    sheet.save(path)


def check(out_dir, fonts_dir, css_path, public_dir=PUBLIC_OUT):
    """-> list of problems."""
    probs = palette_problems(css_path)
    mpath = pathlib.Path(out_dir) / "art.json"
    if not mpath.is_file():
        return probs + [f"{mpath} missing (run tools/release/installer_art.py)"]
    man = json.loads(mpath.read_text(encoding="utf-8"))
    if man.get("generator_sha256") != script_hash():
        probs.append("art.json was made by another version of installer_art.py: render again")
    for key, d in (("files", out_dir), ("public_files", public_dir)):
        for name, meta in man.get(key, {}).items():
            p = pathlib.Path(d) / name
            if not p.is_file():
                probs.append(f"{name} missing")
            elif sha256(p.read_bytes()) != meta["sha256"]:
                probs.append(f"{name} differs from art.json (edited by hand? render again)")
            else:
                lim = SOCIAL_MAX_BYTES if name == "social-preview.png" else BANNER_MAX_BYTES
                if key == "public_files" and meta["bytes"] >= lim:
                    probs.append(f"{name} is {meta['bytes']} bytes, the limit is under {lim}")
    want = {f"wizard-{p}.bmp" for p in WIZARD} | {f"header-{p}.bmp" for p in HEADER} | {f"kz3-{k}.ico" for k in ICONS}
    if set(man.get("files", {})) != want:
        probs.append(f"art.json lists {sorted(man.get('files', {}))}, the generator makes {sorted(want)}")
    want_pub = {"social-preview.png"} | set(BANNER)
    if set(man.get("public_files", {})) != want_pub:
        probs.append(f"art.json lists public {sorted(man.get('public_files', {}))}, the generator makes "
                     f"{sorted(want_pub)}")
    if not missing_fonts(fonts_dir) and not probs:
        fonts = Fonts(fonts_dir)
        if fonts.hashes != man.get("fonts_sha256"):
            probs.append("the fonts in --fonts differ from the ones art.json was made with")
        elif versions() != {k: man.get(k) for k in ("pillow", "freetype")}:
            print(f"installer_art: note: Pillow/FreeType {versions()} differ from art.json's; byte compare skipped")
        else:
            fresh = {**{("files", n): v for n, v in render(fonts).items()},
                     **{("public_files", n): v for n, v in render_public(fonts).items()}}
            for (key, name), (data, _) in fresh.items():
                if sha256(data) != man[key][name]["sha256"]:
                    probs.append(f"{name}: a fresh render differs (non-deterministic or stale)")
    return probs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--public-out", default=str(PUBLIC_OUT))
    ap.add_argument("--fonts", default=str(FONTS))
    ap.add_argument("--css", default=str(CSS))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fetch-fonts", action="store_true")
    ap.add_argument("--stand-in-fonts", action="store_true")
    ap.add_argument("--preview", default="")
    a = ap.parse_args(argv)
    out, pub = pathlib.Path(a.out), pathlib.Path(a.public_out)
    mpath = out / "art.json"
    pinned = json.loads(mpath.read_text(encoding="utf-8")).get("fonts_sha256", {}) if mpath.is_file() else {}
    if a.fetch_fonts:
        try:
            got = fetch_fonts(a.fonts, pinned)
        except (OSError, ValueError) as e:
            print(f"installer_art: fetch failed: {e}", file=sys.stderr)
            return 1
        for fn, h in sorted(got.items()):
            print(f"installer_art: {fn} {h}")
        return 0
    if a.check:
        try:
            import PIL  # noqa: F401
        except ImportError:
            print("installer_art: SKIP --check (no Pillow; run it in the .#re shell)")
            return 0
        probs = check(out, a.fonts, a.css, pub)
        for p in probs:
            print("installer_art: " + p, file=sys.stderr)
        if not probs:
            how = "files match art.json" + ("" if missing_fonts(a.fonts) else ", fresh render identical")
            print(f"installer_art: OK ({how})")
        return 1 if probs else 0
    if a.stand_in_fonts and (out.resolve() == OUT.resolve() or pub.resolve() == PUBLIC_OUT.resolve()):
        print("installer_art: --stand-in-fonts never writes into installer/art or public/assets (pass --out and "
              "--public-out)", file=sys.stderr)
        return 2
    probs = palette_problems(a.css)
    if probs:
        print("installer_art: " + "; ".join(probs), file=sys.stderr)
        return 1
    try:
        fonts = Fonts(a.fonts, a.stand_in_fonts)
    except FileNotFoundError as e:
        print(f"installer_art: {e}", file=sys.stderr)
        return 1
    if pinned and not a.stand_in_fonts and fonts.hashes != pinned:
        print("installer_art: the fonts differ from art.json's pins (fetch again or remove the pins)", file=sys.stderr)
        return 1
    files, public = render(fonts), render_public(fonts)
    probs = size_problems(public)
    if probs:
        print("installer_art: " + "; ".join(probs), file=sys.stderr)
        return 1
    for d, fs in ((out, files), (pub, public)):
        d.mkdir(parents=True, exist_ok=True)
        for name, (data, _) in fs.items():
            (d / name).write_bytes(data)
    mpath.write_text(json.dumps(manifest(files, fonts, public), indent=1) + "\n", encoding="utf-8", newline="\n")
    if a.preview:
        contact_sheet(files, a.preview, a.stand_in_fonts)
    print(f"installer_art: wrote {len(files)} files + art.json to {out}, {len(public)} to {pub} "
          f"({', '.join(f'{n} {len(b)} B' for n, (b, _) in sorted(public.items()))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
