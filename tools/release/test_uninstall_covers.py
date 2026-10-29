#!/usr/bin/env python3
"""Host test (release pass 51): every file or folder the launcher, first run, support and shader tools write under
the install folder {app}, and that Inno did not install itself, has a matching [UninstallDelete] entry in
installer/kz3.iss. Without one, `dirifempty {app}` fails after an uninstall ("Failed to delete directory (145)": the
Windows 10 VM run of 0.1.0). The write paths come from names.py, launch.py's constants and assemble_payload's RULES
(the payload folders where Python writes __pycache__); a new log name in names.py is checked without editing this test.
User data in %APPDATA% (saves, keybinds.ini, gamedata) is out of scope by design. Needs no game data.
"""
import fnmatch
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import names  # noqa: E402
import launch  # noqa: E402
import assemble_payload as ap  # noqa: E402

ISS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


def uninstall_patterns(text):
    """-> [(type, pattern relative to {app}, lower case)] from [UninstallDelete], #defines expanded."""
    defs = dict(re.findall(r'^#define (\w+) "([^"]*)"', text, re.M))
    sec = re.search(r"^\[UninstallDelete\]\n(.*?)(?=^\[)", text, re.M | re.S).group(1)
    out = []
    for m in re.finditer(r'^Type: (\w+); Name: "([^"]+)"', sec, re.M):
        name = re.sub(r"\{#(\w+)\}", lambda d: defs[d.group(1)], m.group(2))
        if name.startswith("{app}\\"):
            out.append((m.group(1), name[len("{app}\\"):].replace("\\", "/").lower()))
    return out


def covered(rel, pats):
    """A file or folder rel (relative to {app}) goes when a pattern matches it or a folder above it (filesandordirs)."""
    r = rel.lower()
    for typ, p in pats:
        if fnmatch.fnmatchcase(r, p):
            return True
        if typ == "filesandordirs" and r.startswith(p.rstrip("/") + "/"):
            return True
    return False


def main():
    with open(ISS, encoding="utf-8") as f:
        text = f.read()
    pats = uninstall_patterns(text)
    check(len(pats) > 10, f"{len(pats)} {{app}} entries read from [UninstallDelete]")

    stem = lambda n: os.path.splitext(n)[0]   # noqa: E731
    files = {
        "first-run log": names.FIRSTRUN_LOG,
        "first-run log, rotated (firstrun.rotate)": f"{stem(names.FIRSTRUN_LOG)}-20260101-120000.log",
        "first-run log, rotated twice": f"{stem(names.FIRSTRUN_LOG)}-20260101-120000-1.log",
        "setup log (firstrun default name)": names.SETUP_LOG,
        "setup log, rotated": f"{stem(names.SETUP_LOG)}-20260101-120000.log",
        "support package (support.zip_name with --out {app})": f"{names.FILE_BASE}-support-20260101-120000.zip",
        "launcher log": "logs/launcher.log",
        "copied first-run log": f"logs/{names.FIRSTRUN_LOG}",
        "install settings": "kz3-install.ini",
        "portable marker": launch.PORTABLE_FILE,
        "build output": "build/kz3.exe",
        "build cache": "build/cache/edat/x",
        "game VFS records": "vfs/link.json",
        "game VFS caches": "vfs/psarc-cache/x",
        "portable shader cache": "cache/x",
    }
    for what, rel in files.items():
        check(covered(rel, pats), f"{what}: {rel}")

    # The payload's own folders: Inno removes the files it installed, but Python (kz3-setup, the embedded toolchain
    # python) writes __pycache__ beside them; the folder must go whole. Folders holding no program are exempt.
    exempt = {"licenses", "fonts", "runtime", "lang"}   # lang: the UI text table only
    tops = {d.split("/")[0] for _, d, kind, _ in ap.RULES if "/" in d or kind == "tree"} | {"toolchain"}
    for t in sorted(tops - exempt):
        check(covered(t + "/__pycache__/x.pyc", pats), f"payload folder {t}/ goes whole (any __pycache__ in it)")

    # A name in names.py that ends in .log or .zip and is not covered above is a new write path: say so.
    for k, v in vars(names).items():
        if isinstance(v, str) and v.endswith((".log", ".zip")) and "/" not in v and "{" not in v:
            check(covered(v, pats), f"names.{k} = {v} is covered")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
