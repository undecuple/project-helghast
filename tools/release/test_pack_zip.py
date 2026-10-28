#!/usr/bin/env python3
"""Host test for tools/release/pack_zip.py on a synthetic payload in a temp folder (no game data, no work/, no
network). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_pack_zip.py

Cases: a clean payload packs; two packs give the same bytes; entries sorted under the top folder with fixed dates and
the same contents; the .sha256 sidecar matches; an existing zip needs --replace; refused with nothing written: a
changed file (SHA-256), a file not in manifest.json, a listed file missing, run.cmd missing, a game-data name, an
ELF magic, a home path; toolchain/ is not scrubbed (third-party binaries) but must still match the manifest.
"""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_zip as pz  # noqa: E402
import names as rn  # noqa: E402  (tools/release/names.py; "names" is a local list below)

FAILS = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  {detail}"[:600]))
    if not cond:
        FAILS.append(what)


def put(root, rel, data):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def manifest(root):
    entries = []
    for rel, p in pz.walk(root):
        if rel not in pz.NOT_LISTED:
            with open(p, "rb") as f:
                d = f.read()
            entries.append({"path": rel, "size": len(d), "sha256": hashlib.sha256(d).hexdigest()})
    m = {"format": 1, "version": "0.1.0", "count": len(entries), "total_bytes": sum(e["size"] for e in entries),
         "files": entries}
    put(root, "manifest.json", json.dumps(m, indent=1) + "\n")


def payload(tmp):
    root = os.path.join(tmp, "payload")
    put(root, "run.cmd", "@echo off\r\n")
    put(root, "VERSION.txt", "0.1.0\n")
    put(root, "kz3-setup/release/firstrun.py", "print('x')\n")
    put(root, "port/kz3/main.cpp", "int main(){}\n")
    put(root, "toolchain/bin/clang.exe", b"MZ" + bytes(3 << 20))     # binary > 2 MB: allowed under toolchain/
    put(root, "Project Helghast.exe", b"MZ" + bytes(1000))
    put(root, "fonts/Rajdhani-Medium.ttf", b"\0\1\0\0" + bytes(500))
    put(root, "fonts/Rajdhani-Bold.ttf", b"\0\1\0\0" + bytes(600))
    put(root, "licenses/Rajdhani-OFL.txt", "SIL OPEN FONT LICENSE Version 1.1\n")
    pe = bytearray(64) + b"PE\0\0" + (0x8664).to_bytes(2, "little") + bytes(16) + (0x2000).to_bytes(2, "little")
    pe[:2], pe[60:64] = b"MZ", (64).to_bytes(4, "little")      # a minimal 64-bit PE DLL header (assemble_payload.pe_x64_problem)
    put(root, "runtime/SDL3.dll", bytes(pe) + bytes(3 << 20))  # a real SDL3.dll is ~5 MB: over the 2 MiB binary cap
    put(root, "licenses/SDL3-LICENSE.txt", "Copyright (C) 1997-2026 Sam Lantinga\n")
    manifest(root)
    return root


