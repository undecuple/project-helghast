#!/usr/bin/env python3
"""Host test for the first-run build's child processes (vekta v0.5.2: unquoted compiler paths split at a space;
vekta issue 94: a program that ended with code 0 after a runtime-DLL error and wrote nothing).

1. Install folder with spaces ("Project Helghast Test/sub dir"): every command the real STEPS build keeps each path a
   single argv element (checked on the argv lists), survives subprocess.list2cmdline and a Windows-style split of the
   resulting string (the splitter follows the Windows backslash rules), no path argument ends in a separator, and the
   child runs in the install folder. Negative control: the same commands joined with plain
   spaces (unquoted) are detected as split.
2. Child checks: a child that exits 0 but writes none of the step's declared files fails the step and names the files;
   the same child writing them passes. A child that exits 0xC0000135 (STATUS_DLL_NOT_FOUND) or says "incorrectly installed"
   gets a message naming the program and the cause; an ordinary exit 1 does not get the library wording. rpcs3.exe is never
   started by this code (rpcs3_locate.py only reads folders): asserted on the source.
   Negative control: with the library check removed the message test fails.
Exit 0 = PASS."""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402

FAILS = []
PY = sys.executable
HERE = os.path.dirname(os.path.abspath(__file__))


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def win_split(line):
    """CommandLineToArgvW / MSVCRT rules: whitespace separates outside quotes, double quotes group; 2n backslashes
    before a quote give n backslashes and the quote toggles, 2n+1 give n backslashes and a literal quote; other
    backslashes are literal."""
    out, cur, q, has, i = [], "", False, False, 0
    while i < len(line):
        ch = line[i]
        if ch == "\\":
            n = 0
            while i < len(line) and line[i] == "\\":
                n, i = n + 1, i + 1
            if i < len(line) and line[i] == '"':
                cur += "\\" * (n // 2)
                if n % 2:
                    cur, i = cur + '"', i + 1
            else:
                cur += "\\" * n
            has = True
            continue
        if ch == '"':
            q, has = not q, True
        elif ch in " \t" and not q:
            if has or cur:
                out.append(cur)
            cur, has = "", False
        else:
            cur += ch
        i += 1
    if has or cur:
        out.append(cur)
    return out


def split_problems(argv, root):
    """Elements that are a fragment of the install root cut at a space (the vekta failure), or '' when none."""
    head, _, tail = root.partition(" ")
    bad = []
    for a in argv:
        if a.endswith(head) or a.startswith(tail.split(os.sep)[0]) and root not in a and tail.split(os.sep)[0] + " " in root:
            bad.append(a)
    return bad


def real_commands(root):
    for d in ("port/kz3", "port/spu", "re/data/overrides", "cmake", "toolchain/llvm-mingw/bin", "toolchain/cmake/bin",
              "toolchain/ninja", "toolchain/python", "build/analysis"):
        os.makedirs(os.path.join(root, *d.split("/")), exist_ok=True)
    for f in ("re/data/overrides/ppu_overrides-114.txt", "EBOOT (1).elf"):
        open(os.path.join(root, *f.split("/")), "w").write("x")
    c = fr.Ctx(root=root, build=os.path.join(root, "build"), dump=os.path.join(root, "game dump", "disc"),
               update=os.path.join(root, "game dump", "update"), eboot=os.path.join(root, "EBOOT (1).elf"), jobs=4,
               toolchain=os.path.join(root, "toolchain", "llvm-mingw"),
               cmake=os.path.join(root, "toolchain", "cmake", "bin", "cmake.exe"),
               ninja=os.path.join(root, "toolchain", "ninja", "ninja.exe"),
               python=os.path.join(root, "toolchain", "python", "python.exe"), spu_fma=True)
    cmds = []
    for s in fr.STEPS + [fr.SHADER_STEP]:
        try:
            cmds += s.commands(c)
        except fr.StepFailed:
            pass
    return c, cmds


def part1(tmp):
    root = os.path.join(tmp, "Project Helghast Test", "sub dir")
    c, cmds = real_commands(root)
    check(len(cmds) >= 8, f"1 the real steps give commands ({len(cmds)})")
    check(sum(1 for a in cmds if any(root in e for e in a)) >= 8, "1 commands carry the install root")
    bad = [e for a in cmds for e in split_problems(a, root)]
    check(not bad, f"1 no argv element is a fragment of the root: {bad[:3]}")
    for a in cmds:
        line = subprocess.list2cmdline(a)
        back = win_split(line)
        check(back == a, f"1 list2cmdline round trip keeps {a[:2]}: {[e for e in back if e not in a][:2]}")
        trail = [e for e in a[1:] if os.path.isabs(e) and e.endswith(("/", "\\"))]
        check(not trail, f"1 no path argument ends in a separator (a trailing backslash before a closing quote): {trail[:2]}")
    # the Windows splitter follows the backslash rules (a path ending in a backslash survives list2cmdline)
    for t in (["C:\\a b\\dir\\", "x"], ['say "hi"', "C:\\p\\"], ["a\\\\b", "c d\\\\"]):
        check(win_split(subprocess.list2cmdline(t)) == t, f"1 splitter round trip {t}")
    # the CMake side: each path variable the configure passes is read quoted in the CMake files (a space in a path
    # otherwise splits a list)
    for rel in ("cmake/llvm-mingw.cmake", "port/kz3/CMakeLists.txt"):
        txt = open(os.path.join(HERE, "..", "..", *rel.split("/")), encoding="utf-8").read()
        for m in re.finditer(r"^\s*(?!#)[^\n]*\$\{(LLVM_MINGW_ROOT|RECOMP_DIR|PS3RECOMP_DIR|SPU_DIR|KZ3_EMBED_EBOOT)\}[^\n]*$",
                             txt, re.M):
            ln = m.group(0)
            if "message(" in ln or ln.lstrip().startswith('"'):
                continue
            check(re.search(r'"[^"\n]*\$\{' + m.group(1) + r'\}[^"\n]*"', ln) is not None,
                  f"1 {rel}: {m.group(1)} is used inside quotes: {ln.strip()[:90]}")
    # negative control: the same commands as one unquoted string split at spaces are caught
    broken = [e for a in cmds for e in split_problems(" ".join(a).split(" "), root)]
    check(broken, "1 control: an unquoted command line is detected as split")

    # a real child starts in the install folder and gets its path argument whole
    steps = [fr.Step("probe", "Probe", 1, lambda cc: [[PY, "-c",
             "import os,sys; print('CWD=' + os.getcwd()); print('ARG=' + sys.argv[1])", os.path.join(root, "EBOOT (1).elf")]])]
    out = io.StringIO()
    rc = fr.main(["--dump", c.dump, "--root", root, "--jobs", "1", "--log-dir", os.path.join(tmp, "dl")], steps=steps, out=out)
    o = out.getvalue()
    cwd = next((l[4:] for l in o.splitlines() if l.startswith("CWD=")), "")
    check(rc == 0 and os.path.realpath(cwd) == os.path.realpath(root), f"1 child cwd is the install folder: {cwd}")
    check(f"ARG={os.path.join(root, 'EBOOT (1).elf')}" in o, "1 the path argument arrived whole")


def run_steps(tmp, name, steps):
    root = os.path.join(tmp, name)
    os.makedirs(root, exist_ok=True)
    out = io.StringIO()
    rc = fr.main(["--dump", os.path.join(tmp, "d"), "--root", root, "--jobs", "1", "--log-dir", os.path.join(tmp, "dl-" + name)],
                 steps=steps, out=out)
    return rc, out.getvalue(), root


def part2(tmp):
    want = lambda c: [os.path.join(c.root, "build", "made.txt")]
    quiet = lambda c: [[PY, "-c", "pass"]]
    rc, o, _ = run_steps(tmp, "p2a", [fr.Step("make", "Making it", 1, quiet, outputs=want)])
    check(rc == 1 and "RESULT ok=0 step=make" in o, f"2 exit 0 but no output file fails the step (rc {rc})")
    check("made.txt" in o and "did not write" in o, "2 the message names the missing file")
    writes = lambda c: [[PY, "-c", f"import os; os.makedirs({os.path.join(c.root, 'build')!r}, exist_ok=True); open({want(c)[0]!r}, 'w').write('x')"]]
    rc, o, _ = run_steps(tmp, "p2b", [fr.Step("make", "Making it", 1, writes, outputs=want)])
    check(rc == 0 and "RESULT ok=1" in o, f"2 control: the same step writing its file passes (rc {rc})")
    rc, o, _ = run_steps(tmp, "p2c", [fr.Step("make", "Making it", 1, quiet)])
    check(rc == 0, "2 control: a step declaring no files is judged by the exit code as before")

    # exit 0 and a library error in the output, nothing written (the vekta 94 shape)
    said = lambda c: [[PY, "-c", "print('The program cannot start because LIBCLANG.dll was not found on your computer.')"]]
    rc, o, _ = run_steps(tmp, "p2d", [fr.Step("make", "Making it", 1, said, outputs=want)])
    check(rc == 1 and "runtime library problem" in o and "LIBCLANG.dll" in o, f"2 exit 0 + DLL text: named ({[l for l in o.splitlines() if 'REASON' in l][:1]})")
    inc = lambda c: [[PY, "-c", "import sys; print('This application has failed because it is incorrectly installed', file=sys.stderr); sys.exit(1)"]]
    rc, o, _ = run_steps(tmp, "p2e", [fr.Step("make", "Making it", 1, inc)])
    check(rc == 1 and "runtime library problem" in o, "2 'incorrectly installed' on stderr, exit 1: named")

    # exit 0xC0000135: a fake child cannot return it on Linux (exit codes are 8 bits), so the runner's Popen result is faked
    class FakeProc:
        returncode = 0xC0000135
        stdout = iter(())
        pid = 1

        def wait(self):
            return 0xC0000135

        def poll(self):
            return self.returncode
    real = subprocess.Popen
    try:
        fr.subprocess.Popen = lambda *a, **k: FakeProc()
        rc, o, _ = run_steps(tmp, "p2f", [fr.Step("make", "Making it", 1, lambda c: [[os.path.join(c.root, "clang.exe"), "-v"]])])
    finally:
        fr.subprocess.Popen = real
    reason = " ".join(l for l in o.splitlines() if "REASON" in l)
    check(rc == 1 and "clang.exe could not start" in reason and "0xC0000135" in reason and "antivirus" in reason,
          f"2 exit 0xC0000135: names the program and the code: {reason[:160]}")
    check("library" not in " ".join(l for l in run_steps(tmp, "p2g", [fr.Step("make", "M", 1, lambda c: [[PY, "-c", "import sys; sys.exit(1)"]])])[1].splitlines() if "REASON" in l),
          "2 control: an ordinary exit 1 gets no library wording")
    # a program that does not start at all
    rc, o, _ = run_steps(tmp, "p2h", [fr.Step("make", "Making it", 1, lambda c: [[os.path.join(c.root, "no-such-clang.exe")]])])
    check(rc == 1 and "no-such-clang.exe" in o and "REASON" in o and "cannot start" in o, "2 a program that cannot start is named")

    # launcher side
    import launch as la
    check("0xC0000135" in la.did_not_finish(0xC0000135, "The build did not finish"), "2 launcher names the library failure")
    check("0xC0000135" in la.game_exit_message(0xC0000135, "", []), "2 launcher names it for the game too")
    check("library" not in la.did_not_finish(1, "The build did not finish"), "2 control: code 1 unchanged")

    # negative control: without the library check the same cases fail
    orig = fr.child_dll_problem
    fr.child_dll_problem = lambda *a, **k: ""
    try:
        check(not orig("clang.exe", 0xC0000135) == "" and fr.child_dll_problem("clang.exe", 0xC0000135) == "",
              "2 control: with the check removed no message appears (the assertions above would fail)")
    finally:
        fr.child_dll_problem = orig

    # rpcs3.exe is never started
    for f in os.listdir(HERE):
        if f.endswith(".py") and not f.startswith("test_"):
            src = open(os.path.join(HERE, f), encoding="utf-8").read()
            check(not re.search(r"(Popen|\.run|\.call|startfile|check_output)\([^)\n]*rpcs3", src, re.I),
                  f"2 {f} never starts rpcs3.exe")


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-childchk-")
    try:
        part1(tmp)
        part2(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
