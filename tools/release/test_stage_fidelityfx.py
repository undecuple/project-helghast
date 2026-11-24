#!/usr/bin/env python3
"""Host test for stage_fidelityfx.py. Self-contained: builds fake PE files and pins their hashes for the test."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import tempfile

import stage_fidelityfx as sf

FAILS = []
LICENSE = """# License

Copyright (C) 2024 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
"""


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
    with open(path, "wb" if isinstance(data, bytes) else "w", newline=None if isinstance(data, bytes) else "\n") as f:
        f.write(data)


def run(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = sf.main(args, out=buf)
    return rc, buf.getvalue()


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-fidelityfx-")
    old = sf.DLL_SHA256, sf.DLL_BYTES, sf.LICENSE_SHA256
    try:
        dll, lic, out = os.path.join(tmp, "amd_fidelityfx_dx12.dll"), os.path.join(tmp, "license.md"), os.path.join(tmp, "out")
        put(dll, pe())
        put(lic, LICENSE)
        sf.DLL_BYTES, sf.DLL_SHA256, sf.LICENSE_SHA256 = os.path.getsize(dll), digest(dll), digest(lic)
        rc, txt = run(["--dll", dll, "--license", lic, "--out", out])
        meta = json.load(open(os.path.join(out, "fidelityfx.json")))
        check(rc == 0 and os.path.isfile(os.path.join(out, "amd_fidelityfx_dx12.dll")) and
              os.path.isfile(os.path.join(out, "LICENSE.txt")) and meta["version"] == "1.1.4" and
              meta["dll_sha256"] == sf.DLL_SHA256, "staged DLL, licence and metadata", txt)
        bad = os.path.join(tmp, "bad.dll")
        put(bad, pe(machine=0x14c))
        rc, txt = run(["--dll", bad, "--license", lic, "--out", out])
        check(rc == 1 and "AMD64" in txt, "refused a 32-bit DLL", txt)
        put(bad, pe(flags=0x0002))
        rc, txt = run(["--dll", bad, "--license", lic, "--out", out])
        check(rc == 1 and "not a DLL" in txt, "refused a PE that is not a DLL", txt)
        put(bad, pe(size=4097))
        rc, txt = run(["--dll", bad, "--license", lic, "--out", out])
        check(rc == 1 and "bytes, expected" in txt and "SHA-256" in txt, "refused a different DLL", txt)
        other = os.path.join(tmp, "other.md")
        put(other, "MIT, someone else\n")
        rc, txt = run(["--dll", dll, "--license", other, "--out", out])
        check(rc == 1 and "MIT licence" in txt, "refused a foreign licence", txt)
        rc, txt = run(["--dll", os.path.join(tmp, "missing.dll"), "--license", lic, "--out", out])
        check(rc == 1 and "not found" in txt, "refused a missing DLL", txt)
    finally:
        sf.DLL_SHA256, sf.DLL_BYTES, sf.LICENSE_SHA256 = old
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"test_stage_fidelityfx: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
