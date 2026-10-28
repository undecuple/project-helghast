#!/usr/bin/env python3
"""Host test for tools/release/lift_fixups.py and its use in firstrun.py's PPU lift step, on synthetic files in a
temp folder (no game data, no EBOOT, no work/). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_lift_fixups.py

Cases: the switch-114 file's shape (an "end" and a "jt ... rel" line) gives the same functions.fixed.json and
jt_hints.txt that switch-114's tools/lift.sh inline code gave (main's lift.sh calls this helper); other functions unchanged; comments and blank lines ignored;
refused with nothing written: an "end" for a function not in the list, a malformed line, a bad jt mode; firstrun's
ppu-lift step runs the helper first and lifts functions.fixed.json with PPU_JT_HINTS set only when the fixups file
exists, and its SPU step passes the build folder to lift_spu.py and gen_spu_register.py; the repo's own fixups files
(re/data/overrides and port/spu ppu_lift_fixups-*.txt) parse, and tools/lift.sh runs this helper for them.
"""
import io
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402
import lift_fixups as lf  # noqa: E402

FAILS = []
FIX = """# PPU lift fixups (synthetic, same shape as port/spu/ppu_lift_fixups-114.txt)
#   end  FUNC NEW_END

end 0x00E67F48 0x00E69AF8
jt  0x00E6804C 0x00E68050 32 rel   # trailing comment
"""


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  {detail}"[:600]))
    if not cond:
        FAILS.append(what)


def lift_sh_reference(functions, fix_text):
    """tools/lift.sh's inline Python and sed on switch-114, restated: ends by start, jt lines minus 'jt'."""
    ends = {int(ln.split()[1], 0): int(ln.split()[2], 0) for ln in fix_text.splitlines() if ln.startswith("end ")}
    fs = json.loads(json.dumps(functions))
    for f in fs:
        a = int(f["start"], 16)
        if a in ends:
            f["end"] = f"0x{ends[a]:08X}"
    hints = [ln[2:].lstrip() for ln in fix_text.splitlines() if ln.startswith("jt ")]
    return fs, hints


def run(tmp, fix_text, functions):
    fx, fn = os.path.join(tmp, "fix.txt"), os.path.join(tmp, "functions.json")
    out, jt = os.path.join(tmp, "o", "functions.fixed.json"), os.path.join(tmp, "o", "jt_hints.txt")
    shutil.rmtree(os.path.join(tmp, "o"), ignore_errors=True)
    with open(fx, "w") as f:
        f.write(fix_text)
    with open(fn, "w") as f:
        json.dump(functions, f)
    buf = io.StringIO()
    rc = lf.main(["--fixups", fx, "--functions", fn, "--out", out, "--jt-hints", jt], out=buf)
    return rc, buf.getvalue(), out, jt


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-fixups-")
    funcs = [{"start": "0x00E67F48", "end": "0x00E68050", "name": "func_00E67F48"},
             {"start": "0x00E69AF8", "end": "0x00E69B40", "name": "func_00E69AF8"}]
    try:
        rc, txt, out, jt = run(tmp, FIX, funcs)
        with open(out) as f:
            got = json.load(f)
        with open(jt) as f:
            hints = f.read().splitlines()
        ref, ref_hints = lift_sh_reference(funcs, FIX.replace("   # trailing comment", ""))
        check(rc == 0 and got == ref and "fixup: func_00E67F48 end 0x00E68050 -> 0x00E69AF8" in txt,
              "end fixup matches lift.sh's", (rc, txt, got))
        check(hints == ref_hints == ["0x00E6804C 0x00E68050 32 rel"], "jt hints match lift.sh's", (hints, ref_hints))
        check(got[1] == funcs[1], "other functions unchanged")

        for what, text in (("end for a missing function", "end 0x00001000 0x2000\n"),
                           ("malformed line", "end 0x00E67F48\n"), ("bad jt mode", "jt 0x1 0x2 3 far\n"),
                           ("not a number", "end 0xZZ 0x1\n"), ("unknown kind", "skip 0x1\n")):
            rc, txt, out, jt = run(tmp, text, funcs)
            check(rc == 1 and "not applied" in txt and not os.path.exists(out) and not os.path.exists(jt),
                  f"refused: {what}", txt)
        rc, txt, out, jt = run(tmp, "# only comments\n", funcs)
        with open(jt) as f:
            check(rc == 0 and f.read() == "", "no fixups: empty hints file, list unchanged", txt)

        # firstrun's ppu-lift and spu-lift commands
        root = os.path.join(tmp, "root")
        os.makedirs(os.path.join(root, "port", "spu"))
        c = fr.Ctx(root=root, build=os.path.join(root, "build"), dump="d", eboot=os.path.join(tmp, "EBOOT.elf"))
        cmds = fr.cmd_ppu_lift(c)
        check(len(cmds) == 1 and cmds[0][cmds[0].index("--functions") + 1].endswith("functions.json")
              and "PPU_JT_HINTS" not in c.env(), "without a fixups file: one lift of functions.json", cmds)
        with open(os.path.join(root, "port", "spu", "ppu_lift_fixups-114.txt"), "w") as f:
            f.write(FIX)
        cmds = fr.cmd_ppu_lift(c)
        env = c.env()
        check(len(cmds) == 2 and cmds[0][3].endswith("lift_fixups.py") and cmds[0][1:3] == ["-X", "utf8"]
              and cmds[1][cmds[1].index("--functions") + 1] == c.b("analysis", "functions.fixed.json")
              and env.get("PPU_JT_HINTS") == c.b("analysis", "jt_hints.txt"), "with fixups: helper, then the lift",
              cmds)
        check(env.get("KZ3_EBOOT_VER") == "114" and env.get("KZ3_ANALYSIS_DIR") == c.b("analysis")
              and env.get("KZ3_EBOOT_ELF") == c.eboot, "SPU tools pointed at this build's EBOOT and analysis")
        spu = fr.cmd_spu_lift(c)
        check(spu[0][spu[0].index("--out") + 1] == c.b("spu") and spu[1][spu[1].index("--lift") + 1] == c.b("spu")
              and spu[1][spu[1].index("--out") + 1] == c.b("spu", "gen"), "SPU lift writes into the build", spu)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # The repo's fixups files (text only, no game data) and tools/lift.sh's use of the helper.
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    files = [os.path.join(root, d, f"ppu_lift_fixups-{v}.txt") for d in ("re/data/overrides", "port/spu")
             for v in ("100", "114")]
    files = [f for f in files if os.path.exists(f)]
    check(len(files) >= 1, "repo has a 1.14 fixups file", files)
    for f in files:
        try:
            ends, hints = lf.parse(f)
            check(bool(ends or hints), f"{os.path.relpath(f, root)} parses", (ends, hints))
        except lf.FixupError as e:
            check(False, f"{os.path.relpath(f, root)} parses", e)
    lift = open(os.path.join(root, "tools", "lift.sh")).read()
    check("tools/release/lift_fixups.py" in lift and "PPU_JT_HINTS" in lift and "tools/spu/kz3ver.py ver recomp" in lift,
          "tools/lift.sh applies the fixups through this helper (version and output dir from kz3ver.py)")
    print(f"test_lift_fixups: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
