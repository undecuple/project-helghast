#!/usr/bin/env python3
"""Host test for firstrun_check.py's input reasons (issues #8 and #10, pass 64) and its imports under an isolated
Python (the bundled embeddable Python's ._pth puts only its own folders on sys.path). Synthetic fixtures only (the
helpers of test_firstrun_check.py), no game data. Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_input_reasons.py

Cases: a drive-root-like folder as the dump names what it holds, the accepted forms, and the update and EBOOT as
passed, without the redundant D1-D3 messages; PS3_GAME one folder down and an ISO file beside are named; a pick of
PS3_GAME or PS3_GAME/USRDIR passes; the update: empty, a disc dump, PARAM.SFO without USRDIR/EBOOT.BIN, a folder
above it, its USRDIR picked (passes); the EBOOT: missing (EBOOT.elf is missing + GETTING-FILES), the disc's 1.00 one;
a passing run has no reasons (control). Isolated import: `python3 -I firstrun_check.py --dump x.iso` from another
folder imports iso9660; a copy without the sys.path line fails the same way (negative control).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun_check as fc  # noqa: E402
from test_firstrun_check import make_dump, make_update, write_elf, write_sfo  # noqa: E402

FAILS = []
SMALL = ["--min-psarc-bytes", "1024", "--skip-archive-hash", "--json"]


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"\n     {str(detail)[:1500]}"))
    if not cond:
        FAILS.append(name)


def run(args):
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fc.main(args + SMALL)
    j = json.loads(buf.getvalue())
    return rc, j["facts"].get("reasons", []), j


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-reasons-")
    try:
        d = make_dump(tmp)
        u = make_update(tmp)
        elf = os.path.join(u, "USRDIR", "EBOOT.elf")
        s = os.sep
        # control: everything right -> no reasons
        rc, rs, _ = run(["--dump", d, "--update", u])
        check("control: a passing run has no reasons", rc == 0 and rs == [], rs)

        # the disc: a drive-root-like folder (the Reddit report: e:\ given, D1-D3 failed, read as an EBOOT problem)
        root = os.path.join(tmp, "drive")
        for n in ("Games", "Music", "System Volume Information"):
            os.makedirs(os.path.join(root, n))
        open(os.path.join(root, "notes.txt"), "w").close()
        rc, rs, _ = run(["--dump", root, "--update", u])
        check("drive root: one disc line naming what is there", rc == 1 and len(rs) == 2 and rs[0].startswith(
            f"Disc dump failed: {root} holds no PS3_GAME folder (3 folders, 1 files). It must be the folder that "
            f"holds PS3_GAME (with PS3_GAME{s}PARAM.SFO"), rs)
        check("drive root: the accepted forms", "the PS3_GAME folder itself, or a decrypted .iso file picked as a file"
              in rs[0], rs)
        check("drive root: no redundant D1-D3 text", "no PARAM.SFO at" not in rs[0] and "Found:" not in rs[0], rs)
        check("drive root: update and EBOOT passed", rs[-1] == "Passed: the 1.14 update and the decrypted EBOOT.", rs)
        # PS3_GAME one folder down; an ISO beside
        up = os.path.join(tmp, "up")
        os.makedirs(up)
        shutil.copytree(d, os.path.join(up, "KZ3"))
        rc, rs, _ = run(["--dump", up, "--update", u])
        check("PS3_GAME one folder down is named", rc == 1 and f"one folder down, the disc dump looks like {os.path.join(up, 'KZ3')}: pick that folder"
              in rs[0], rs)
        isod = os.path.join(tmp, "isod")
        os.makedirs(isod)
        open(os.path.join(isod, "Killzone 3.iso"), "w").close()
        rc, rs, _ = run(["--dump", isod, "--update", u])
        check("an ISO file beside is named", rc == 1 and "it holds the ISO file Killzone 3.iso: pick that file" in rs[0], rs)
        rc, rs, _ = run(["--dump", os.path.join(tmp, "nowhere"), "--update", u])
        check("missing dump path: nothing is there", rc == 1 and "nothing is there" in rs[0], rs)
        # PS3_GAME with a file missing: the D3 message is kept
        os.remove(os.path.join(d, "PS3_GAME", "USRDIR", "data_ps3.psarc"))
        rc, rs, _ = run(["--dump", d, "--update", u])
        check("PS3_GAME without the archive: names it and keeps D3", rc == 1 and
              f"holds PS3_GAME, but not PS3_GAME{s}USRDIR{s}data_ps3.psarc" in rs[0] and "Found: disc dump: no PS3_GAME"
              in rs[0], rs)
        d = make_dump(os.path.join(tmp, "d2"))
        # picks one or two levels too deep pass
        for pick in (os.path.join(d, "PS3_GAME"), os.path.join(d, "PS3_GAME", "USRDIR")):
            rc, rs, j = run(["--dump", pick, "--update", u])
            check(f"a pick of {os.path.relpath(pick, d)} passes", rc == 0 and rs == [] and any(
                "using the folder that holds PS3_GAME" in c["message"] for c in j["checks"]), (rs, j["checks"][:3]))

        # the update
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        rc, rs, _ = run(["--dump", d, "--update", empty, "--eboot", elf])
        check("empty update folder", rc == 1 and rs[0].startswith(f"1.14 update failed: {empty} holds none of these "
              f"files (the folder is empty). It must be the installed 1.14 update folder") and
              f"PARAM.SFO, USRDIR{s}EBOOT.BIN and USRDIR{s}{fc.UPDATE_EDAT}" in rs[0], rs)
        check("empty update: disc and EBOOT passed", rs[-1] == "Passed: the disc dump and the decrypted EBOOT.", rs)
        rc, rs, _ = run(["--dump", d, "--update", d, "--eboot", elf])
        check("a disc dump given as the update", rc == 1 and "so it looks like the disc dump, not the update: give it as the disc dump" in rs[0], rs)
        half = os.path.join(tmp, "half")
        write_sfo(os.path.join(half, "PARAM.SFO"), {"APP_VER": "01.14", "CATEGORY": "GD", "TITLE_ID": "BCUS98234"})
        rc, rs, _ = run(["--dump", d, "--update", half, "--eboot", elf])
        check("update with PARAM.SFO only: says what it holds and keeps U2", rc == 1 and f"{half} holds PARAM.SFO." in rs[0]
              and "Found: 1.14 update: no USRDIR/EBOOT.BIN" in rs[0], rs)
        rc, rs, _ = run(["--dump", d, "--update", os.path.dirname(u), "--eboot", elf])
        check("the folder above the update", rc == 1 and f"one folder down, the 1.14 update looks like {u}: pick that folder" in rs[0], rs)
        rc, rs, _ = run(["--dump", d, "--update", os.path.join(u, "USRDIR")])
        check("a pick of the update's USRDIR passes", rc == 0 and rs == [], rs)

        # the EBOOT
        rc, rs, _ = run(["--dump", d, "--update", u, "--eboot", os.path.join(tmp, "EBOOT.elf")])
        check("missing EBOOT: EBOOT.elf is missing + GETTING-FILES", rc == 1 and rs[0].startswith(
            f"EBOOT failed: no file at {os.path.join(tmp, 'EBOOT.elf')}: EBOOT.elf is missing.") and
            fc.GETTING_FILES in rs[0] and rs[-1] == "Passed: the disc dump and the 1.14 update.", rs)
        old = os.path.join(tmp, "old", "EBOOT.elf")
        write_elf(old, 20_745_952)
        rc, rs, _ = run(["--dump", d, "--update", u, "--eboot", old])
        check("the disc's 1.00 EBOOT: E1's message kept", rc == 1 and "Found: this EBOOT is the disc's 1.00 one" in rs[0], rs)
        for r in rs:
            check("reason fits a REASON line", len(r) <= 900, r)

        # issue #13: drive roots as both inputs (e:\\ and f:\\); two levels searched on a root, one elsewhere
        e, f2 = os.path.join(tmp, "e"), os.path.join(tmp, "f")
        shutil.copytree(d, os.path.join(e, "Games", "KZ3"))
        shutil.copytree(u, os.path.join(f2, "Backup", "BCUS98234"))
        shutil.copytree(u, os.path.join(e, "PS3", "BCUS98234"))
        real_root = fc.is_drive_root
        fc.is_drive_root = lambda p: os.path.abspath(p) in (e, f2)
        try:
            rc, rs, _ = run(["--dump", e, "--update", f2, "--eboot", elf])
        finally:
            fc.is_drive_root = real_root
        disc_l = next((x for x in rs if x.startswith("Disc dump failed:")), "")
        upd_l = next((x for x in rs if x.startswith("1.14 update failed:")), "")
        check("drive roots: the disc line names the disc and the update on the drive", rc == 1 and
              f"{e} is a drive root; on this drive, the disc dump looks like {os.path.join(e, 'Games', 'KZ3')}: pick that "
              f"folder; the 1.14 update looks like {os.path.join(e, 'PS3', 'BCUS98234')} (give it as the update)" in disc_l, rs)
        check("drive roots: the update line names the update on its drive", f"{f2} is a drive root; on this drive, the "
              f"1.14 update looks like {os.path.join(f2, 'Backup', 'BCUS98234')}: pick that folder" in upd_l, rs)
        check("drive roots: EBOOT passed", rs[-1] == "Passed: the decrypted EBOOT.", rs)
        rc, rs, _ = run(["--dump", e, "--update", f2, "--eboot", elf])
        check("control: the same folders, not drive roots, are searched one level only", rc == 1 and
              "looks like" not in " ".join(rs) and "(2 folders, 0 files)" in rs[0], rs)
        # issue #12: roles swapped or mixed up
        rc, rs, _ = run(["--dump", u, "--update", d, "--eboot", elf])
        check("swapped: the update given as the disc", rc == 1 and rs[0].startswith(f"Disc dump failed: {u} looks like the "
              "1.14 update") and "give it as the 1.14 update" in rs[0], rs)
        check("swapped: the disc given as the update", len(rs) > 1 and rs[1].startswith(f"1.14 update failed: {d} holds "
              "PS3_GAME, so it looks like the disc dump"), rs)
        edat_only = os.path.join(tmp, "edat_only")
        os.makedirs(os.path.join(edat_only, "USRDIR"))
        shutil.copy(os.path.join(u, "USRDIR", fc.UPDATE_EDAT), os.path.join(edat_only, "USRDIR"))
        rc, rs, _ = run(["--dump", edat_only, "--update", u])
        check("an update folder with only its edat, as the disc (data_ps3.psarc missing)", rc == 1 and
              "looks like the 1.14 update" in rs[0], rs)
        rc, rs, j = run(["--dump", d, "--update", os.path.join(d, "PS3_GAME"), "--eboot", elf])
        check("the disc's PS3_GAME as the update fails U1 and says so", rc == 1 and any(
            c["code"] == "U1" and c["level"] == "FAIL" and "category DG" in c["message"] for c in j["checks"]) and
            "looks like the disc's PS3_GAME folder" in rs[0], rs)
        check("control: the update with category GD passes U1", not any(
            c["code"] == "U1" and c["level"] == "FAIL" for c in run(["--dump", d, "--update", u])[2]["checks"]))
        # labels (issue #12): every input line starts with its input
        labels = ("Disc dump failed: ", "1.14 update failed: ", "EBOOT failed: ", "Passed: ")
        rc, rs, _ = run(["--dump", e, "--update", f2, "--eboot", os.path.join(tmp, "none.elf")])
        check("every line is labelled", rc == 1 and len(rs) == 3 and all(x.startswith(labels) for x in rs), rs)

        # isolated Python (the embeddable package's ._pth): the script's folder is not on sys.path
        if sys.version_info < (3, 11):
            print("SKIP isolated import (needs Python 3.11+ for -I to drop the script folder)")
        else:
            from iso_fixture import build_iso
            iso = build_iso(os.path.join(tmp, "g.iso"), {"PS3_GAME/PARAM.SFO": open(os.path.join(d, "PS3_GAME", "PARAM.SFO"), "rb").read()})
            cwd = os.path.join(tmp, "elsewhere")
            os.makedirs(cwd)
            p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "firstrun_check.py"), "--dump", iso, "--json"],
                               cwd=cwd, capture_output=True, text=True)
            check("isolated: firstrun_check imports iso9660 by full path", "ModuleNotFoundError" not in p.stderr and
                  '"checks"' in p.stdout, p.stderr[-600:])
            ctl = os.path.join(tmp, "ctl")
            os.makedirs(ctl)
            src = open(os.path.join(HERE, "firstrun_check.py")).read()
            line = "sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))\n"
            open(os.path.join(ctl, "firstrun_check.py"), "w").write(src.replace(line, "", 1))
            shutil.copy(os.path.join(HERE, "iso9660.py"), ctl)
            p = subprocess.run([sys.executable, "-I", os.path.join(ctl, "firstrun_check.py"), "--dump", iso, "--json"],
                               cwd=cwd, capture_output=True, text=True)
            check("isolated control: without the sys.path line the import fails", line in src and
                  "ModuleNotFoundError" in p.stderr and "iso9660" in p.stderr, p.stderr[-600:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"input_reasons: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
