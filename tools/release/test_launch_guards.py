#!/usr/bin/env python3
"""Launcher guards (audit items): launcher.log is redacted like the install log; an emptied or replaced update folder
is named by decide(); a killed move's leftover <name>.copying is cleaned; build/paths.json is replaced atomically and
the move with an underivable old folder is handled. No game data."""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402
import test_firstrun_check as tfc  # noqa: E402

FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def write(p, t="x"):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(t)


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-guards-")
    old_env = os.environ.get("USERPROFILE")
    try:
        try:
            # 1. launcher.log carries no user name
            os.environ["USERPROFILE"] = r"C:\Users\Zed"
            root = os.path.join(tmp, "root")
            la.note(root, r"dump C:\Users\Zed\Games, json C:\\Users\\Zed\\x, url C%3A%5CUsers%5CZed, other D:\Users\bob\y")
            txt = open(os.path.join(root, "logs", "launcher.log")).read()
            check("Zed" not in txt and "bob" not in txt, f"1 launcher.log has no user name: {txt.strip()[:120]}")
            check("%USERPROFILE%" in txt and "<path>" in txt, "1 the replacements are named")

        except Exception as e:  # noqa: BLE001
            check(False, '1 raised ' + repr(e)[:100])
        try:
            # 1b. Proton: the Unix home is printed as Z:/home/<name> while USERPROFILE is C:\users\steamuser
            os.environ["USERPROFILE"] = r"C:\users\steamuser"
            red = fr.redactor()
            forms = ["Z:/home/Alice/Games/x", r"Z:\home\Alice\x", r"Z:\\home\\Alice\\x", "/home/Alice/.local", "y:/home/Alice/x",
                     "Z%3A%5Chome%5CAlice%5Cx", "%2Fhome%2FAlice%2Fx", "file:///home/Alice/x", "Z_/home/Alice/obj",
                     r"C:\users\steamuser\AppData"]
            gone = [t for t in forms if "Alice" in red(t) or "steamuser" in red(t)]
            check(not gone, f"1b console/log redactor removes the Unix home name in every form {gone}")
            check(red("/mnt/data/home/Carol/x") == "<path>\\x", "1b a /home inside a longer path: last component only")
            la.note(root, "dump Z:/home/Alice/Games and /home/Alice/x")
            txt = open(os.path.join(root, "logs", "launcher.log")).read()
            check("Alice" not in txt.split("\n")[-2], "1b launcher.log: no Unix user name")

        except Exception as e:  # noqa: BLE001
            check(False, '1b raised ' + repr(e)[:100])
        try:
            # 1c. the support package (fr.redactor) carries no Unix user name from any file it collects
            import support
            import zipfile
            sroot = os.path.join(tmp, "sup")
            write(os.path.join(sroot, "logs", "launcher.log"), "dump Z:/home/Alice/Games\n")
            write(os.path.join(sroot, "Killzone3-PC-setup.log"), "x /home/Alice/y Z:\\\\home\\\\Alice\n")
            dest, _ = support.build(sroot, sroot, os.path.join(tmp, "sout"), fr.redactor(), "20261029-000000")
            z = zipfile.ZipFile(dest)
            alltext = " ".join(z.read(n).decode("utf-8", "replace") for n in z.namelist())
            check("Alice" not in alltext and "<path>" in alltext, "1c support package: no Unix user name in any collected file")
        except Exception as e:  # noqa: BLE001
            check(False, '1c raised ' + repr(e)[:100])
        try:
        # 3. the update folder still holds the update
            upd = tfc.make_update(os.path.join(tmp, "u"))
            check(la.update_problem(upd) == "", "3 a complete update: no problem")
            check(la.update_problem("") == "" and la.update_problem(os.path.join(tmp, "gone")) == "",
                  "3 no folder recorded or folder gone: not this check's case (decide names it)")
            os.remove(os.path.join(upd, "USRDIR", tfc.fc.UPDATE_EDAT))
            m = la.update_problem(upd)
            check("no longer holds the update" in m and "Pick" in m and "added_data" in m, f"3 data archive gone: {m[:110]}")
            shutil.rmtree(upd)
            os.makedirs(upd)
            m = la.update_problem(upd)
            check("PARAM.SFO" in m, f"3 emptied folder: {m[:110]}")
            root2 = os.path.join(tmp, "r2")
            dump = os.path.join(tmp, "dump")
            write(os.path.join(dump, "PS3_GAME", "PARAM.SFO"))
            eb = os.path.join(tmp, "EBOOT.elf")
            write(eb)
            write(os.path.join(root2, "build", "paths.json"), json.dumps({"dump": dump, "update": upd, "eboot": eb}))
            a, why = la.decide(root2)
            check(a == "missing-inputs" and "Pick the folder" in why, f"3 decide() names the fix: {a} {why[:80]}")

        except Exception as e:  # noqa: BLE001
            check(False, '3 raised ' + repr(e)[:100])
        try:
            # 4a. a leftover <name>.copying from a killed copy
            src, dst = os.path.join(tmp, "s"), os.path.join(tmp, "d")
            write(os.path.join(src, "a.bin"), "AAAA")
            write(os.path.join(src, "dirx", "f"), "F")
            write(os.path.join(dst, "a.bin.copying"), "half")
            write(os.path.join(dst, "dirx.copying", "stale"), "old")
            ok, msg = la.move_entries(["a.bin", "dirx"], src, dst)
            check(ok and open(os.path.join(dst, "a.bin")).read() == "AAAA" and os.listdir(os.path.join(dst, "dirx")) == ["f"]
                  and not os.path.exists(os.path.join(dst, "a.bin.copying")), f"4 leftover copying cleaned, move done: {ok} {msg}")

        except Exception as e:  # noqa: BLE001
            check(False, '4a raised ' + repr(e)[:100])
        try:
            # 4b. paths.json: replaced atomically; old folder derivable from the build folder or the stamp
            calls = []
            real = os.replace
            os.replace = lambda a, b: (calls.append((a, b)), real(a, b))[1]
            try:
                R = os.path.join(tmp, "new", "Helghast")
                os.makedirs(os.path.join(R, "build", "obj"))
                O = os.path.join(tmp, "old", "Helghast")
                json.dump({"dump": os.path.join(O, "game", "disc"), "update": upd}, open(os.path.join(R, "build", "paths.json"), "w"))
                write(os.path.join(R, "build", "obj", "CMakeCache.txt"),
                      f"CMAKE_CACHEFILE_DIR:INTERNAL={O}/build/obj\n")   # no source folder line: the build folder derives it
                old = fr.heal_moved_paths(R, os.path.join(R, "build"))
                pj = fr._la().load_json(os.path.join(R, "build", "paths.json"))
                check(os.path.normpath(old) == os.path.normpath(O) and pj["dump"] == os.path.join(R, "game", "disc"),
                      f"4 old folder derived from the cache's build folder, path follows: {old}")
                check(any(b.endswith("paths.json") and a.endswith("paths.json.tmp") for a, b in calls),
                      "4 paths.json is written aside and swapped in (os.replace)")
                check(not os.path.exists(os.path.join(R, "build", "paths.json.tmp")), "4 no .tmp left")
                # old folder not derivable: nothing rewritten, no crash, decide still names what is gone
                R2 = os.path.join(tmp, "n2", "H")
                os.makedirs(os.path.join(R2, "build", "obj"))
                gone = os.path.join(tmp, "elsewhere", "disc")
                before = json.dumps({"dump": gone, "update": upd})
                open(os.path.join(R2, "build", "paths.json"), "w").write(before)
                write(os.path.join(R2, "build", "obj", "CMakeCache.txt"), "CMAKE_CACHEFILE_DIR:INTERNAL=/odd/place\n")
                old2 = fr.heal_moved_paths(R2, os.path.join(R2, "build"))
                check(old2 == "" and open(os.path.join(R2, "build", "paths.json")).read() == before,
                      "4 old folder not derivable: paths.json untouched")
                a, why = la.decide(R2)
                check(a == "missing-inputs" and gone in why, f"4 ... and decide() names the missing folder: {a}")
            finally:
                os.replace = real
        except Exception as e:  # noqa: BLE001
            check(False, '4b raised ' + repr(e)[:100])
    finally:
        if old_env is None:
            os.environ.pop("USERPROFILE", None)
        else:
            os.environ["USERPROFILE"] = old_env
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_launch_guards: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
