#!/usr/bin/env python3
"""Preflight for a sandboxed game run (tools/proton-test.sh calls it before the sandbox starts).

The game reads its file switches as Windows paths (Z:\\home\\...). Inside tools/sandbox.sh only the checkout running the
run is visible, and only its work/ and build/ are writable. A switch pointing anywhere else fails inside the game
(every write "cannot write"), so this check refuses the run before it starts.

Rules for every KZ3_/LD_/RSX_ variable whose value is a drive path (X:\\... or X:/...):
- Z: maps to the host root; any other drive letter is refused (nothing else is mapped in the prefix).
- The host path must be inside ROOT (the sandbox's checkout).
- Under ROOT/work or ROOT/build: fine (its folder is created if missing, since the game does not create folders).
- Elsewhere under ROOT: read-only, so the file must already exist (an input).

usage: runenv_check.py ROOT      exit 0 = fine, 1 = refused (reasons on stderr)
"""
import os
import re
import sys

DRIVE = re.compile(r"^([A-Za-z]):[\\/]")
PREFIXES = ("KZ3_", "LD_", "RSX_")


def host_path(value):
    m = DRIVE.match(value)
    if not m:
        return None, None
    if m.group(1).upper() != "Z":
        return None, f"drive {m.group(1).upper()}: is not mapped in the run's prefix (use Z:)"
    return "/" + value[3:].replace("\\", "/"), None


def inside(path, root):
    root = root.rstrip("/")
    return path == root or path.startswith(root + "/")


def check(env, root, make_dirs=True):
    """List of problems (empty = fine)."""
    root = os.path.realpath(root)
    problems = []
    for name, value in sorted(env.items()):
        if not name.startswith(PREFIXES):
            continue
        path, err = host_path(value.strip())
        if err:
            problems.append(f"{name}={value}: {err}")
            continue
        if path is None:
            continue
        path = os.path.normpath(path)
        if not inside(path, root):
            problems.append(f"{name}={value}: outside this checkout ({root}); the sandbox cannot see it")
        elif inside(path, os.path.join(root, "work")) or inside(path, os.path.join(root, "build")):
            if make_dirs:
                os.makedirs(os.path.dirname(path), exist_ok=True)
        elif not os.path.exists(path):
            problems.append(f"{name}={value}: read-only in the sandbox and missing (outputs go under work/)")
    return problems


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    problems = check(os.environ, sys.argv[1])
    for p in problems:
        print("runenv-check: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
