#!/usr/bin/env python3
"""The setup log merged into the install log carries no user name, in any form firstrun.py's redactor covers.

Two parts. (1) firstrun.merge_inno_log, which the installer runs through the bundled Python: real behaviour on a fake
Inno log. (2) installer/kz3.iss has no Inno test harness here, so this part is a STATIC check on the Pascal fallback
(used only when the bundled Python is missing): MergeInnoLog must prefer merge_inno_log and its fallback RedactUser must
name every form one by one (plain USERPROFILE only was the old code). It cannot prove the Pascal runs; an installer run
on Windows with a fake user folder is the real check (pending: it needs the VM)."""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
IISP = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def main():
    iss = open(IISP, encoding="utf-8").read()
    m = re.search(r"procedure MergeInnoLog;.*?\nend;\n", iss, re.S)
    merge = m.group(0) if m else ""
    # the Pascal redaction code: everything from the first redaction helper to the end of MergeInnoLog
    a = iss.find("function ReplaceNoCase")
    pas = iss[a:iss.find("end;\n", iss.find("procedure MergeInnoLog;")) + 5] if a >= 0 else merge
    check(bool(merge), "MergeInnoLog found")
    check("merge_inno_log" in merge, "the bundled Python (one redactor for every log) is tried first")
    for what, tok in (("backslash form", "'/', '\\'"), ("forward-slash form", "'\\', '/'"),
                      ("doubled backslashes", "'\\', '\\\\'"), ("mangled object-path form", "':', '_'"),
                      ("URL-encoded backslash %5C", "'%5C'"), ("URL-encoded slash %2F", "'%2F'"),
                      ("URL-encoded colon %3A", "'%3A'"), ("URL-encoded space %20", "'%20'"),
                      ("any letter case", "Lowercase("), ("other accounts under <drive>:\\Users", "'users'"),
                      ("replacement for other accounts", "'<user>'"), ("profile replacement", "'%USERPROFILE%'")):
        check(tok in pas, f"Pascal fallback names the {what}")
    for what, tok in (("install folder -> %INSTALL%", "'%INSTALL%'"), ("%LOCALAPPDATA% keeps its name", "'%LOCALAPPDATA%'"),
                      ("%APPDATA% keeps its name", "'%APPDATA%'"), ("other paths -> <path>\\last", "'<path>\\'"),
                      ("UNC paths", "Kind := 2"), ("Unix paths", "Kind := 3"), ("drive paths", "Kind := 1"),
                      ("virtual /dev_ paths are left", "'dev_'"), ("virtual /app_home paths are left", "'app_'"),
                      ("RedactLine applies every step to each line", "RedactLine(Lines[I]")):
        check(tok in iss, f"Pascal fallback: {what}")
    check("merge_inno_log(sys.argv[1], sys.argv[2], None, sys.argv[4])" in merge and "ExpandConstant('{app}') + '\"'" in merge,
          "the bundled-Python call passes the install folder ({app})")
    check("LoadStringsFromLockedFile(ExpandConstant('{log}')" in merge,
          "the fallback reads Inno's open log with shared access (LoadStringsFromLockedFile, Inno Setup 6.7.3)")
    check(not re.search(r"LoadStringsFromFile\(", merge), "no plain LoadStringsFromFile on the open log")
    fail = merge[merge.find("LoadStringsFromLockedFile"):]
    check("could not be read" in fail.split("Exit;")[0], "a failed read is written into the kept log, not skipped")
    check("StringChangeEx(Lines[I], Home" not in merge, "the plain-string-only replacement is gone")

    import firstrun as fr
    tmp = tempfile.mkdtemp(prefix="kz3-inno-")
    old = os.environ.get("USERPROFILE")
    os.environ["USERPROFILE"] = r"C:\Users\Zed"
    old_app = os.environ.get("APPDATA")
    os.environ["APPDATA"] = r"C:\Users\Zed\AppData\Roaming"
    try:
        inst = r"F:\Apps\KZ3 Port"
        src, dst = os.path.join(tmp, "inno.log"), os.path.join(tmp, "install.log")
        open(dst, "w").write("first run log\n")
        open(src, "w", encoding="utf-8").write(
            "Log opened\nplain C:\\Users\\Zed\\AppData\ndbl C:\\\\Users\\\\Zed\\\\x\n" + "fwd c:/users/zed/x\n" +
            "url C%3A%5CUsers%5CZed%5Cx\nmangled C_/Users/Zed/obj\nother E:\\Users\\bob\\Docs\nkept D:\\Games\\kz3\n" +
            "inst " + inst + "\\build\\kz3.exe and " + inst.replace("\\", "/") + "\n" + "near " + inst + "-old\\x\n" +
            "app " + r"C:\Users\Zed\AppData\Roaming" + "\\KZ3\\f" + "\n" +
            "mixed d:\\Games\\KZ3\\PS3_GAME\\USRDIR\\EBOOT.BIN, E:/PS3/BCUS98234\n" +
            "unc \\\\nas\\share\\Games\\x.bin proton Z:\\home\\al\\Games\\y.bin unix /mnt/games/Killzone 3/EBOOT.BIN\n" +
            "spaces D:\\Program Files (x86)\\My Games\\Killzone 3 (US)\\a.exe ok\n" +
            "virtual /dev_bdvd/PS3_GAME/x /dev_hdd0/game/y /app_home/z relative/dir/x\n")
        check(fr.merge_inno_log(src, dst, None, inst), "merge_inno_log appends")
        t = open(dst, encoding="utf-8").read()
        check("Zed" not in t and "zed" not in t and "bob" not in t, "no user name in the merged log")
        check("# --- Inno Setup log ---" in t and t.startswith("first run log") and "<path>\\kz3" in t,
              "header, earlier text and unrelated paths kept")
        for want in ("inst %INSTALL%\\build\\kz3.exe and %INSTALL%", "near <path>\\x", "app %APPDATA%\\KZ3\\f",
                     "mixed <path>\\EBOOT.BIN, <path>\\BCUS98234", "unc <path>\\x.bin proton <path>\\y.bin unix <path>\\EBOOT.BIN",
                     "spaces <path>\\a.exe ok", "virtual /dev_bdvd/PS3_GAME/x /dev_hdd0/game/y /app_home/z relative/dir/x"):
            check(want in t, "merged log has: " + want)
        check(not fr.merge_inno_log(os.path.join(tmp, "missing.log"), dst), "a missing Inno log is reported, not raised")
    finally:
        if old_app is None:
            os.environ.pop("APPDATA", None)
        else:
            os.environ["APPDATA"] = old_app
        if old is None:
            os.environ.pop("USERPROFILE", None)
        else:
            os.environ["USERPROFILE"] = old
    print("test_inno_log_redact: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
