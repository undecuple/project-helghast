#!/usr/bin/env python3
"""switch_data edge cases from the audit: S1 a portable.txt marker that disagrees with kz3-install.ini must not
produce the "%APPDATA% is not available" message (both sides are the install folder; only the marker is missing).
S2 when recording the new mode fails after the data AND the caches moved, both go back and the message says so."""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def write(p, t="x"):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(t)


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-sw-")
    try:
        root, ad, lad = (os.path.join(tmp, n) for n in ("root", "Roaming", "Local"))
        env = {"APPDATA": ad, "LOCALAPPDATA": lad}
        S = la.SAVE_FOLDER
        # S1: ini says portable, the marker file is missing, appdata exists
        os.makedirs(root)
        fr.write_install_ini(root, True, False)
        write(os.path.join(root, "keybinds.ini"), "keys")
        ok, m = la.switch_data(root, True, env)
        check("not available" not in m, f"S1 no APPDATA message for a marker mismatch: {m!r}")
        check(ok and os.path.isfile(os.path.join(root, la.PORTABLE_FILE)) and la.read_ini(root)["portable"]
              and os.path.isfile(os.path.join(root, "keybinds.ini")), f"S1 the marker is created, data stays: {ok} {m!r}")
        check(la.switch_data(root, True, env) == (False, "Already portable."), "S1 then it is simply already portable")
        # the genuine case keeps its message: AppData wanted but not available
        r2 = os.path.join(tmp, "r2")
        os.makedirs(r2)
        fr.write_install_ini(r2, True, False)
        write(os.path.join(r2, la.PORTABLE_FILE), "")
        ok, m = la.switch_data(r2, False, {})
        check(not ok and "not available" in m, f"S1 control: no APPDATA at all keeps the message: {m!r}")

        # S2: data and caches moved, then the mode cannot be recorded
        r3 = os.path.join(tmp, "r3")
        os.makedirs(r3)
        fr.write_install_ini(r3, False, False)
        write(os.path.join(ad, S, "gamedata", "SAVE", "P"), "save")
        write(os.path.join(lad, S, "cache", "shaders", "k.bin"), "pso")
        real = la.write_ini
        la.write_ini = lambda *a, **k: (_ for _ in ()).throw(OSError("read-only file system"))
        try:
            ok, m = la.switch_data(r3, True, env)
        finally:
            la.write_ini = real
        check(not ok and "unchanged" in m, f"S2 refused with the old state named: {m!r}")
        check(os.path.isfile(os.path.join(ad, S, "gamedata", "SAVE", "P")) and not os.path.exists(os.path.join(r3, "gamedata")),
              "S2 the saves are back in AppData")
        check(os.path.isfile(os.path.join(lad, S, "cache", "shaders", "k.bin")) and not os.path.exists(os.path.join(r3, "cache")),
              "S2 the caches are back too (the message says files unchanged)")
        check(not os.path.exists(os.path.join(r3, la.PORTABLE_FILE)), "S2 no marker left")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_switch_data: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
