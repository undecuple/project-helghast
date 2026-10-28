"""Helpers for the upstream focused tests: pull a function or #define out of a ps3recomp source file verbatim."""
import re, sys

def function(src, signature_regex):
    m = re.search(signature_regex, src)
    if not m:
        return None
    i, depth = src.index("{", m.start()) + 1, 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0); i += 1
    return src[m.start():i]

def define(src, name):
    m = re.search(rf"^#define {name}\b.*$", src, re.M)
    if not m:
        sys.exit(f"#define {name} not found")
    return m.group(0)


# ---- shared by the newer focused tests (tls blocks, io mappings, savedata, aa surface, gamma) ----
import os, subprocess, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

def trees():
    """(upstream v0.13.0 tree, tree under test). OLD_TREE / NEW_TREE override the defaults."""
    return (os.environ.get("OLD_TREE", os.path.join(ROOT, "third_party/ps3recomp")),
            os.environ.get("NEW_TREE", os.path.join(ROOT, "work/ps3recomp-patched")))

def build_run(code, lang="c", flags=()):
    """Compile `code` with cc / c++ and run it. Returns (compile_failed, stdout_or_compiler_errors)."""
    cc, ext = ("cc", "c") if lang == "c" else ("c++", "cpp")
    with tempfile.TemporaryDirectory() as d:
        open(f"{d}/t.{ext}", "w").write(code)
        r = subprocess.run([cc, "-O0", "-w", *flags, f"{d}/t.{ext}", "-o", f"{d}/t", "-lm"], capture_output=True, text=True)
        if r.returncode:
            return True, r.stderr
        return False, subprocess.run([f"{d}/t"], capture_output=True, text=True).stdout

def verdict(name, upstream_bad, new_ok, control_bad):
    """One PASS/FAIL line. PASS = the bug shows on upstream, the tree under test is right, the negative control is caught."""
    ok = upstream_bad and new_ok and control_bad
    print(f"{'PASS' if ok else 'FAIL'}: {name} (upstream shows the bug: {upstream_bad}; tree under test correct: {new_ok}; "
          f"negative control caught: {control_bad})")
    sys.exit(0 if ok else 1)

def verdict_absorbed(name, pr, upstream_ok, new_ok, control_bad):
    """For a fix upstream absorbed (ps3recomp v0.15.0): PASS = upstream is right now, the tree under test is right (our
    stack did not undo it), and the negative control (the bug put back into the tree under test) is caught."""
    ok = upstream_ok and new_ok and control_bad
    print(f"{'PASS' if ok else 'FAIL'}: {name}, absorbed upstream ({pr}) (upstream correct: {upstream_ok}; "
          f"tree under test correct: {new_ok}; negative control caught: {control_bad})")
    sys.exit(0 if ok else 1)
