#!/usr/bin/env python3
"""Host test of tools/run-host-tests.sh (release pass 81, audit 1125 rows 10 and 13):
- list rules: a test listed in tools/host-tests.txt that is missing from the tree FAILS the run (also without the
  developer-only tools: the public tree); a test in the private list (host-tests-private.txt, never exported) that is
  missing is skipped;
- exit codes: a failing test makes the runner exit non-zero; a test that exits 77 is a SKIP (counted separately, named
  in the summary, does not fail by default); --max-skip / HOST_TESTS_MAX_SKIP and --skip-allow bound the skips for CI;
  (path, then "# reason", one per line).
Runs the script in a scratch tree. KZ3_RUN_HOST_TESTS_SCRIPT=<path> tests another copy of the script.
Exits 77 (SKIP) when bash is missing."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.environ.get("KZ3_RUN_HOST_TESTS_SCRIPT") or os.path.join(ROOT, "tools", "run-host-tests.sh")
bash = shutil.which("bash")
if not bash:
    print("SKIP: no bash on PATH")
    sys.exit(77)
if not os.path.isfile(SCRIPT):
    print(f"SKIP: {SCRIPT} not in this tree")
    sys.exit(77)

fails = []


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"  {detail}"))
    if not cond:
        fails.append(name)


def run(tree, lists, args=(), env=None):
    os.makedirs(os.path.join(tree, "tools"), exist_ok=True)
    shutil.copy(SCRIPT, os.path.join(tree, "tools", "run-host-tests.sh"))
    for nm, code in (("ok.py", 0), ("bad.py", 1), ("skip.py", 77)):
        with open(os.path.join(tree, "tools", nm), "w", newline="\n") as f:
            f.write(f"import sys\nsys.exit({code})\n")
    for name, text in lists.items():
        with open(os.path.join(tree, "tools", name), "w", newline="\n") as f:
            f.write(text)
    e = dict(os.environ, PATH=os.environ.get("PATH", ""), **(env or {}))
    for k in ("HOST_TESTS_MAX_SKIP", "HOST_TESTS_SKIP_ALLOW"):
        if k not in (env or {}):
            e.pop(k, None)
    p = subprocess.run([bash, os.path.join(tree, "tools", "run-host-tests.sh"), *args], capture_output=True,
                       text=True, cwd=tree, env=e)
    return p.returncode, p.stdout + p.stderr


with tempfile.TemporaryDirectory() as tmp:
    # no tools/agents/merge-lane.sh in the scratch tree: this is what the public tree looks like
    rc, out = run(os.path.join(tmp, "a"), {"host-tests.txt": "tools/ok.py\ntools/gone.py\n"})
    check("a listed test missing from the tree fails the run", rc != 0 and "gone.py" in out, f"rc={rc} {out[-200:]}")
    check("the summary names it", "gone.py" in out.splitlines()[-1], out[-200:])
    rc, out = run(os.path.join(tmp, "b"), {"host-tests.txt": "tools/ok.py\n"})
    check("every listed test present: pass", rc == 0 and "1/1 pass" in out, f"rc={rc} {out[-200:]}")
    rc, out = run(os.path.join(tmp, "c"), {"host-tests.txt": "tools/ok.py\n",
                                           "host-tests-private.txt": "tools/only-private.py\n"})
    check("a private-list test that is missing is skipped, not failed", rc == 0 and "only-private.py" in out, f"rc={rc} {out[-200:]}")
    rc, out = run(os.path.join(tmp, "d"), {"host-tests.txt": "tools/ok.py\n",
                                           "host-tests-private.txt": "tools/ok.py\n"})
    check("a private-list test that exists runs", rc == 0 and "2/2 pass" in out, f"rc={rc} {out[-200:]}")

    # a failing test fails the run
    rc, out = run(os.path.join(tmp, "e"), {"host-tests.txt": "tools/ok.py\ntools/bad.py\n"})
    check("a failing test makes the runner exit non-zero", rc != 0 and "FAIL tools/bad.py" in out and "1/2 pass" in out
          and "failed: tools/bad.py" in out.splitlines()[-1], f"rc={rc} {out[-300:]}")
    # exit 77 = SKIP: counted separately, named in the summary, does not fail by default
    rc, out = run(os.path.join(tmp, "f"), {"host-tests.txt": "tools/ok.py\ntools/skip.py\n"})
    check("a skipping test (exit 77) is a SKIP and does not fail the run", rc == 0 and "SKIP tools/skip.py" in out
          and "1/2 pass, 1 skipped (tools/skip.py)" in out.splitlines()[-1], f"rc={rc} {out[-300:]}")
    rc, out = run(os.path.join(tmp, "g"), {"host-tests.txt": "tools/skip.py\ntools/bad.py\n"})
    check("a skip does not hide a failure next to it", rc != 0 and "0/2 pass, 1 skipped" in out
          and "failed: tools/bad.py" in out, f"rc={rc} {out[-300:]}")
    rc, out = run(os.path.join(tmp, "h"), {"host-tests.txt": "tools/ok.py\ntools/skip.py\n"}, ("--max-skip", "0"))
    check("--max-skip 0 fails the run on a skip", rc != 0 and "more than 0 skipped" in out, f"rc={rc} {out[-300:]}")
    rc, out = run(os.path.join(tmp, "i"), {"host-tests.txt": "tools/ok.py\ntools/skip.py\n"}, (),
                  {"HOST_TESTS_MAX_SKIP": "1"})
    check("HOST_TESTS_MAX_SKIP=1 allows one skip", rc == 0, f"rc={rc} {out[-300:]}")
    tree = os.path.join(tmp, "j")
    os.makedirs(os.path.join(tree, "tools"))
    with open(os.path.join(tree, "tools", "allow.txt"), "w", newline="\n") as f:
        f.write("# known skips\ntools/other.py\n")
    rc, out = run(tree, {"host-tests.txt": "tools/ok.py\ntools/skip.py\n"}, ("--skip-allow", "tools/allow.txt"))
    check("a skip outside the allowlist fails the run", rc != 0 and "not in the allowlist: tools/skip.py" in out,
          f"rc={rc} {out[-300:]}")
    with open(os.path.join(tree, "tools", "allow.txt"), "a", newline="\n") as f:
        f.write("tools/skip.py\n")
    rc, out = run(tree, {"host-tests.txt": "tools/ok.py\ntools/skip.py\n"}, ("--skip-allow", "tools/allow.txt"))
    check("a skip inside the allowlist passes", rc == 0, f"rc={rc} {out[-300:]}")


print("FAIL: " + ", ".join(fails) if fails else "all run-host-tests checks passed")
sys.exit(1 if fails else 0)
