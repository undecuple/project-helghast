#!/usr/bin/env python3
"""kz3-install.ini and kz3.ini edited by hand: duplicate keys, non-UTF-8 bytes, comments, an ANSI font_dir. Nothing
raises; comments and other keys stay where the format allows; writes are atomic; reading and writing use one encoding
rule (UTF-8 with or without BOM, else the ANSI code page: latin-1 on this host, "mbcs" on Windows)."""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def attempt(label, fn):
    try:
        return fn()
    except Exception as e:  # noqa: BLE001
        check(False, f"{label} raised {type(e).__name__}: {str(e)[:70]}")
        return None


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-ini-")
    try:
        root = os.path.join(tmp, "root")
        os.makedirs(os.path.join(root, "build"))
        ip = os.path.join(root, "kz3-install.ini")
        ANSI = "C:\\Fonts\\M\u00fcller"
        raw = (b"; my notes, keep\r\n[install]\r\nportable = 0\r\nportable = 1\r\n# a comment\r\nextra = keep\r\n"
               b"[paths]\r\ndump = C:\\Games\\K\xe9\xe9\r\nfont_dir = " + ANSI.encode("latin-1") + b"\r\n")   # not UTF-8
        open(ip, "wb").write(raw)

        # readers do not raise on duplicate keys or non-UTF-8 bytes
        r = attempt("read_ini", lambda: la.read_ini(root))
        check(r is not None, "launch.read_ini survives a duplicate key and non-UTF-8 bytes")
        sp = attempt("setup_paths", lambda: la.setup_paths(root))
        check(sp is not None and sp.get("font_dir", "") == "" or sp == {}, "launch.setup_paths survives (needs dump and update)")
        attempt("shaders_wanted", lambda: fr.shaders_wanted(root, "auto"))

        # the writer keeps comments, other keys and the ANSI bytes; both duplicates get the value
        attempt("write_install_ini", lambda: fr.write_install_ini(root, False, True, shaders=False))
        out = open(ip, "rb").read()
        check(attempt("write", lambda: fr.write_install_ini(root, False, True)) == ip, "write_install_ini returns the path")
        out = open(ip, "rb").read()
        check(b"; my notes, keep" in out and b"# a comment" in out and b"extra = keep" in out, "comments and other keys kept")
        check(out.count(b"portable = 0") == 2 and b"portable = 1" not in out, "both duplicate keys carry the new value")
        check(b"check_updates = 1" in out and b"[online]" in out and b"prepare = 0" in out, "new sections and keys added")
        check(ANSI.encode("latin-1") in out and b"\xef\xbf\xbd" not in out, "the ANSI bytes round-trip, no U+FFFD")
        check(b"\r\n" in out and b"\n" not in out.replace(b"\r\n", b""), "the file's CRLF line ends are kept")
        check(not os.path.exists(ip + ".tmp"), "no .tmp left")
        calls = []
        real = os.replace
        os.replace = lambda a, b: (calls.append((a, b)), real(a, b))[1]
        try:
            la.write_ini(root, "online", "last_check", "2026-10-29")
        finally:
            os.replace = real
        check(calls and calls[0][1] == ip and calls[0][0] == ip + ".tmp", "writes go through a .tmp and os.replace")

        # a UTF-8 file with BOM stays UTF-8 with BOM
        bp = os.path.join(root, "b.ini")
        open(bp, "wb").write(b"\xef\xbb\xbf[install]\nportable = 1\n")
        fr.ini_set(bp, [("install", "portable", "0"), ("paths", "dump", "D:\\K\u00e9")])
        b = open(bp, "rb").read()
        check(b.startswith(b"\xef\xbb\xbf") and "D:\\K\u00e9".encode() in b, "UTF-8 BOM file written back as UTF-8 with BOM")

        # kz3.ini: ANSI font_dir read the way it is written, comments above a hand-added key stay
        gp = os.path.join(root, "build", "kz3.ini")
        open(gp, "wb").write(b"[game]\nvfs_root = C:\\d\n; my setting\ndiscord_presence = 1\nfont_dir = " + ANSI.encode("latin-1") + b"\n")
        g = attempt("read_game_ini", lambda: la.read_game_ini(gp)) or {}
        check(g.get("font_dir") == ANSI, f"ANSI font_dir read as {g.get('font_dir')!r}")
        attempt("write_game_ini", lambda: la.write_game_ini(root, g))
        back = open(gp, "rb").read()
        check(ANSI.encode("latin-1") in back and b"\xef\xbf\xbd" not in back, "ANSI font_dir written back unchanged")
        check(b"; my setting\r\ndiscord_presence = 1" in back.replace(b"\n", b"\r\n").replace(b"\r\r", b"\r"),
              "the comment above a hand-added key is kept")
        check(not os.path.exists(gp + ".tmp"), "kz3.ini written aside and swapped in (no .tmp left)")
        # a file nothing can parse
        open(ip, "wb").write(b"\xff\xfe\x00garbage [[[ \n=\n")
        check(attempt("garbage read", lambda: la.read_ini(root)) is not None, "an unparsable file reads as empty")
        attempt("garbage write", lambda: fr.write_install_ini(root, True, False))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_ini_hand_edit: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
