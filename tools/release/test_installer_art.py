#!/usr/bin/env python3
"""Host test for tools/release/installer_art.py (no game data, no work/, no network): renders with Pillow's built-in
font into a temp folder, then checks the image sizes Inno expects, the icon sizes and entry kinds, the site palette
in the pixels, determinism (two renders, same bytes), --check (passes fresh; fails on an edited image, a manifest
from another generator, a palette colour gone from the stylesheet, a missing file), that stand-in fonts never write
into installer/art, and --fetch-fonts' pin check with a fake opener. The GitHub cover images (public/assets): social
preview 1280x640 opaque, under 1 MB, nothing but ground outside the 40 px safe margin; banner 1280x320 + 2560x640
RGBA with transparent chamfers and an opaque body, under the banner limit; size limits enforced; --check covers them. SKIP without Pillow. Exit 0 = PASS.

    python3 tools/release/test_installer_art.py
"""
import contextlib
import io
import json
import os
import pathlib
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import installer_art as art  # noqa: E402

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def quiet(fn, *a):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        rc = fn(*a)
    return rc, buf.getvalue()


def near(c, ref, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def main():
    try:
        from PIL import Image
    except ImportError:
        print("test_installer_art: SKIP (no Pillow; run in the .#re shell)")
        return 0
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="kz3-art-"))
    try:
        out, pub, nofonts, css = tmp / "art", tmp / "pub", tmp / "fonts", tmp / "style.css"
        nofonts.mkdir()
        css.write_text(" ".join(art.PAL.values()) + "\n", encoding="utf-8")
        base = ["--stand-in-fonts", "--fonts", str(nofonts), "--css", str(css), "--out", str(out), "--public-out", str(pub)]
        rc, _ = quiet(art.main, base)
        check(rc == 0, "render with stand-in fonts")
        names = sorted(p.name for p in out.iterdir())
        check(len(names) == 12 and "art.json" in names, f"12 outputs {names}")
        for pct, size in art.WIZARD.items():
            im = Image.open(out / f"wizard-{pct}.bmp")
            check(im.size == size and im.mode == "RGB", f"wizard-{pct} {im.size} {im.mode}")
        for pct, size in art.HEADER.items():
            im = Image.open(out / f"header-{pct}.bmp")
            check(im.size == size, f"header-{pct} {im.size}")
        check((art.WIZARD[100], art.HEADER[100]) == ((164, 314), (55, 55)), "Inno 100 % sizes")
        for k in art.ICONS:
            ents = art.parse_ico((out / f"kz3-{k}.ico").read_bytes())
            check([e[0] for e in ents] == list(art.ICON_SIZES) and [e[2] for e in ents] == ["dib"] * 5 + ["png"],
                  f"{k}.ico entries {ents}")
            ico = Image.open(out / f"kz3-{k}.ico")
            check(set(ico.info.get("sizes", ())) >= {(16, 16), (256, 256)}, f"{k}.ico readable by Pillow")
        # the palette shows up: ground low in the panel, the accent (tag, wordmark, brackets) and the warning amber
        im = Image.open(out / "wizard-200.bmp").convert("RGB")
        cols = {c for _, c in im.getcolors(1 << 20)}
        check(near(im.getpixel((im.size[0] // 2, int(im.size[1] * 0.72))), art.rgb("bg"), 12), "ground colour low in the panel")
        for name in ("red", "red-hi", "amber"):
            check(any(near(c, art.rgb(name), 12) for c in cols), f"wizard has {name} {art.PAL[name]}")
        ico = Image.open(out / "kz3-launcher.ico")
        ico.size = (256, 256)
        px = ico.convert("RGBA")
        check(px.getpixel((255, 0))[3] == 0 and px.getpixel((128, 128))[3] == 255, "icon chamfer transparent, body opaque")
        # the GitHub cover images
        check(sorted(p.name for p in pub.iterdir()) == ["banner.png", "banner@2x.png", "social-preview.png"],
              "public/assets: social preview + banner 1x/2x")
        sp = Image.open(pub / "social-preview.png")
        check(sp.size == (1280, 640) and sp.mode == "RGB", f"social preview {sp.size} {sp.mode} (opaque)")
        nbytes = (pub / "social-preview.png").stat().st_size
        check(nbytes < art.SOCIAL_MAX_BYTES == 1 << 20, f"social preview under GitHub's 1 MB ({nbytes} B)")
        M, (W, H) = art.SOCIAL_SAFE, sp.size
        px = sp.load()
        stray = [(x, y) for y in range(H) for x in range(W)
                 if (x < M or y < M or x >= W - M or y >= H - M) and (px[x, y][0] > 60 or px[x, y][1] > 20)]   # the ground: bg #0a0909 under the red glow #2a0707
        check(M == 40 and not stray, f"nothing but the dark ground outside the 40 px safe margin {stray[:3]}")
        inside = {px[x, y] for y in range(M, H - M, 2) for x in range(M, W - M, 2)}
        for name in ("red", "red-hi", "amber", "text"):
            check(any(near(c, art.rgb(name), 12) for c in inside), f"social preview has {name}")
        for name, size in art.BANNER.items():
            b = Image.open(pub / name)
            nb = (pub / name).stat().st_size
            check(b.size == size and b.mode == "RGBA", f"{name} {b.size} {b.mode}")
            check(nb < art.BANNER_MAX_BYTES, f"{name} light ({nb} B < {art.BANNER_MAX_BYTES})")
            check(b.getpixel((size[0] - 1, 0))[3] == 0 and b.getpixel((0, size[1] - 1))[3] == 0
                  and b.getpixel((size[0] // 2, size[1] // 2))[3] == 255
                  and min(a for _, _, _, a in [b.getpixel((x, size[1] // 2)) for x in range(size[0])]) == 255,
                  f"{name}: chamfers transparent, body opaque (same on light and dark pages)")
        check(art.BANNER["banner@2x.png"] == tuple(2 * v for v in art.BANNER["banner.png"]), "banner@2x is 2x")
        big = {"social-preview.png": (b"x" * art.SOCIAL_MAX_BYTES, (1, 1)), "banner.png": (b"x" * 10, (1, 1)),
               "banner@2x.png": (b"x" * art.BANNER_MAX_BYTES, (1, 1))}
        sz = art.size_problems(big)
        check(len(sz) == 2 and "social-preview.png" in sz[0] and "banner@2x.png" in " ".join(sz),
              f"size limits refuse 1 MB previews and heavy banners {sz}")
        # determinism
        def snap():
            return {p.relative_to(tmp).as_posix(): p.read_bytes() for d in (out, pub) for p in d.iterdir()}
        first = snap()
        first = {k.split("/", 1)[1]: v for k, v in first.items()}
        rc, _ = quiet(art.main, base)
        check(rc == 0 and first == {k.split("/", 1)[1]: v for k, v in snap().items()}, "same bytes on a re-run")
        # --check
        chk = ["--check", "--fonts", str(nofonts), "--css", str(css), "--out", str(out), "--public-out", str(pub)]
        rc, msg = quiet(art.main, chk)
        check(rc == 0 and "OK" in msg, f"--check fresh passes {msg.strip()}")
        p = out / "header-150.bmp"
        b = bytearray(p.read_bytes())
        b[-1] ^= 0xFF
        p.write_bytes(bytes(b))
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "header-150.bmp differs" in msg, "--check catches an edited image")
        p.write_bytes(first["header-150.bmp"])
        m = json.loads((out / "art.json").read_text(encoding="utf-8"))
        m["generator_sha256"] = "0" * 64
        (out / "art.json").write_text(json.dumps(m), encoding="utf-8")
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "another version" in msg, "--check catches a stale generator")
        (out / "art.json").write_bytes(first["art.json"])
        (out / "kz3-setup.ico").unlink()
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "kz3-setup.ico missing" in msg, "--check catches a missing icon")
        (out / "kz3-setup.ico").write_bytes(first["kz3-setup.ico"])
        p = pub / "banner.png"
        p.write_bytes(first["banner.png"][:-1] + bytes([first["banner.png"][-1] ^ 0xFF]))
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "banner.png differs" in msg, "--check catches an edited banner")
        p.unlink()
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "banner.png missing" in msg, "--check catches a missing banner")
        p.write_bytes(first["banner.png"])
        m = json.loads(first["art.json"])
        m["public_files"]["social-preview.png"]["bytes"] = art.SOCIAL_MAX_BYTES
        (out / "art.json").write_text(json.dumps(m), encoding="utf-8")
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "social-preview.png is" in msg, "--check catches a preview over GitHub's limit")
        del m["public_files"]["banner@2x.png"]
        m["public_files"]["social-preview.png"]["bytes"] = 1
        (out / "art.json").write_text(json.dumps(m), encoding="utf-8")
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "lists public" in msg, "--check catches a cover image left out of art.json")
        (out / "art.json").write_bytes(first["art.json"])
        rc, msg = quiet(art.main, chk)
        check(rc == 0, f"--check passes again after the restores {msg.strip()[:120]}")
        css.write_text(css.read_text().replace(art.PAL["amber"], "#123456"), encoding="utf-8")
        rc, msg = quiet(art.main, chk)
        check(rc == 1 and "amber" in msg, "--check catches a palette colour gone from the stylesheet")
        rc, _ = quiet(art.main, ["--stand-in-fonts", "--fonts", str(nofonts)])
        check(rc == 2, "stand-in fonts refuse installer/art")
        rc, _ = quiet(art.main, ["--stand-in-fonts", "--fonts", str(nofonts), "--css", str(css), "--out", str(tmp / "y")])
        check(rc == 2 and not (tmp / "y").exists(), "stand-in fonts refuse public/assets")
        rc, msg = quiet(art.main, ["--fonts", str(nofonts), "--css", str(art.CSS), "--out", str(tmp / "x"),
                                   "--public-out", str(tmp / "xp")])
        check(rc == 1 and "--fetch-fonts" in msg, "real render without fonts says how to get them")

        # --fetch-fonts with a fake opener: writes the fonts + OFL texts; a changed upstream font is refused
        class Resp(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False
        seen = []

        def opener(url, timeout):
            seen.append(url)
            return Resp(url.rsplit("/", 1)[1].encode())
        got = art.fetch_fonts(tmp / "f", {}, opener)
        check(all(u.startswith("https://raw.githubusercontent.com/google/fonts/main/ofl/") for u in seen)
              and len(seen) == 3 and set(got) == {fn for _, fn in art.FONT_FILES.values()}
              and (tmp / "f" / "OFL-rajdhani.txt").is_file() and not art.missing_fonts(tmp / "f"),
              f"fetch: Google's repo only, 2 fonts + 1 licence (Rajdhani only) {len(seen)}")
        try:
            art.fetch_fonts(tmp / "f", {"Rajdhani-Bold.ttf": "0" * 64}, opener)
            check(False, "fetch refuses a font that differs from its pin")
        except ValueError:
            check(True, "fetch refuses a font that differs from its pin")
        # the committed art, when present, is current (files vs art.json; no fonts needed)
        if (art.OUT / "art.json").is_file():
            rc, msg = quiet(art.main, ["--check", "--fonts", str(nofonts)])
            check(rc == 0, f"installer/art --check {msg.strip()[:200]}")
        else:
            print("skip installer/art not generated yet")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_installer_art:", "FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
