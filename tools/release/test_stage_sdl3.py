#!/usr/bin/env python3
"""Host test for stage_sdl3.py: finds SDL3.dll from the shell's path lists (nix layout: bin, -lib, -dev outputs), refuses
a 32-bit or non-DLL file and a missing or foreign licence, writes sdl3.json. Self-contained: builds fake PE files."""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stage_sdl3 as ss  # noqa: E402

FAILS = []
LICENCE = "Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>\n\nThis software is provided 'as-is', without any express\n"


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  [{detail}]"))
    if not cond:
        FAILS.append(what)


def pe(machine=0x8664, flags=0x2000, size=4096):
    head = bytearray(64) + b"PE\0\0" + machine.to_bytes(2, "little") + bytes(16) + flags.to_bytes(2, "little")
    head[:2], head[60:64] = b"MZ", (64).to_bytes(4, "little")
    return bytes(head) + bytes(size - len(head))


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def run(args, environ):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ss.main(args, environ=environ, out=buf)
    return rc, buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-sdl3-")
    try:
        store = os.path.join(tmp, "store")
        out_main = os.path.join(store, "aaa-sdl3-x86_64-w64-mingw32-3.4.16")
        dev = os.path.join(store, "bbb-sdl3-x86_64-w64-mingw32-3.4.16-dev")
        put(os.path.join(out_main, "bin", "SDL3.dll"), pe())
        put(os.path.join(out_main, "share", "licenses", "SDL3", "LICENSE.txt"), LICENCE)
        os.makedirs(os.path.join(dev, "lib", "pkgconfig"))
        # found from the main output's bin on PATH
        dll, lic = ss.find_sdl3({"PATH": os.pathsep.join(["/nonexistent", os.path.join(out_main, "bin")])})
        check(dll.endswith("SDL3.dll") and lic.endswith("LICENSE.txt"), "found via a PATH entry", f"{dll} {lic}")
        # found from the -dev output's pkgconfig dir only when the main output sits beside it under the same name
        dll, _ = ss.find_sdl3({"PKG_CONFIG_PATH": os.path.join(dev, "lib", "pkgconfig")})
        check(dll == "", "a -dev output without the DLL beside it is not a match", dll)
        dll, _ = ss.find_sdl3({"PATH": "/usr/bin"})
        check(dll == "", "nothing found when no entry names sdl3", dll)
        out = os.path.join(tmp, "out")
        env = {"PATH": os.path.join(out_main, "bin")}
        rc, txt = run(["--out", out], env)
        meta = json.load(open(os.path.join(out, "sdl3.json")))
        check(rc == 0 and os.path.isfile(os.path.join(out, "SDL3.dll")) and os.path.isfile(os.path.join(out, "LICENSE.txt"))
              and meta["licence"] == "zlib" and "Sam Lantinga" in meta["copyright"] and len(meta["sha256"]) == 64,
              "staged DLL, licence and sdl3.json", txt)
        rc, txt = run(["--out", out], {"PATH": "/usr/bin"})
        check(rc == 1 and "not found" in txt, "no DLL in the shell: refused with the fix", txt)
        bad = os.path.join(tmp, "bad32.dll")
        put(bad, pe(machine=0x14c))
        rc, txt = run(["--out", out, "--dll", bad, "--license", os.path.join(out, "LICENSE.txt")], {})
        check(rc == 1 and "AMD64" in txt, "a 32-bit DLL is refused", txt)
        notdll = os.path.join(tmp, "x.dll")
        put(notdll, pe(flags=0x0002))
        rc, txt = run(["--out", out, "--dll", notdll, "--license", os.path.join(out, "LICENSE.txt")], {})
        check(rc == 1 and "not a DLL" in txt, "a PE that is not a DLL is refused", txt)
        other = os.path.join(tmp, "other-licence.txt")
        put(other, "MIT licence, someone else\n")
        rc, txt = run(["--out", out, "--dll", os.path.join(out, "SDL3.dll"), "--license", other], {})
        check(rc == 1 and "zlib" in txt, "a licence text that is not SDL's is refused", txt)
        rc, txt = run(["--out", out, "--dll", os.path.join(out, "SDL3.dll")], {})
        check(rc == 1 and "LICENSE.txt not found" in txt, "--dll without a licence is refused", txt)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"test_stage_sdl3: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
