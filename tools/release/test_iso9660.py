#!/usr/bin/env python3
"""Host test for tools/release/iso9660.py on tiny ISO images built in the test (iso_fixture.py; no external tools, no
game data): nested lookup (any case, ';1' stripped), a multi-extent file read and copied whole, a missing file, a
truncated image, a non-ISO file, a Joliet image, an encrypted-looking image (region table in sector 0, wrong magic),
a damaged directory, chunked copy progress. Exit 0 = PASS."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso9660 as iso  # noqa: E402
from iso_fixture import build_iso  # noqa: E402

bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


def raises(fn, cls=iso.IsoError):
    try:
        fn()
    except cls as e:
        return e
    return None


big = bytes((i * 7 + 3) & 255 for i in range(10000))
FILES = {
    "PS3_DISC.SFB": b".SFB" + b"\0" * 20,
    "PS3_GAME/PARAM.SFO": b"\0PSF" + b"x" * 100,
    "PS3_GAME/USRDIR/EBOOT.BIN": b"SCE\0" + b"e" * 50,
    "PS3_GAME/USRDIR/data_ps3.psarc": b"PSAR" + big,
    "PS3_GAME/TROPDIR/NPWR01569_00/TROPHY.TRP": b"trp",
    "PS3_GAME/ICON0.PNG": b"png",
}
d = tempfile.mkdtemp(prefix="kz3-iso-")
p = os.path.join(d, "t.iso")
build_iso(p, FILES, multi={"PS3_GAME/USRDIR/data_ps3.psarc": 4096})
with iso.Iso(p) as im:
    e = im.lookup("ps3_game/usrdir/EBOOT.bin")
    check(e is not None and not e.is_dir and e.size == 54, "nested lookup, case-insensitive, ;1 stripped")
    check(im.lookup("PS3_GAME\\PARAM.SFO") is not None, "backslash path")
    check(im.read("PS3_GAME/PARAM.SFO", 4) == b"\0PSF", "read the first bytes")
    check(im.lookup("PS3_GAME/NOPE.SFO") is None and im.lookup("PS3_GAME/PARAM.SFO/x") is None, "a missing file is None")
    check(raises(lambda: im.read("PS3_GAME/NOPE.SFO", 4)) is not None, "reading a missing file raises IsoError")
    pe = im.lookup("PS3_GAME/USRDIR/data_ps3.psarc")
    check(len(pe.extents) == 3 and pe.size == 10004, f"multi-extent file chained ({len(pe.extents)} extents, {pe.size})")
    check(im.read("PS3_GAME/USRDIR/data_ps3.psarc") == b"PSAR" + big, "multi-extent read crosses extents")
    check(im.read("PS3_GAME/USRDIR/data_ps3.psarc", 100, 4090) == (b"PSAR" + big)[4090:4190], "read at an offset")
    seen = []
    out = os.path.join(d, "o.psarc")
    n = im.copy("PS3_GAME/USRDIR/data_ps3.psarc", out, lambda a, b: seen.append((a, b)), chunk=3000)
    check(n == 10004 and open(out, "rb").read() == b"PSAR" + big and not os.path.exists(out + ".part"),
          "chunked copy is exact, no .part left")
    check(len(seen) >= 4 and seen[-1] == (10004, 10004) and all(b == 10004 for _, b in seen), "progress ends at total")
    names = sorted(r for r, _ in im.walk("PS3_GAME"))
    check(names == ["ICON0.PNG", "PARAM.SFO", "TROPDIR/NPWR01569_00/TROPHY.TRP",
                    "USRDIR/DATA_PS3.PSARC", "USRDIR/EBOOT.BIN"], f"walk: {names}")
    check(sorted(im.listdir("")) == ["PS3_DISC.SFB", "PS3_GAME"], "root listing")
    check(raises(lambda: im.check_magic("PS3_GAME/PARAM.SFO", b"XXXX"), iso.IsoEncrypted) is not None,
          "wrong magic = encrypted")
    check(im.check_magic("PS3_GAME/PARAM.SFO", b"\0PSF") is None, "right magic passes")
    check(not im.truncated_image, "volume size matches the file")

# truncated image: cut inside the multi-extent file, then before the directories
data = open(p, "rb").read()
t = os.path.join(d, "trunc.iso")
open(t, "wb").write(data[:len(data) - 5000])
with iso.Iso(t) as im:
    e = raises(lambda: im.copy("PS3_GAME/USRDIR/data_ps3.psarc", os.path.join(d, "x")))
    check(e is not None and "truncated" in str(e), "copy from a truncated image refuses")
    check(im.truncated_image and not im.in_image(im.lookup("PS3_GAME/USRDIR/data_ps3.psarc")), "truncation is reported")
open(t, "wb").write(data[:18 * 2048])
e = raises(lambda: iso.Iso(t).lookup("PS3_GAME"))
check(e is not None, f"image cut before the directories refuses ({e})")

# not an ISO
n = os.path.join(d, "n.iso")
open(n, "wb").write(b"hello" * 100000)
e = raises(lambda: iso.Iso(n))
check(e is not None and "not an ISO" in str(e) and not isinstance(e, iso.IsoEncrypted), f"a non-ISO file: {e}")
open(n, "wb").write(b"tiny")
check(raises(lambda: iso.Iso(n)) is not None, "a tiny file refuses")
check(raises(lambda: iso.Iso(os.path.join(d, "missing.iso"))) is not None, "a missing file refuses")

# an image with an encrypted-region table in sector 0 and no volume descriptor
reg = bytearray(2048 * 40)
reg[0:4] = struct.pack(">I", 2)
reg[8:24] = struct.pack(">IIII", 0, 15, 16, 30)
open(n, "wb").write(bytes(reg))
e = raises(lambda: iso.Iso(n), iso.IsoEncrypted)
check(e is not None and str(e) == "this ISO is encrypted; use a decrypted dump", "region table, no PVD = encrypted")

# a damaged directory (record length runs past the sector) -> IsoError
bd = bytearray(data)
root_lba = struct.unpack_from("<I", bd, 16 * 2048 + 158)[0]
bd[root_lba * 2048 + 68] = 20      # shorter than any record
open(t, "wb").write(bytes(bd))
e = raises(lambda: iso.Iso(t).lookup("PS3_GAME/PARAM.SFO"))
check(e is not None, f"a damaged directory record refuses ({e})")

# Joliet image opens; a volume bigger than the file is flagged
build_iso(p, {"PS3_GAME/PARAM.SFO": b"\0PSF"}, joliet=True)
with iso.Iso(p) as im:
    check(len(im.volumes) == 2 and im.lookup("ps3_game/param.sfo") is not None, "a Joliet image opens")
build_iso(p, FILES, volume_sectors=999999)
with iso.Iso(p) as im:
    check(im.truncated_image, "a volume bigger than the file is flagged")

# CLI
me = os.path.join(HERE, "iso9660.py")
r = subprocess.run([sys.executable, me, "list", p, "PS3_GAME"], capture_output=True, text=True)
check(r.returncode == 0 and "PARAM.SFO" in r.stdout, "CLI list")
r = subprocess.run([sys.executable, me, "list", n], capture_output=True, text=True)
check(r.returncode == 1 and r.stdout.startswith("FAIL: "), "CLI refuses with FAIL: <message>")

shutil.rmtree(d, ignore_errors=True)
print("PASS" if not bad else f"FAIL ({bad})")
sys.exit(1 if bad else 0)
