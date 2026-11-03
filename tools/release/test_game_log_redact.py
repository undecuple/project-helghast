#!/usr/bin/env python3
"""The game's own session log (logs/Killzone3-PC-*.log) and crash report hold no user name.

The log is written by the C runtime (port/kz3/kz3_log.c), not by the launcher, so the redactor is C
(port/kz3/kz3_log_redact.h) with the same rules as firstrun.py redactor(). The change is kept as
tools/release/kz3-log-redact.patch until it is applied to port/kz3; this test builds the runtime logger
(host cc) from a scratch copy of port/kz3 with the patch applied when the header is not there yet.

  python3 tools/release/test_game_log_redact.py            checks the tree plus the patch (passes)
  python3 tools/release/test_game_log_redact.py --as-is    checks port/kz3 exactly as it is

SKIP when no C compiler is on PATH."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PATCH = os.path.join(ROOT, "tools", "release", "kz3-log-redact.patch")

DRIVER = r'''
#include "kz3_log.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    if (kz3_log_init(argv[1], KZ3_LOG_F_NO_ENV) != 0) return 3;
    FILE* f = fopen(argv[2], "rb"); char l[1024];
    while (f && fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; KZ3_LOGI("test", "path %s end", l); }
    if (f) fclose(f);
    kz3_log_write_crash("reason C:\\Users\\Zed\\r", "details C:/Users/Zed/d\nsecond E:\\Users\\bob\\d Z:/home/Alice/c");
    printf("%s\n", kz3_log_file_path());
    kz3_log_shutdown();
    return 0;
}
'''

CASES = [  # (line, text that must NOT survive)
    (r"C:\Users\Zed\Games\dump", "Zed"),
    (r"C:\\Users\\Zed\\Games", "Zed"),
    ("C:/Users/Zed/Games", "Zed"),
    ("C%3A%5CUsers%5CZed%5CGames", "Zed"),
    ("file:///C:/Users/Zed/Games", "Zed"),
    ("C_/Users/Zed/obj", "Zed"),
    (r"c:\users\zed\Games", "zed"),
    (r"E:\Users\bob\Documents", "bob"),
    (r"D:\\Users\\bob smith\\x", "bob"),
    (r"D:\Profiles\Qux Name\x", "Qux"),        # a profile outside a Users folder (USERPROFILE below)
    # the Unix home as Proton prints it (USERPROFILE there is C:\users\steamuser)
    ("Z:/home/Alice/Games/x", "Alice"),
    (r"Z:\home\Alice\x", "Alice"),
    (r"y:\\home\\Alice\\x", "Alice"),
    ("/home/Alice/.local/share", "Alice"),
    ("Z%3A%5Chome%5CAlice%5Cx", "Alice"),
    ("file:///home/Alice/x", "Alice"),
    ("%2Fhome%2FAlice%2Fx", "Alice"),
    ("Z_/home/Alice/obj", "Alice"),
]
KEEP = ["relative/Users/readme", "/dev_hdd0/game/BCUS98234/USRDIR", "/dev_bdvd/PS3_GAME/x /app_home/y", "build\\logs\\y.txt"]   # unchanged
INSTALL = r"F:\Apps\KZ3 Port"
APPDATA = r"D:\Profiles\Qux Name\AppData\Roaming"
EXPECT = [  # (line, line as the log must hold it): the path rule of pathrules.py (2026-11-02)
    (r"D:\Games\KZ3\PS3_GAME\USRDIR\EBOOT.BIN", r"<path>\EBOOT.BIN"),
    ("d:/Games/KZ3/x.bin", r"<path>\x.bin"),                                  # mixed case drive, forward slashes
    (r"e:\PS3\BCUS98234", r"<path>\BCUS98234"),
    (r"\\nas\share\Games\f.bin", r"<path>\f.bin"),                           # UNC
    (r"Z:\mnt\games\KZ3\x.bin", r"<path>\x.bin"),                             # Proton Z:
    ("/mnt/games/KZ3/EBOOT.BIN", r"<path>\EBOOT.BIN"),
    ("/mnt/data/home/Carol/x", r"<path>\x"),
    (r"opened D:\Program Files (x86)\My Games\Killzone 3 (US)\a.exe ok", r"opened <path>\a.exe ok"),
    (INSTALL, "%INSTALL%"),                                                    # a path equal to the install folder
    (INSTALL + r"\build\kz3.exe", r"%INSTALL%\build\kz3.exe"),
    (INSTALL.replace("\\", "/") + "/logs/a.log", "%INSTALL%/logs/a.log"),
    (INSTALL + r"-old\x", r"<path>\x"),                                       # a longer folder name is not the install
    (APPDATA + r"\KZ3\shaders", r"%APPDATA%\KZ3\shaders"),
    (r"D:\Profiles\Qux Name\Saved Games\f", r"%USERPROFILE%\Saved Games\f"),
    (r"E:\Users\bob\Documents\f.txt", r"<path>\f.txt"),
]


def build(src, out):
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        return None
    exe = os.path.join(out, "drv")
    with open(os.path.join(out, "drv.c"), "w") as f:
        f.write(DRIVER)
    r = subprocess.run([cc, "-std=c11", "-O1", "-I", src, os.path.join(out, "drv.c"), os.path.join(src, "kz3_log.c"),
                        "-lpthread", "-o", exe], capture_output=True, text=True)
    if r.returncode:
        print("build failed:\n" + r.stderr[-1500:])
        sys.exit(1)
    return exe


def main():
    as_is = "--as-is" in sys.argv
    tmp = tempfile.mkdtemp(prefix="kz3redact")
    src = os.path.join(tmp, "port", "kz3")
    os.makedirs(src)
    for n in ("kz3_log.c", "kz3_log.h", "kz3_log_redact.h"):
        p = os.path.join(ROOT, "port", "kz3", n)
        if os.path.exists(p):
            shutil.copy(p, src)
    if not as_is and not os.path.exists(os.path.join(src, "kz3_log_redact.h")):
        r = subprocess.run(["git", "apply", "-p1", PATCH], cwd=tmp, capture_output=True, text=True)
        if r.returncode:
            print("[FAIL] the patch does not apply:", r.stderr[-300:])
            return 1
    exe = build(src, tmp)
    if exe is None:
        print("SKIP: no C compiler")
        return 0
    inp = os.path.join(tmp, "lines.txt")
    with open(inp, "w") as f:
        f.write("\n".join([c[0] for c in CASES] + KEEP + [e[0] for e in EXPECT]) + "\n")
    env = dict(os.environ, USERPROFILE=r"D:\Profiles\Qux Name", KZ3_INSTALL_DIR=INSTALL, APPDATA=APPDATA)
    logdir = os.path.join(tmp, "out")
    os.makedirs(logdir)
    r = subprocess.run([exe, logdir, inp], capture_output=True, text=True, env=env)
    if r.returncode:
        print("driver failed", r.returncode, r.stderr[-300:])
        return 1
    log = open(r.stdout.strip(), errors="replace").read()
    crash = ""
    ld = os.path.join(logdir, "logs")
    for n in os.listdir(ld):
        if "crash" in n:
            crash = open(os.path.join(ld, n), errors="replace").read()
    bad = 0
    for line, user in CASES:   # made-up user names
        # the home-folder case needs USERPROFILE (D:\Profiles\Qux Name); the rest hold with any
        ok = user not in log
        print(("[PASS] " if ok else "[FAIL] ") + f"log has no {user!r} from {line!r}")
        bad += not ok
    for k in KEEP:
        ok = k in log
        print(("[PASS] " if ok else "[FAIL] ") + f"unrelated text kept: {k!r}")
        bad += not ok
    for line, want in EXPECT:
        ok = "path " + want + " end" in log
        print(("[PASS] " if ok else "[FAIL] ") + f"{line!r} -> {want!r}")
        bad += not ok
    for user in ("Zed", "bob", "Qux", "Alice"):   # made-up user names from the test paths
        ok = user not in crash and bool(crash)
        print(("[PASS] " if ok else "[FAIL] ") + f"crash report has no {user!r}")
        bad += not ok
    ok = "%USERPROFILE%" in log
    print(("[PASS] " if ok else "[FAIL] ") + "the profile folder is named %USERPROFILE%")
    bad += not ok
    shutil.rmtree(tmp, ignore_errors=True)
    print("test_game_log_redact: " + ("FAILED %d" % bad if bad else "all passed"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