def run(args):
    buf = io.StringIO()
    rc = pz.main(args, out=buf)
    return rc, buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-zip-")
    try:
        root = payload(tmp)
        z1, z2 = os.path.join(tmp, "a", "kz3.zip"), os.path.join(tmp, "b", "kz3.zip")
        rc, txt = run(["--payload", root, "--out", z1])
        check(rc == 0 and os.path.isfile(z1), "clean payload packs", txt)
        rc2, _ = run(["--payload", root, "--out", z2])
        with open(z1, "rb") as f1, open(z2, "rb") as f2:
            b1, b2 = f1.read(), f2.read()
        check(rc2 == 0 and b1 == b2, "two packs give the same bytes")
        with zipfile.ZipFile(z1) as z:
            infos = z.infolist()
            names = [i.filename for i in infos]
            same = all(z.read(f"Project Helghast/{rel}") == open(p, "rb").read() for rel, p in pz.walk(root))
        check(names == sorted(names) and all(n.startswith("Project Helghast/") for n in names)
              and len(names) == 12 and "Project Helghast/manifest.json" in names, "sorted entries under the top folder",
              names)
        check({"Project Helghast/runtime/SDL3.dll", "Project Helghast/licenses/SDL3-LICENSE.txt"} <= set(names),
              "SDL3.dll and its licence are in the zip", names)
        check({"Project Helghast/fonts/Rajdhani-Medium.ttf", "Project Helghast/fonts/Rajdhani-Bold.ttf",
               "Project Helghast/licenses/Rajdhani-OFL.txt"} <= set(names), "the notice fonts and their OFL text are in the zip",
              names)
        check(all(i.date_time == pz.FIXED_DATE for i in infos) and same, "fixed dates, same contents")
        with open(z1 + ".sha256", encoding="utf-8") as f:
            side = f.read()
        check(side == f"{hashlib.sha256(b1).hexdigest()}  kz3.zip\n", ".sha256 sidecar", side)
        rc, txt = run(["--payload", root, "--out", z1])
        check(rc == 1 and "--replace" in txt, "existing zip needs --replace", txt)
        rc, txt = run(["--payload", root, "--out", z1, "--replace"])
        check(rc == 0, "--replace overwrites", txt)

        bad = os.path.join(tmp, "bad.zip")

        def refused(what, mutate, expect, remanifest=False):
            r = payload(os.path.join(tmp, what.replace(" ", "_")))
            mutate(r)
            if remanifest:
                manifest(r)
            rc, txt = run(["--payload", r, "--out", bad])
            check(rc == 1 and expect in txt and not os.path.exists(bad) and not os.path.exists(bad + ".partial"),
                  f"refused: {what}", txt)

        refused("changed file", lambda r: put(r, "port/kz3/main.cpp", "int main(){return 1;}\n"), "SHA-256 differs")
        refused("unlisted file", lambda r: put(r, "port/kz3/extra.c", "x\n"), "not in manifest.json")
        refused("listed file missing", lambda r: os.remove(os.path.join(r, "port", "kz3", "main.cpp")),
                "missing on disk")
        refused("no run.cmd", lambda r: os.remove(os.path.join(r, "run.cmd")), "run.cmd: missing", remanifest=True)
        refused("no notice font", lambda r: os.remove(os.path.join(r, "fonts", "Rajdhani-Medium.ttf")),
                "fonts/Rajdhani-Medium.ttf: missing", remanifest=True)
        refused("no font licence", lambda r: os.remove(os.path.join(r, "licenses", "Rajdhani-OFL.txt")),
                "licenses/Rajdhani-OFL.txt: missing", remanifest=True)
        refused("no SDL3.dll", lambda r: os.remove(os.path.join(r, "runtime", "SDL3.dll")),
                "runtime/SDL3.dll: missing", remanifest=True)
        refused("game name", lambda r: put(r, "port/EBOOT.BIN", "x\n"), "game-data name", remanifest=True)
        refused("ELF magic", lambda r: put(r, "port/blob.bin", b"\x7fELF" + bytes(10)), "ELF binary",
                remanifest=True)
        refused("home path", lambda r: put(r, "port/n.txt", "see /home/" "someone/x\n"), "home path",
                remanifest=True)
        refused("changed toolchain file", lambda r: put(r, "toolchain/bin/clang.exe", b"MZ" + bytes(10)),
                "SHA-256 differs")
        put(tmp, "x.tar", b"not a folder")
        rc, txt = run(["--payload", root, "--out", os.path.join(tmp, "x.tar")])
        check(rc == 2, "--out must be a .zip or a folder", txt)
        # --out FOLDER: the release name from names.py and the manifest's version, one top folder (rn.ZIP_TOP)
        rc, txt = run(["--payload", root, "--out", os.path.join(tmp, "rel")])
        zp = os.path.join(tmp, "rel", rn.zip_name("0.1.0"))
        with zipfile.ZipFile(zp) as z:
            tops = {n.split("/")[0] for n in z.namelist()}
        check(rc == 0 and os.path.basename(zp) == "Project-Helghast-0.1.0-portable.zip"
              and tops == {"Project Helghast"} and os.path.isfile(zp + ".sha256"),
              f"--out folder -> {os.path.basename(zp)} under {tops}", txt)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"test_pack_zip: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
