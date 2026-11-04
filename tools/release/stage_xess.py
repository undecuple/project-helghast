#!/usr/bin/env python3
"""Stage Intel XeSS SDK 3.0.2's XeSS-SR runtime and required notices for the release payload.

  stage_xess.py --dll PATH --license PATH --notices PATH [--out DIR]

Writes DIR/libxess.dll, DIR/LICENSE.txt, DIR/third-party-programs.txt and DIR/xess.json.
Every redistributed file is pinned to Intel's unmodified official release.
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

SDK_VERSION = "3.0.2"
SR_API_VERSION = "2.0.2"
# The pins live in assemble_payload.XESS_PINS; the payload gate and this staging step must agree.
PINS = dict(zip(("dll", "license", "notices"), (ap.XESS_PINS[dest] for dest in ap.XESS_DEST)))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_pinned(path, kind, label):
    if not path or not os.path.isfile(path) or os.path.islink(path):
        return [f"{label} not found as a regular file"]
    size, digest = PINS[kind]
    problems = []
    if os.path.getsize(path) != size:
        problems.append(
            f"{path}: {os.path.getsize(path)} bytes, expected {size} for XeSS SDK {SDK_VERSION}"
        )
    got = sha256(path)
    if got != digest:
        problems.append(
            f"{path}: SHA-256 {got}, expected {digest} for XeSS SDK {SDK_VERSION}"
        )
    return problems


def stage(dll, lic, notices, out):
    """Return a list of problems; an empty list means the files were staged."""
    problems = check_pinned(dll, "dll", "libxess.dll")
    if dll and os.path.isfile(dll):
        bad = ap.pe_x64_problem(dll)
        if bad:
            problems.append(f"{dll}: {bad}")
    problems += check_pinned(lic, "license", "Intel XeSS LICENSE.txt")
    problems += check_pinned(
        notices, "notices", "Intel XeSS third-party-programs.txt"
    )
    if problems:
        return problems

    os.makedirs(out, exist_ok=True)
    for src, name in zip((dll, lic, notices), ap.XESS_SRC):
        shutil.copyfile(src, os.path.join(out, name))
    with open(
        os.path.join(out, "xess.json"),
        "w",
        encoding="utf-8",
        newline="\n",
    ) as f:
        json.dump(
            {
                "sdk_version": SDK_VERSION,
                "sr_api_version": SR_API_VERSION,
                "files": {
                    name: {"bytes": PINS[kind][0], "sha256": PINS[kind][1]}
                    for name, kind in zip(ap.XESS_SRC, ("dll", "license", "notices"))
                },
            },
            f,
            indent=1,
        )
        f.write("\n")
    return []


def main(argv=None, out=sys.stdout):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument(
        "--out",
        default=os.path.join(
            os.path.dirname(os.path.dirname(HERE)), "work", "release", "xess"
        ),
    )
    p.add_argument("--dll", required=True)
    p.add_argument("--license", required=True)
    p.add_argument("--notices", required=True)
    a = p.parse_args(argv)
    problems = stage(a.dll, a.license, a.notices, a.out)
    for problem in problems:
        print("stage_xess: " + problem, file=out)
    if not problems:
        print(
            f"staged Intel XeSS SDK {SDK_VERSION} "
            f"({PINS['dll'][0]:,} byte DLL) -> {a.out}",
            file=out,
        )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
