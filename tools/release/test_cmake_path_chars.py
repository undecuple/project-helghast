#!/usr/bin/env python3
"""Host test (public issue 20): CMake 4.x configure from a folder whose path holds ';' stops in
its own compiler detection (CMakeDetermineCompilerId.cmake), with a plain project of no files of ours: the
cause is CMake's, so the setup and firstrun_check.py refuse [ ] ; (see test_install_dir_rule.py, test_firstrun_check.py).
Prints the result for '[' and ']' too (informational: on Linux they configure; the reporter's failure was on Windows).
SKIP when cmake or a C compiler is missing."""
import os
import shutil
import subprocess
import sys
import tempfile

if os.name == "nt":
    # the Windows case (CMake installed under a '[' folder) was reproduced by hand on a Windows 10 VM; a runner's
    # default generator and compiler differ from the bundled toolchain, so the check runs on Linux only
    print("SKIP: Linux-only check")
    sys.exit(0)
cmake = shutil.which("cmake")
cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang") or shutil.which("x86_64-w64-mingw32-gcc")
if not cmake or not cc:
    print("SKIP: cmake or a C compiler not on PATH")
    sys.exit(0)


def configure(base, name):
    src = os.path.join(base, name, "src")
    os.makedirs(src)
    with open(os.path.join(src, "CMakeLists.txt"), "w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\nproject(t C)\nadd_executable(t m.c)\n")
    with open(os.path.join(src, "m.c"), "w") as f:
        f.write("int main(void){return 0;}\n")
    p = subprocess.run([cmake, "-S", src, "-B", os.path.join(base, name, "bld"), "-DCMAKE_C_COMPILER=" + cc],
                       capture_output=True, text=True)
    return p.returncode == 0


bad = 0
with tempfile.TemporaryDirectory() as tmp:
    ok_plain = configure(tmp, "plain")
    print(("ok   " if ok_plain else "FAIL ") + "control: a plain path configures")
    bad += not ok_plain
    semi = configure(tmp, "a;b")
    print(("ok   " if not semi else "NOTE ") + "a ';' in the path fails configure" + ("" if not semi else " (this CMake accepts it now: revisit the refusal)"))
    for ch in "[]":
        print(f"info {ch!r} in the path: configure " + ("works" if configure(tmp, "a" + ch + "b") else "fails"))
sys.exit(1 if bad else 0)
