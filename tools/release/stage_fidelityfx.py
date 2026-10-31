#!/usr/bin/env python3
"""Stage AMD's signed FidelityFX SDK v1.1.4 DX12 DLL and MIT licence for the release payload.

  stage_fidelityfx.py --dll PATH --license PATH [--out DIR]

Writes DIR/amd_fidelityfx_dx12.dll, DIR/LICENSE.txt and DIR/fidelityfx.json. The exact DLL and licence hashes are
pinned so a different SDK provider cannot enter the payload under the expected name.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import assemble_payload as ap  # noqa: E402

SDK_VERSION = "1.1.4"
DLL_SHA256 = "12a5081257ec95b0b53ad51b4a87fb3c03f97fe0bbb59f9496968f8d50ef93a6"
DLL_BYTES = 6675664
LICENSE_SHA256 = "a335c2bb2bd28184b9a30b6e35b0c7e4aa7826abd1c388fc2aa02405e9d04f03"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stage(dll, lic, out):
    """Return a list of problems; an empty list means the files were staged."""
    problems = []
    if not dll or not os.path.isfile(dll):
        problems.append("amd_fidelityfx_dx12.dll not found (pass --dll)")
    else:
        bad = ap.pe_x64_problem(dll)
        if bad:
            problems.append(f"{dll}: {bad}")
        size = os.path.getsize(dll)
        if size != DLL_BYTES:
            problems.append(f"{dll}: {size} bytes, expected {DLL_BYTES} for SDK v{SDK_VERSION}")
        got = sha256(dll)
        if got != DLL_SHA256:
            problems.append(f"{dll}: SHA-256 {got}, expected {DLL_SHA256} for SDK v{SDK_VERSION}")
    if not lic or not os.path.isfile(lic):
        problems.append("FidelityFX docs/license.md not found (pass --license)")
    else:
        got = sha256(lic)
        if got != LICENSE_SHA256:
            problems.append(f"{lic}: SHA-256 {got}, expected the SDK v{SDK_VERSION} MIT licence")
        with open(lic, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "Advanced Micro Devices, Inc." not in text or "Permission is hereby granted" not in text:
            problems.append(f"{lic}: not the FidelityFX SDK MIT licence")
    if problems:
        return problems
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(dll, os.path.join(out, ap.FIDELITYFX_SRC[0]))
    shutil.copyfile(lic, os.path.join(out, ap.FIDELITYFX_SRC[1]))
    with open(os.path.join(out, "fidelityfx.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"version": SDK_VERSION, "dll_sha256": DLL_SHA256, "dll_bytes": DLL_BYTES,
                   "license_sha256": LICENSE_SHA256, "licence": "MIT"}, f, indent=1)
        f.write("\n")
    return []


def main(argv=None, out=sys.stdout):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(HERE)), "work", "release", "fidelityfx"))
    p.add_argument("--dll", required=True)
    p.add_argument("--license", required=True)
    a = p.parse_args(argv)
    problems = stage(a.dll, a.license, a.out)
    for problem in problems:
        print("stage_fidelityfx: " + problem, file=out)
    if not problems:
        print(f"staged FidelityFX SDK v{SDK_VERSION} ({DLL_BYTES:,} byte DLL) -> {a.out}", file=out)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
