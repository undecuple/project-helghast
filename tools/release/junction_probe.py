#!/usr/bin/env python3
"""Windows diagnostic for "the game cannot open files through <install>\\vfs\\PS3_GAME" (round KZ3-03).

    python junction_probe.py <path-to-a-file-through-the-junction> [<same-file-by-its-real-path>]

For each path it tries, in this one process (same user, same token as a game started from here): os.stat, Python open(),
the Win32 CreateFileW (GetLastError), the C runtime's fopen with the ANSI path (what kz3.exe uses) and _wfopen; and
prints the reparse tag/target (fsutil reparsepoint query on the nearest link), the process's elevation, and the
Redirection Guard policy (ProcessRedirectionTrustPolicy). Run it from the shell the launcher is started from, then
elevated; compare with kz3.exe's log. Prints no user names beyond the paths you pass. Windows only; exits 0 always."""
import ctypes
import os
import subprocess
import sys


def try_open(label, fn):
    try:
        fn()
        print(f"  ok    {label}")
    except Exception as e:   # noqa: BLE001 - a diagnostic reports whatever happens
        print(f"  FAIL  {label}: {e}")


def probe(path):
    print(f"\n== {path}")
    try:
        if os.name == "nt":
            path.encode("mbcs")
        print("  ansi-encodable: yes")
    except UnicodeEncodeError:
        print("  NOT ANSI-encodable: fopen cannot name this path")
    try_open("os.stat", lambda: os.stat(path))
    try_open("python open(rb)", lambda: open(path, "rb").close())
    if os.name != "nt":
        return
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = ctypes.c_void_p
    h = k.CreateFileW(path, 0x80000000, 7, None, 3, 0x80, None)
    if h in (None, ctypes.c_void_p(-1).value):
        print(f"  FAIL  CreateFileW: Win32 error {ctypes.get_last_error()}")
    else:
        print("  ok    CreateFileW")
        k.CloseHandle(ctypes.c_void_p(h))
    for dll in ("ucrtbase", "msvcrt"):
        try:
            c = ctypes.CDLL(dll)
        except OSError:
            continue
        c.fopen.restype = ctypes.c_void_p
        f = c.fopen(path.encode("mbcs", "replace"), b"rb")
        print(f"  {'ok   ' if f else 'FAIL '} {dll}.fopen (ANSI){'' if f else ': errno ' + str(ctypes.get_errno())}")
        if f:
            c.fclose(ctypes.c_void_p(f))
    # nearest link on the way up
    p = os.path.abspath(path)
    while p and os.path.dirname(p) != p:
        if os.path.islink(p) or (hasattr(os.path, "isjunction") and os.path.isjunction(p)):
            print(f"  link at {p} -> {os.readlink(p)!r}")
            r = subprocess.run(["fsutil", "reparsepoint", "query", p], capture_output=True, text=True)
            print("  " + "\n  ".join((r.stdout or r.stderr).splitlines()[:8]))
            break
        p = os.path.dirname(p)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 0
    if os.name == "nt":
        elevated = bool(ctypes.windll.shell32.IsUserAnAdmin())
        print(f"elevated: {elevated}")
        pol = ctypes.c_uint32(0)   # PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY.Flags (EnforceRedirectionTrust = bit 0)
        ok = ctypes.windll.kernel32.GetProcessMitigationPolicy(ctypes.c_void_p(-1), 16, ctypes.byref(pol), 4)
        print(f"redirection trust policy: {'flags=%d' % pol.value if ok else 'not available'}")
    for p in argv[1:]:
        probe(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
