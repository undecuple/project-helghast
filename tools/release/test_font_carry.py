#!/usr/bin/env python3
"""Issue #60: the player's PS3 font folder survives an upgrade. "--font-dir auto" (the installer's Recommended type and
the zip flow) takes the earlier install's folder (build/paths.json font_dir, else font_src) before RPCS3's folder
beside the update; with none it stays empty; "-" still means none. Negative controls: each case is re-run against the
old rule (font_guess only) and must differ. Host test, no game data."""
import io
import os
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def fonts(d):
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "SCE-PS3-RD-R-LATIN.TTF"), "wb").write(b"x")
    return d


def paths(build, **kw):
    import json
    os.makedirs(build, exist_ok=True)
    json.dump(kw, open(os.path.join(build, "paths.json"), "w"))


def resolved(args, root):
    """What ctx.font_dir main() resolves for these arguments (the dry run is captured)."""
    got = {}
    real = fr.dry_run
    fr.dry_run = lambda ctx, *a, **k: got.update(fd=ctx.font_dir) or 0
    try:
        fr.main(args + ["--root", root, "--dry-run"], out=io.StringIO())
    finally:
        fr.dry_run = real
    return got.get("fd")


def main():
    with tempfile.TemporaryDirectory() as t:
        t = os.path.realpath(t)
        dump = os.path.join(t, "dump")
        os.makedirs(dump)
        base = ["--dump", dump]
        # A: earlier install with a font copy in build/fonts/ps3
        ra = os.path.join(t, "a")
        copy = fonts(os.path.join(ra, "build", "fonts", "ps3"))
        paths(os.path.join(ra, "build"), font_dir=copy, font_src=os.path.join(t, "gone"))
        check(resolved(base + ["--font-dir", "auto"], ra) == copy, "auto with an earlier font_dir: the copy is kept")
        check(resolved(base + ["--font-dir", "-"], ra) == "", "explicit '-' still means none")
        check(fr.font_guess("") == "", "negative control: the old rule (font_guess only) finds nothing here")
        # B: the copy is gone, the source folder remains (font_src)
        rb = os.path.join(t, "b")
        src = fonts(os.path.join(t, "fw", "font"))
        paths(os.path.join(rb, "build"), font_dir=os.path.join(rb, "build", "fonts", "ps3"), font_src=src)
        check(resolved(base + ["--font-dir", "auto"], rb) == src, "auto falls back to font_src when the copy is gone")
        # C: a stale record, nothing on disk
        rc = os.path.join(t, "c")
        paths(os.path.join(rc, "build"), font_dir=os.path.join(t, "nope"), font_src=os.path.join(t, "nope2"))
        check(resolved(base + ["--font-dir", "auto"], rc) == "", "auto with a recorded folder that is gone: none")
        # D: no earlier install
        rd = os.path.join(t, "d")
        os.makedirs(rd)
        check(resolved(base + ["--font-dir", "auto"], rd) == "", "auto with none: empty")
        # E: no earlier install, RPCS3 folder beside the update: font_guess still works (fresh install unchanged)
        upd = os.path.join(t, "rpcs3", "dev_hdd0", "game", "BCUS98234")
        os.makedirs(upd)
        r3 = fonts(os.path.join(t, "rpcs3", "dev_flash", "data", "font"))
        check(resolved(base + ["--update", upd, "--font-dir", "auto"], rd) == r3, "fresh install: RPCS3's folder beside the update")
        # F: both: the earlier install wins
        check(resolved(base + ["--update", upd, "--font-dir", "auto"], ra) == copy, "earlier install beats RPCS3's folder")
        # G: zip flow helper
        check(fr.earlier_fonts(os.path.join(ra, "build")) == copy and fr.earlier_fonts(os.path.join(rd, "build")) == "",
              "earlier_fonts: found / none")
        # H: zip flow (interactive) with no answer from setup: the earlier folder is used
        a = types.SimpleNamespace(dump=dump, update=os.path.join(t, "rpcs3", "dev_hdd0", "game", "BCUS98234"), eboot="", build=os.path.join(ra, "build"), portable=True,
                                  font_dir="", shaders="off", rpcs3="off", check_first=False)
        notes = []
        try:
            fr.interactive(a, ra, "x.log", notes, ask=lambda q: "", out=io.StringIO(), check=lambda *x: (0, "OK   D1: x\n"))
        except (EOFError, StopIteration, SystemExit):
            pass
        check(getattr(a, "font_dir", "") == copy, f"zip flow: earlier font folder used ({getattr(a, 'font_dir', None)!r})")
        # I: a source that only ever answers "" must end the dialogue, not re-ask for ever (the 2026-11-24 runaway: no
        # default folder because HOME held no RPCS3 folder, folder() looped and out grew to 16 GB). Detector: a cap on
        # the questions asked; the fix is that interactive() raises EOFError by itself.
        asked = []
        a2 = types.SimpleNamespace(dump="", update="", eboot="", build=os.path.join(rd, "build"), portable=True,
                                   font_dir="", shaders="off", rpcs3="off", check_first=False)

        def ask_empty(q):
            asked.append(q)
            if len(asked) > 200:
                raise RuntimeError("asked more than 200 times: loops for ever")
            return ""
        try:
            fr.interactive(a2, rd, "x.log", [], ask=ask_empty, out=io.StringIO(), check=lambda *x: (0, "OK   D1: x\n"))
            res = "returned"
        except EOFError:
            res = "EOFError"
        except RuntimeError as e:
            res = str(e)
        check(res == "EOFError" and len(asked) <= fr.MAX_REASK + 3,
              f"empty answers end the dialogue ({res}, {len(asked)} questions)")
        # negative control: with the cap off (the old behaviour) the same input never ends
        keep, fr.MAX_REASK, asked[:] = fr.MAX_REASK, 10 ** 9, []
        try:
            fr.interactive(a2, rd, "x.log", [], ask=ask_empty, out=io.StringIO(), check=lambda *x: (0, "OK   D1: x\n"))
            old = "returned"
        except RuntimeError:
            old = "loops"
        except EOFError:
            old = "EOFError"
        fr.MAX_REASK = keep
        check(old == "loops", f"negative control: without the cap the dialogue loops ({old})")
    print("FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
