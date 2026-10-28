#!/usr/bin/env python3
"""Host test (release pass 66): installer/kz3.iss' update page and firstrun_check U3 agree on what the 1.14 update must hold.
Static checks (no ISCC, no game data):
- UpdateOK requires PARAM.SFO, USRDIR\\EBOOT.BIN and the data archive named by firstrun_check.UPDATE_EDAT (one name in both);
- UpdateProblem names the missing data archive; the page box and the silent-install refusal list it as needed;
- firstrun_check's update check fails (U3) when the archive is missing, and its message names the file;
- the same three file names are the ones firstrun_check's folder recogniser and error text use."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ISS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
if not os.path.isfile(ISS):
    print("SKIP: installer/kz3.iss not in this tree")
    sys.exit(0)
import firstrun_check as fc  # noqa: E402

t = open(ISS, encoding="utf-8").read()
code = t[t.find("[Code]"):]
src = open(os.path.join(HERE, "firstrun_check.py"), encoding="utf-8").read()
bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


def func(name):
    m = re.search(r"(?:function|procedure) " + name + r"\b.*?\nend;", code, re.S)
    return m.group(0) if m else ""


name = fc.UPDATE_EDAT
check(name == "added_data_ps3.psarc.edat", "firstrun_check.UPDATE_EDAT is the update's data archive")
uok = func("UpdateOK")
check("USRDIR\\" + name in uok and "PARAM.SFO" in uok and "USRDIR\\EBOOT.BIN" in uok and uok.count("FileExists(") == 3,
      "UpdateOK requires PARAM.SFO, EBOOT.BIN and " + name)
up = func("UpdateProblem")
check("USRDIR\\" + name in up and "Miss := Miss + 'USRDIR\\" + name + "'" in up, "UpdateProblem names the missing " + name)
check(code.count("USRDIR\\" + name) >= 5 and "PARAM.SFO, USRDIR\\EBOOT.BIN and USRDIR\\" + name in code,
      "the update box lists the file as needed")
check("USRDIR\\EBOOT.BIN and USRDIR\\" + name + " are needed" in code, "the silent-install refusal lists the file as needed")
# firstrun_check: the same requirement, as a failure with the reason
m = re.search(r'r\.(ok|warn|fail)\("U3", f"update data archive present.*?\n    else:\n        r\.(warn|fail)\("U3"', src, re.S)
check(m is not None and m.group(2) == "fail", "firstrun_check U3 fails (not warns) when the archive is missing")
check('{UPDATE_EDAT}. It is the update' in src and "menu screens" in src, "the U3 message names the file and what it holds")
check(src.count("UPDATE_EDAT") >= 5, "firstrun_check's recogniser and texts use the same name")

print("test_install_update_page:", "PASS" if not bad else f"FAIL ({bad})")
sys.exit(1 if bad else 0)
