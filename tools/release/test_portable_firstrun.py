#!/usr/bin/env python3
"""Host test: a first run that was not told the data mode never lowers the installer's portable=1 (tester report,
2026-10: firstrun.py --interactive wrote portable=0 and the game's working folder became AppData). Also: launch.firstrun_cmd
passes --portable in the --check-first branch. Synthetic folders only. Exit 0 = PASS.

    python3 tools/release/test_portable_firstrun.py
"""
import io
import os
import shutil
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def run_interactive(root, dump, upd, portable_attr):
    a = types.SimpleNamespace(dump=dump, update=upd, eboot="", build=os.path.join(root, "b"), font_dir="-", font_dir_set=True,
                              check_first=False, rpcs3="off", shaders="off")
    if portable_attr != "unset":
        a.portable = portable_attr
    fr.interactive(a, root, "x.log", [], ask=lambda q: "", out=io.StringIO(), check=lambda *x: (0, "OK   D1: fine\n"))


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-pf-")
    try:
        dump, upd = os.path.join(tmp, "d"), os.path.join(tmp, "u")
        os.makedirs(dump)
        os.makedirs(upd)
        for mode, attr, want in (("unset keeps 1", None, "1"), ("attribute missing keeps 1", "unset", "1"),
                                 ("--portable sets 1", True, "1"), ("explicit False lowers", False, "0")):
            root = os.path.join(tmp, mode.replace(" ", "_"))
            os.makedirs(root)
            fr.write_install_ini(root, True, False)
            run_interactive(root, dump, upd, attr)
            got = fr.ini_parser(os.path.join(root, "kz3-install.ini")).get("install", "portable", fallback="")
            check(got == want, f"{mode}: portable={got!r}, want {want!r}")
        root = os.path.join(tmp, "unset_stays_unset")
        os.makedirs(root)
        run_interactive(root, dump, upd, None)
        cp = fr.ini_parser(os.path.join(root, "kz3-install.ini"))
        check(cp.get("install", "portable", fallback="") == "" and not la.read_ini(root)["portable"],
              "no stored value stays unset (AppData default)")
        # the launcher's check-first command carries the mode
        root = os.path.join(tmp, "cmd")
        os.makedirs(root)
        with open(os.path.join(root, "kz3-install.ini"), "w") as f:
            f.write(f"[paths]\ndump = {dump}\nupdate = {upd}\n")
        for portable in (True, False):
            cmd = la.firstrun_cmd(root, "first-build", {}, portable, "py")
            check("--check-first" in cmd and (("--portable" in cmd) == portable), f"check-first branch portable={portable}: {cmd}")
        # the game's keybinds.ini / display.ini follow the launcher's data folder (portable.txt included)
        root = os.path.join(tmp, "env")
        _, cwd, env, added = la.game_env(root, {"dump": "d", "eboot": "e"}, True, {})
        check(added.get("KZ3_KEYBINDS") == os.path.join(root, "keybinds.ini") and cwd == root, f"portable KZ3_KEYBINDS: {added}")
        _, cwd, env, added = la.game_env(root, {"dump": "d", "eboot": "e"}, False, {"APPDATA": os.path.join(tmp, "ad")},
                                         appdata=os.path.join(tmp, "ad"))
        check(added.get("KZ3_KEYBINDS") == os.path.join(cwd, "keybinds.ini") and cwd != root, f"AppData KZ3_KEYBINDS: {added}")
        _, _, env, added = la.game_env(root, {"dump": "d", "eboot": "e"}, True, {"KZ3_KEYBINDS": "x.ini"})
        check("KZ3_KEYBINDS" not in added and env["KZ3_KEYBINDS"] == "x.ini", "a set KZ3_KEYBINDS wins")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_portable_firstrun:", "FAIL " + ", ".join(FAILS) if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
