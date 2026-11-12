#!/usr/bin/env python3
"""Host test of tools/run-host-tests.sh's list rules (release pass 81): a test listed in tools/host-tests.txt that is
missing from the tree FAILS the run (also without the developer-only tools: the public tree); a test in the
private list (host-tests-private.txt, never exported) that is missing is skipped. Runs the script in a scratch tree.
KZ3_RUN_HOST_TESTS_SCRIPT=<path> tests another copy of the script. SKIP when bash is missing."""
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
    sys.exit(0)
if not os.path.isfile(SCRIPT):
    print(f"SKIP: {SCRIPT} not in this tree")
    sys.exit(0)

fails = []


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"  {detail}"))
    if not cond:
        fails.append(name)


def run(tree, lists):
    os.makedirs(os.path.join(tree, "tools"), exist_ok=True)
    shutil.copy(SCRIPT, os.path.join(tree, "tools", "run-host-tests.sh"))
    with open(os.path.join(tree, "tools", "ok.py"), "w", newline="\n") as f:
        f.write("import sys\nsys.exit(0)\n")
    for name, text in lists.items():
        with open(os.path.join(tree, "tools", name), "w", newline="\n") as f:
            f.write(text)
    p = subprocess.run([bash, os.path.join(tree, "tools", "run-host-tests.sh")], capture_output=True, text=True,
                       cwd=tree, env=dict(os.environ, PATH=os.environ.get("PATH", "")))
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
print("FAIL: " + ", ".join(fails) if fails else "all run-host-tests list checks passed")
sys.exit(1 if fails else 0)
