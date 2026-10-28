#!/usr/bin/env python3
"""Host test for tools/assets/profile_edit.py (RE pass 66): synthetic 73,448-byte profile, no game data.

Checks: show on a fresh profile (continue = none), the exact-edit unlock through index 3 sets exactly the three
first-node bits of j < 3 (file 0x1058, 0x1818, 0x211C), T=0 sets only 0x1058, the sixaxis byte (0x86) and
clear-continue, stage (template savedata tree + this profile); negative controls: wrong size refused, write outside work/ refused, in-place write refused,
a continue record is reported as continuable only when done == 0.
"""
import struct
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "assets" / "profile_edit.py"
sys.path.insert(0, str(TOOL.parent))
import profile_edit as pe  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def fresh():
    d = bytearray(pe.SIZE)
    struct.pack_into(">I", d, 0, pe.VERSION_WORD)
    struct.pack_into(">i", d, pe.CONT_LEVEL, -1)
    struct.pack_into(">b", d, pe.CONT_DIFF, -1)
    d[pe.SIXAXIS] = 1
    return d


def run(*args):
    r = subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, env=ENV)
    return r.returncode, r.stdout + r.stderr


def main():
    global ENV
    with tempfile.TemporaryDirectory() as td:
        ENV = dict(os.environ, KZ3_WORK_DIR=str(Path(td) / "work"))
        work = Path(td) / "work" / "proton" / "t66" / "app" / "gamedata" / "sd"
        work.mkdir(parents=True)
        src = work / "PROFILE.DAT"
        src.write_bytes(bytes(fresh()))

        rc, out = run("show", str(work.parent.parent.parent))  # run dir: file found by rglob
        check(rc == 0 and "can_continue=False" in out and "sixaxis byte  file 0x86 = 1" in out, "show fresh profile from a run dir")

        out1 = Path(td) / "work" / "edit1.DAT"
        rc, out = run("edit", str(src), "--out", str(out1), "--unlock-through", "3")
        d = out1.read_bytes()
        bits = {off: struct.unpack_from(">I", d, off)[0] for off in (0x1058, 0x1818, 0x211C)}
        others = sum(1 for off in range(pe.SLOT0, pe.SLOT0 + pe.NSLOTS * pe.SLOT_STRIDE, 4)
                     if struct.unpack_from(">I", d, off)[0] and off not in bits)
        check(rc == 0 and all(v == 1 for v in bits.values()) and others == 0, "unlock-through 3 sets exactly the three first-node bits (j < 3)")
        check(len(d) == pe.SIZE and d[:0x1058] == bytes(fresh())[:0x1058], "unlock edit leaves the rest untouched")

        out0 = Path(td) / "work" / "edit0.DAT"
        rc, out = run("edit", str(src), "--out", str(out0), "--unlock-through", "0")
        d0 = out0.read_bytes()
        check(rc == 0 and struct.unpack_from(">I", d0, 0x1058)[0] == 1 and struct.unpack_from(">I", d0, 0x1818)[0] == 0, "unlock-through 0 marks mission_0's node 10 only")

        out2 = Path(td) / "work" / "edit2.DAT"
        rc, out = run("edit", str(src), "--out", str(out2), "--sixaxis", "0")
        check(rc == 0 and out2.read_bytes()[0x86] == 0 and "sixaxis" in out, "sixaxis byte set to 0")

        # continue record: continuable only when done == 0
        c = fresh()
        struct.pack_into(">i", c, pe.CONT_LEVEL, 200)
        struct.pack_into(">b", c, pe.CONT_DIFF, 1)
        struct.pack_into(">I", c, pe.CONT_SIZE, 0x84)
        c[pe.CONT_REC:pe.CONT_REC + 4] = b"\xde\xad\xbe\xef"
        csrc = Path(td) / "work" / "cont.DAT"
        csrc.write_bytes(bytes(c))
        rc, out = run("show", str(csrc))
        check(rc == 0 and "level_id=200 diff=1" in out and "can_continue=True" in out and "deadbeef" in out, "show reports a continuable record")
        c[pe.CONT_DONE] = 1
        csrc.write_bytes(bytes(c))
        rc, out = run("show", str(csrc))
        check(rc == 0 and "can_continue=False" in out, "negative: done=1 is not continuable")
        out3 = Path(td) / "work" / "cleared.DAT"
        rc, out = run("edit", str(csrc), "--out", str(out3), "--clear-continue")
        d3 = out3.read_bytes()
        check(rc == 0 and d3[pe.CONT_LEVEL:pe.CONT_LEVEL + 4] == b"\xff\xff\xff\xff" and d3[pe.CONT_SIZE:pe.CONT_SIZE + 4] == bytes(4)
              and d3[pe.CONT_REC:pe.CONT_REC + 4] == bytes(4), "clear-continue resets the record")

        # stage: template run dir + this profile -> a --save-from run dir
        tpl = Path(td) / "work" / "proton" / "t66"
        (work / "PARAM.SFO").write_bytes(b"sfo")
        st = Path(td) / "work" / "staged"
        rc, out = run("stage", str(out1), "--template", str(tpl), "--out", str(st))
        sp = st / "app" / "gamedata" / "sd" / "PROFILE.DAT"
        check(rc == 0 and sp.read_bytes() == out1.read_bytes() and (sp.parent / "PARAM.SFO").read_bytes() == b"sfo"
              and src.read_bytes() == bytes(fresh()), "stage copies the template tree and replaces only PROFILE.DAT")
        rc, out = run("stage", str(out1), "--template", str(tpl), "--out", str(st))
        check(rc != 0 and "exists" in out, "negative: stage refuses an existing run dir")
        rc, out = run("stage", str(out1), "--template", str(tpl), "--out", str(Path(td) / "outside"))
        check(rc != 0 and not (Path(td) / "outside").exists(), "negative: stage outside work/ refused")

        # negative controls
        bad = Path(td) / "work" / "short.DAT"
        bad.write_bytes(bytes(100))
        rc, out = run("show", str(bad))
        check(rc != 0 and "size 100" in out, "negative: wrong size refused")
        rc, out = run("edit", str(src), "--out", str(Path(td) / "outside.DAT"), "--sixaxis", "0")
        check(rc != 0 and not (Path(td) / "outside.DAT").exists(), "negative: output outside work/ refused")
        rc, out = run("edit", str(src), "--out", str(src), "--sixaxis", "0")
        check(rc != 0 and src.read_bytes()[0x86] == 1, "negative: in-place write refused, source untouched")
        rc, out = run("edit", str(src), "--out", str(Path(td) / "work" / "x.DAT"), "--unlock-through", "9")
        check(rc != 0, "negative: unlock index outside chapter 1 refused")
    print(f"test_profile_edit: {'PASS' if fails == 0 else f'{fails} FAIL'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
