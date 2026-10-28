#!/usr/bin/env python3
"""Host test for firstrun_check.py's T1 (pass 65): a PC clock behind the install's files. Synthetic folders only.
Cases: files dated now pass (control); one file 2 days ahead FAILs with both times and the Settings path; 2 minutes ahead
passes (slack); future files in build/ and game/ are not looked at; the file cap is kept; an empty root checks nothing;
the REASON line reaches facts["reasons"] through the CLI. Run: python3 tools/release/test_clock_check.py
"""
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun_check as fc  # noqa: E402

bad = 0


def check(ok, what):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + what)


def touch(path, t):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write("x")
    os.utime(path, (t, t))


def t1(root, **kw):
    r = fc.Report()
    fc.check_clock(r, root, **kw)
    return [c for c in r.checks if c["code"] == "T1"]


with tempfile.TemporaryDirectory() as td:
    now = time.time()
    touch(os.path.join(td, "tools", "a.py"), now - 100)
    touch(os.path.join(td, "b.txt"), now)
    c = t1(td)
    check(len(c) == 1 and c[0]["level"] == "OK", "correct clock passes (control)")
    touch(os.path.join(td, "tools", "a.py"), now + 120)
    check(t1(td)[0]["level"] == "OK", "2 minutes ahead is within the slack")
    touch(os.path.join(td, "build", "x.o"), now + 9 * 86400)
    touch(os.path.join(td, "game", "y"), now + 9 * 86400)
    check(t1(td)[0]["level"] == "OK", "future files under build/ and game/ are not looked at")
    touch(os.path.join(td, "tools", "a.py"), now + 2 * 86400)
    c = t1(td)
    m = c[0]["message"] if c else ""
    check(c and c[0]["level"] == "FAIL" and "PC clock" in m and "newest" in m and "Set time automatically" in m,
          "a file two days ahead FAILs with the reason: " + m[:80])
    check(t1(td, now=now + 3 * 86400)[0]["level"] == "OK", "same tree with the clock set right passes")
    check(t1(os.path.join(td, "none")) == [], "a missing root checks nothing")
    n = 0
    for i in range(30):
        touch(os.path.join(td, "many", f"f{i}"), now)
    _, seen = fc.newest_mtime(td, max_files=10)
    check(seen == 10, f"the file cap is kept ({seen})")
    # through the CLI: the reason line is in facts["reasons"] and the run fails
    import json, subprocess
    p = subprocess.run([sys.executable, os.path.join(HERE, "firstrun_check.py"), "--dump", os.path.join(td, "nodump"),
                        "--root", td, "--json", "--skip-archive-hash"], capture_output=True, text=True)
    j = json.loads(p.stdout)
    check(any("PC clock" in x for x in j["facts"].get("reasons", [])), "the CLI shows the T1 reason in facts[reasons]")
print("clock_check:", "PASS" if not bad else "FAIL")
sys.exit(1 if bad else 0)
