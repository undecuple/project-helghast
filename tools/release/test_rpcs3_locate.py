#!/usr/bin/env python3
"""Host test for rpcs3_locate.py's portable-folder search (no game data): a portable RPCS3 (rpcs3.exe beside config/)
unzipped under a Downloads-like base is found; a folder named rpcs3 without rpcs3.exe, a non-rpcs3 folder and a deep
subfolder are not; the hint's config/ child resolves to the portable root. Exit 0 = PASS, 1 = FAIL."""
import os
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if sys.version_info < (3, 11):
    print("test_rpcs3_locate: SKIP (rpcs3_locate needs Python 3.11+)")
    sys.exit(0)
import rpcs3_locate as L  # noqa: E402

fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


def main():
    with tempfile.TemporaryDirectory() as td:
        dl = Path(td) / "Downloads"
        port = dl / "rpcs3-v0.0.30-win64"
        (port / "config").mkdir(parents=True)
        (port / "rpcs3.exe").write_bytes(b"MZ")
        (port / "config" / "vfs.yml").write_text("/dev_hdd0/: $(EmulatorDir)dev_hdd0/\n")
        (dl / "rpcs3-notes").mkdir()                     # named rpcs3 but no rpcs3.exe
        (dl / "other").mkdir()
        (dl / "other" / "rpcs3.exe").write_bytes(b"MZ")  # not a direct 'rpcs3' child, and not a base itself
        deep = dl / "a" / "rpcs3-deep"
        (deep / "config").mkdir(parents=True)
        (deep / "rpcs3.exe").write_bytes(b"MZ")
        res = L.find_rpcs3(None, portable_bases=[dl, Path(td) / "missing"])
        roots = [r.get("root") for r in res if r.get("source") == "portable"]
        check(roots == [str(port)], f"portable RPCS3 under Downloads found, no false positives ({roots})")
        check(res and res[0].get("dev_hdd0") == str(port / "dev_hdd0"), "its dev_hdd0 comes from the portable vfs.yml")
        res = L.find_rpcs3(str(port / "config"), portable_bases=[])
        check(any(r.get("source") == "hint" and r.get("root") == str(port) for r in res),
              "a hint at the portable folder's config/ resolves to the portable root")
        check(L.find_rpcs3(None, portable_bases=[Path(td) / "none"]) == [] or all(r.get("source") != "portable" for r in L.find_rpcs3(None, portable_bases=[Path(td) / "none"])),
              "no portable folder, none reported")
    # fake winreg: moved / OneDrive-redirected folders come first, env vars expanded, duplicates dropped
    class FakeWR:
        HKEY_CURRENT_USER = 1
        vals = {"Desktop": "%KZ3FAKE%/Desktop", "Personal": "%KZ3FAKE%/Docs",
                "{374DE290-123F-4565-9164-39C4925E467B}": "%KZ3FAKE%/Downloads"}
        def OpenKey(self, root, sub):
            return object()
        def QueryValueEx(self, key, name):
            if name in self.vals:
                return self.vals[name], 2
            raise OSError(name)
        def CloseKey(self, key):
            pass
    os.environ["KZ3FAKE"] = "/od"
    od = os.path.normpath("/od")   # "\\od" on Windows, where Path prints backslashes
    sf = [os.path.normpath(str(x)) for x in L._shell_folders(FakeWR())]
    check(len(sf) == 3 and all(x.startswith(od) for x in sf), f"registry shell folders expanded ({sf})")
    b = L._portable_bases(FakeWR())
    check(os.path.normpath(str(b[0])).startswith(od) and len({os.path.normpath(str(x)) for x in b}) == len(b), "registry folders first, no duplicates")
    class NoWR(FakeWR):
        def OpenKey(self, root, sub):
            raise OSError("none")
    check(L._shell_folders(NoWR()) == [] and any(x.name == "Downloads" for x in L._portable_bases(NoWR())),
          "registry failure falls back to the fixed names")
    print("test_rpcs3_locate: " + ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
