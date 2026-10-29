#!/usr/bin/env python3
"""Pack an assembled payload (assemble_payload.py --out DIR) into the portable zip
(release pass 4). Standard library only.

    pack_zip.py --payload DIR --out Project-Helghast-<version>-portable.zip (names.zip_name) [--top names.ZIP_TOP] [--replace]

Before writing anything it checks the payload against its own manifest.json: every listed file present with the
listed size and SHA-256, nothing on disk that the manifest does not list (besides manifest.json and VERSION.txt),
run.cmd present (the zip's entry point), the two Rajdhani fonts under fonts/ and their OFL text, and the assemble scrub (game names and magics, home paths, private hosts,
secrets, sizes) again on every file outside toolchain/ (the bundled compiler, CMake and Python are third-party
binaries over the scrub's size limit; their pinned hashes are the release workflow's check, plan 10.4).
Any problem refuses the whole zip.

Deterministic: entries sorted by path, all under one top folder (default names.ZIP_TOP, "Project Helghast/"), fixed 1980-01-01 dates,
MS-DOS attributes, Deflate level 9, no extra fields, no comment, so the same payload gives the same bytes with the
same Python/zlib (ASSUMPTION: another zlib build may compress differently; the manifest's hashes stay the same).
A `<zip>.sha256` file ("<hash>  <name>", the release asset format the version check's Download step reads) is
written beside it. Exit 0 = written, 1 = refused, 2 = usage.
"""
import argparse
import hashlib
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import names  # noqa: E402  (tools/release/names.py)
import assemble_payload as ap_mod  # noqa: E402

FIXED_DATE = (1980, 1, 1, 0, 0, 0)
NOT_LISTED = {"manifest.json", "VERSION.txt"}      # written by assemble_payload itself, not entries of the manifest
REQUIRED = ("run.cmd", "manifest.json", "VERSION.txt",
            # the runtime's error-notice fonts and their licence (assemble_payload FONTS; RSX pass 38)
            "fonts/Rajdhani-Medium.ttf", "fonts/Rajdhani-Bold.ttf", "licenses/Rajdhani-OFL.txt",
            # SDL3 for the pads (patch 4010) and its licence (assemble_payload SDL3_DEST)
            "runtime/SDL3.dll", "licenses/SDL3-LICENSE.txt",
            # the port UI text table the runtime reads from <exe dir>\lang (net 45)
            "lang/port_ui.tsv")
NO_SCRUB_PREFIX = "toolchain/"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def walk(payload):
    out = []
    for d, dirs, files in os.walk(payload):
        dirs[:] = [x for x in dirs if not os.path.islink(os.path.join(d, x))]
        for n in files:
            p = os.path.join(d, n)
            out.append((os.path.relpath(p, payload).replace(os.sep, "/"), p))
    return sorted(out)


def verify(payload):
    """-> (problems, [(rel, path)], manifest)."""
    problems = []
    try:
        with open(os.path.join(payload, "manifest.json"), encoding="utf-8") as f:
            man = json.load(f)
    except (OSError, ValueError) as e:
        return [f"manifest.json: {e}"], [], None
    listed = {e["path"]: e for e in man.get("files", [])}
    on_disk = walk(payload)
    for rel, p in on_disk:
        if os.path.islink(p):
            problems.append(f"{rel}: symlink")
        elif rel not in listed and rel not in NOT_LISTED:
            problems.append(f"{rel}: not in manifest.json")
    have = {rel for rel, _ in on_disk}
    for rel in REQUIRED:
        if rel not in have:
            problems.append(f"{rel}: missing (required)")
    for rel, e in sorted(listed.items()):
        p = os.path.join(payload, *rel.split("/"))
        if rel not in have:
            problems.append(f"{rel}: listed in manifest.json, missing on disk")
        elif os.path.getsize(p) != e["size"] or sha256_file(p) != e["sha256"]:
            problems.append(f"{rel}: size or SHA-256 differs from manifest.json")
    if man.get("count") != len(listed):
        problems.append(f"manifest.json: count {man.get('count')} != {len(listed)} entries")
    for rel, p in on_disk:
        if not rel.startswith(NO_SCRUB_PREFIX) and not os.path.islink(p):
            problems += [f"{rel}: {why}" for why in ap_mod.scrub_ship(p, rel)]
    return problems, on_disk, man


def zip_name(top, rel):
    """A zip entry name: always '/' whatever the host OS (a zip built on Windows with backslashes unpacks wrongly on
    Linux; zip files use '/' only)."""
    return f"{top}/{rel}".replace(os.sep, "/").replace("\\", "/")


def write_zip(out, top, entries):
    part = out + ".partial"
    with zipfile.ZipFile(part, "w") as z:
        for rel, p in entries:
            zi = zipfile.ZipInfo(zip_name(top, rel), date_time=FIXED_DATE)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 0
            zi.external_attr = 0x20                     # MS-DOS "archive"
            with open(p, "rb") as f:
                z.writestr(zi, f.read(), compresslevel=9)
    os.replace(part, out)
    digest = sha256_file(out)
    with open(out + ".sha256", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"{digest}  {os.path.basename(out)}\n")
    return digest


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--payload", required=True)
    ap.add_argument("--out", required=True, help="the .zip, or a folder: names.zip_name(<manifest version>) in it")
    ap.add_argument("--top", default=names.ZIP_TOP)
    ap.add_argument("--replace", action="store_true")
    a = ap.parse_args(argv)
    to_dir = not a.out.lower().endswith(".zip")
    if (to_dir and os.path.exists(a.out) and not os.path.isdir(a.out)) or "/" in a.top or "\\" in a.top \
            or not a.top.strip():
        print("--out must end in .zip or name a folder, and --top must be one folder name", file=out)
        return 2
    problems, entries, man = verify(os.path.abspath(a.payload))
    dest = os.path.abspath(os.path.join(a.out, names.zip_name((man or {}).get("version") or "0.0.0")) if to_dir else a.out)
    if os.path.lexists(dest) and not a.replace:
        print(f"{dest} exists (--replace overwrites it)", file=out)
        return 1
    for p in problems:
        print(f"REFUSED {p}", file=out)
    if problems:
        print(f"zip refused: {len(problems)} problem(s); nothing written", file=out)
        return 1
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    digest = write_zip(dest, a.top, entries)
    total = sum(os.path.getsize(p) for _, p in entries)
    print(f"wrote {dest}: {len(entries)} files, {total:,} bytes unpacked, {os.path.getsize(dest):,} packed, "
          f"version {man.get('version')}, sha256 {digest}", file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
