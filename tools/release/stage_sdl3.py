#!/usr/bin/env python3
"""Stage SDL3.dll and its licence for the payload (assemble_payload.py --sdl3 DIR; default work/release/sdl3).

SDL3 (zlib licence) is what the game loads from its exe folder for native pads: DualShock 4, DualSense, Switch Pro,
Xbox without Steam, motion sensors and rumble (patch 4010). Run it in `nix develop .#win`: the sdl3 build input's
mingw bin/ is on the shell's path lists, so the DLL is found from the environment, never from a hard-coded store path.

  stage_sdl3.py [--out DIR] [--dll PATH --license PATH]     (--dll/--license override the search, e.g. on a CI runner
                                                             that unpacked libsdl-org's release zip)
Writes DIR/SDL3.dll, DIR/LICENSE.txt and DIR/sdl3.json (sha256, size, source). Exit 0 = staged, 1 = not found / not a
64-bit DLL / no licence text.
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

PATH_VARS = ("PATH", "HOST_PATH", "LINK_DLL_FOLDERS", "NIXPKGS_CMAKE_PREFIX_PATH", "CMAKE_PREFIX_PATH",
             "CMAKE_LIBRARY_PATH", "PKG_CONFIG_PATH")


def find_sdl3(environ):
    """-> (dll, licence or "") from the first entry of the shell's path lists that holds SDL3.dll (the entry itself, its
    bin/, or the sibling bin/ of an sdl3 -lib/-dev output), else ("", "")."""
    seen = []
    for var in PATH_VARS:
        for ent in environ.get(var, "").split(os.pathsep):
            if ent and "sdl3" in ent.lower() and ent not in seen:
                seen.append(ent)
    cands = []
    for ent in seen:
        base = ent
        for _ in range(3):                      # .../lib/pkgconfig -> .../lib -> the output's root
            cands += [base, os.path.join(base, "bin")]
            base = os.path.dirname(base)
        root = base
        cands += [root, os.path.join(root, "bin")]
        # nix splits outputs: strip -lib/-dev from the name and look beside it
        name = os.path.basename(root)
        for suf in ("-lib", "-dev"):
            if name.endswith(suf):
                cands.append(os.path.join(os.path.dirname(root), name[:-len(suf)], "bin"))
    for d in cands:
        dll = os.path.join(d, ap.SDL3_SRC[0])
        if os.path.isfile(dll):
            root = os.path.dirname(d) if os.path.basename(d) == "bin" else d
            lic = os.path.join(root, "share", "licenses", "SDL3", "LICENSE.txt")
            return dll, lic if os.path.isfile(lic) else ""
    return "", ""


def stage(dll, lic, out):
    """-> list of problems ([] = staged)."""
    if not dll or not os.path.isfile(dll):
        return ["SDL3.dll not found (run in `nix develop .#win`, or pass --dll)"]
    bad = ap.pe_x64_problem(dll)
    if bad:
        return [f"{dll}: {bad}"]
    if not lic or not os.path.isfile(lic):
        return ["SDL3's LICENSE.txt not found (pass --license)"]
    with open(lic, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "Sam Lantinga" not in text or "provided 'as-is'" not in text:
        return [f"{lic}: not SDL's zlib licence text"]
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(dll, os.path.join(out, ap.SDL3_SRC[0]))
    shutil.copyfile(lic, os.path.join(out, ap.SDL3_SRC[1]))
    with open(dll, "rb") as f:
        h = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(out, "sdl3.json"), "w", encoding="utf-8") as f:
        json.dump({"sha256": h, "bytes": os.path.getsize(dll), "licence": "zlib", "copyright": text.splitlines()[0]}, f, indent=1)
        f.write("\n")
    return []


def main(argv=None, environ=None, out=sys.stdout):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(HERE)), "work", "release", "sdl3"))
    p.add_argument("--dll", default="")
    p.add_argument("--license", default="")
    a = p.parse_args(argv)
    dll, lic = a.dll, a.license
    if not dll:
        dll, lic0 = find_sdl3(os.environ if environ is None else environ)
        lic = lic or lic0
    probs = stage(dll, lic, a.out)
    for m in probs:
        print("stage_sdl3: " + m, file=out)
    if not probs:
        print(f"staged {dll} ({os.path.getsize(dll):,} bytes) -> {a.out}", file=out)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main())
