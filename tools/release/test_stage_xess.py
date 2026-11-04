#!/usr/bin/env python3
"""Host test for stage_xess.py. Self-contained: uses fake pinned SDK files."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import tempfile

import stage_xess as sx

FAILS = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  [{detail}]"))
    if not cond:
        FAILS.append(what)


def pe(machine=0x8664, flags=0x2000, size=4096):
    head = (
        bytearray(64)
        + b"PE\0\0"
        + machine.to_bytes(2, "little")
        + bytes(16)
        + flags.to_bytes(2, "little")
    )
    head[:2], head[60:64] = b"MZ", (64).to_bytes(4, "little")
    return bytes(head) + bytes(size - len(head))


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode, newline=None if isinstance(data, bytes) else "\n") as f:
        f.write(data)


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = sx.main(args, out=buf)
    return rc, buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-xess-")
    old_pins = dict(sx.PINS)
    try:
        dll = os.path.join(tmp, "libxess.dll")
        lic = os.path.join(tmp, "LICENSE.txt")
        notices = os.path.join(tmp, "third-party-programs.txt")
        out = os.path.join(tmp, "out")
        put(dll, pe())
        put(lic, "Intel XeSS software license\n")
        put(notices, "Intel XeSS third-party programs\n")
        sx.PINS = {
            "dll": (os.path.getsize(dll), digest(dll)),
            "license": (os.path.getsize(lic), digest(lic)),
            "notices": (os.path.getsize(notices), digest(notices)),
        }

        rc, txt = run(
            [
                "--dll",
                dll,
                "--license",
                lic,
                "--notices",
                notices,
                "--out",
                out,
            ]
        )
        meta = json.load(open(os.path.join(out, "xess.json"), encoding="utf-8"))
        check(
            rc == 0
            and all(os.path.isfile(os.path.join(out, name)) for name in sx.ap.XESS_SRC)
            and meta["sdk_version"] == "3.0.2"
            and meta["sr_api_version"] == "2.0.2",
            "staged runtime, licence, notices, and metadata",
            txt,
        )

        bad = os.path.join(tmp, "bad.dll")
        put(bad, pe(machine=0x14C))
        sx.PINS["dll"] = (os.path.getsize(bad), digest(bad))
        rc, txt = run(
            ["--dll", bad, "--license", lic, "--notices", notices, "--out", out]
        )
        check(rc == 1 and "AMD64" in txt, "refused a 32-bit DLL", txt)

        put(bad, pe(flags=0x0002))
        sx.PINS["dll"] = (os.path.getsize(bad), digest(bad))
        rc, txt = run(
            ["--dll", bad, "--license", lic, "--notices", notices, "--out", out]
        )
        check(rc == 1 and "not a DLL" in txt, "refused a PE that is not a DLL", txt)

        sx.PINS["dll"] = old_pins["dll"]
        rc, txt = run(
            ["--dll", dll, "--license", lic, "--notices", notices, "--out", out]
        )
        check(
            rc == 1 and "bytes, expected" in txt and "SHA-256" in txt,
            "refused a different runtime",
            txt,
        )

        sx.PINS["dll"] = (os.path.getsize(dll), digest(dll))
        rc, txt = run(
            [
                "--dll",
                dll,
                "--license",
                lic,
                "--notices",
                os.path.join(tmp, "missing.txt"),
                "--out",
                out,
            ]
        )
        check(rc == 1 and "not found" in txt, "refused missing notices", txt)
    finally:
        sx.PINS = old_pins
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"test_stage_xess: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
