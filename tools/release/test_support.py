#!/usr/bin/env python3
"""Host test for tools/release/support.py (the support package) on synthetic folders. Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_support.py

Cases: the zip holds the newest 3 game logs, first-run/launcher logs, kz3-install.ini, display.ini, keybinds.ini and
facts.txt; user names are redacted (plain and CMake-mangled); game-data names and non-text extensions are refused;
files outside the install and data folders are not read; the size cap shortens the oldest lines first and facts.txt
says so; missing files are tolerated; nothing to collect -> no zip; launch.py --support-package end to end.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402
import names  # noqa: E402
import support as sp  # noqa: E402
import support_text as T  # noqa: E402

BAD = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        BAD.append(what)


def write(p, s, age=0):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(s)
    t = time.time() - age
    os.utime(p, (t, t))


class UI:
    def __init__(self):
        self.said, self.opened = [], []

    def info(self, t):
        self.said.append(t)

    def open_folder(self, p):
        self.opened.append(p)


def red(s):
    return s.replace("C:\\Users\\Bob\\", "C:\\Users\\<user>\\").replace("C__Users_Bob_", "C__Users_<user>_")


def main():
    with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as zs:
        root, data, out = (os.path.join(tmp, d) for d in ("inst", "data", "out"))
        for i in range(5):
            write(os.path.join(data, "logs", f"Killzone3-PC-{i}.log"), f"run {i}\nC:\\Users\\Bob\\x\nadapter: NVIDIA GeForce RTX 4070\n", age=100 - i)
        write(os.path.join(root, "VERSION.txt"), "1.2.3\n")
        write(os.path.join(root, "manifest.json"), json.dumps({"repo_commit": "abc123"}))
        write(os.path.join(root, "build", "paths.json"), json.dumps({"facts": {"eboot_elf": {"version": "1.14"}}}))
        write(os.path.join(root, names.FIRSTRUN_LOG), "first C__Users_Bob_build\n")
        write(os.path.join(root, "kz3-install.ini"), "[install]\ndump=C:\\Users\\Bob\\dump\n")
        write(os.path.join(data, "display.ini"), "mode=1\n")
        write(os.path.join(data, "keybinds.ini"), "a=b\n")
        write(os.path.join(root, "logs", "launcher.log"), "start\n")
        write(os.path.join(data, "gamedata", "SAVE.sav"), "secret")
        write(os.path.join(root, "EBOOT.elf"), "x")
        dest, skipped = sp.build(root, data, out, red, "20261022-120000")
        check(os.path.basename(dest) == f"{names.FILE_BASE}-support-20261022-120000.zip", "1 zip name is FILE_BASE-support-date")
        z = zs.enter_context(zipfile.ZipFile(dest))   # closed before the temp folder goes (Windows cannot delete an open file)
        n = set(z.namelist())
        check({"facts.txt", "kz3-install.ini", "display.ini", "keybinds.ini", "launcher.log", names.FIRSTRUN_LOG} <= n, "1 listed files present")
        check(sum(x.startswith("logs/") for x in n) == 3 and "logs/Killzone3-PC-4.log" in n and "logs/Killzone3-PC-0.log" not in n,
              "2 newest 3 game logs")
        allt = "".join(z.read(x).decode() for x in n)
        check("Bob" not in allt and "<user>" in allt, "3 user names redacted (plain and mangled)")
        check(not any("sav" in x.lower() or "eboot" in x.lower() for x in n), "4 no saves or EBOOT")
        f = z.read("facts.txt").decode()
        check("1.2.3" in f and "abc123" in f and "1.14" in f and "RTX 4070" in f and "No log was shortened" in f, "5 facts.txt content")
        rr = fr.redactor()("C:\\Users\\Bob\\dump C_/Users/Bob/build/x")
        check("Bob" not in rr and rr.count("<user>") == 2, f"5b real firstrun redactor: {rr}")
        # refused names / extensions
        check(all(sp.refused(x) for x in ("EBOOT.elf", "EBOOT.BIN", "PARAM.SFO", "a.psarc", "x.sav", "a.rap", "kz3.exe", "a.bin")), "6 refusals")
        check(not any(sp.refused(x) for x in ("a.log", "b.ini", "c.txt")), "6 text names pass")
        # outside folders
        check(not sp.inside(os.path.join(tmp, "elsewhere", "x.log"), root, data) and sp.inside(os.path.join(root, "a.log"), root, data), "7 inside check")
        # cap
        big = os.path.join(data, "logs", "Killzone3-PC-9.log")
        write(big, "".join(f"line {i:06d}\n" for i in range(5000)), age=1)
        dest, _ = sp.build(root, data, out, red, "cap", cap=20000)
        z = zs.enter_context(zipfile.ZipFile(dest))   # closed before the temp folder goes (Windows cannot delete an open file)
        t = z.read("logs/Killzone3-PC-9.log").decode()
        total = sum(len(z.read(x)) for x in z.namelist())
        check(total <= 20000 and "line 004999" in t and "line 000000" not in t and t.startswith("[earlier"), f"8 cap: oldest lines first ({total} bytes)")
        check("shortened" in z.read("facts.txt").decode() and "Killzone3-PC-9.log" in z.read("facts.txt").decode(), "8 facts.txt says so")
        # missing everything
        d2, _ = sp.build(os.path.join(tmp, "none"), os.path.join(tmp, "none2"), out, red, "z")
        check(d2 == "", "9 nothing to collect -> no zip")
        # launch.py end to end
        ui, env = UI(), {"APPDATA": os.path.join(tmp, "ad"), "USERPROFILE": tmp}
        # launch.user_folder asks the real Windows known folder first (by design): off here, so the env dict above is
        # the user folder and the test never reads or writes the real %APPDATA%
        real_known, la.known_folder = la.known_folder, lambda name: ""
        write(os.path.join(root, "kz3-install.ini"), "[install]\nportable=1\n")
        write(os.path.join(root, "logs", "Killzone3-PC-1.log"), "hello\n")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = la.main(["--root", root, "--support-package", "--out", out], ui=ui, environ=env)
        check(rc == 0 and ui.opened == [out] and "support-" in ui.said[0] and la.ISSUES_URL in ui.said[0], "10 launch.py --support-package")
        ui = UI()
        rc = la.main(["--root", os.path.join(tmp, "empty"), "--support-package", "--out", out], ui=ui, environ=env)
        la.known_folder = real_known
        check(rc == 1 and ui.said == [T.NOTHING], "10 empty install says so")
    print("FAIL" if BAD else "PASS all")
    return 1 if BAD else 0


if __name__ == "__main__":
    sys.exit(main())
