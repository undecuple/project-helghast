#!/usr/bin/env python3
"""Host test for rpcs3_import.py and its place in firstrun.interactive, on synthetic RPCS3 trees (a made-up PARAM.SFO,
a 4-byte 'SCE' EBOOT.BIN, no game data). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_rpcs3_import.py

Cases: locate found / not found / wrong update version; encrypted-EBOOT guidance shown, not shown once EBOOT.elf is
beside it; the wizard accepts the decrypted EBOOT after the check is repeated; saves yes / no / conflict (asked,
default keep); the RPCS3 tree is never written (file list and mtimes unchanged) and a save root inside it is refused;
the old prompts stay when --rpcs3 is off or nothing is found; every wizard string comes from rpcs3_text.
"""
import io
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if sys.version_info < (3, 11):
    print("test_rpcs3_import: SKIP (rpcs3_locate needs Python 3.11+)")
    sys.exit(0)
import firstrun as fr  # noqa: E402
import firstrun_check as fc  # noqa: E402
import rpcs3_import as ri  # noqa: E402
import rpcs3_text as T  # noqa: E402
import test_firstrun_check as tfc  # noqa: E402

fc.MIN_PSARC_BYTES = 1024
FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def snapshot(root):
    return {os.path.relpath(os.path.join(d, f), root): (os.path.getmtime(os.path.join(d, f)), os.path.getsize(os.path.join(d, f)))
            for d, _, fs in os.walk(root) for f in fs} | \
           {os.path.relpath(os.path.join(d, x), root): None for d, ds, _ in os.walk(root) for x in ds}


def make_rpcs3(base, app_ver="01.14", with_update=True, with_elf=False, saves=("BCUS98234-AUTO",)):
    """A portable RPCS3 folder: rpcs3.exe, config/games.yml -> a synthetic disc dump, the update, saves."""
    r = os.path.join(base, "rpcs3")
    os.makedirs(os.path.join(r, "config"))
    open(os.path.join(r, "rpcs3.exe"), "wb").write(b"MZ")
    dump = tfc.make_dump(base)
    open(os.path.join(r, "config", "games.yml"), "w").write(f"BCUS98234: {dump}\n")
    upd = os.path.join(r, "dev_hdd0", "game", "BCUS98234")
    if with_update:
        tfc.write_sfo(os.path.join(upd, "PARAM.SFO"),
                      {"APP_VER": app_ver, "CATEGORY": "GD", "TITLE": "Synthetic", "TITLE_ID": "BCUS98234"})
        os.makedirs(os.path.join(upd, "USRDIR"))
        open(os.path.join(upd, "USRDIR", "EBOOT.BIN"), "wb").write(fc.SELF_MAGIC + b"\0" * 60)
        tfc.add_update_edat(upd)
        if with_elf:
            tfc.write_elf(os.path.join(upd, "USRDIR", "EBOOT.elf"), 21_228_256)
    for n in saves:
        sd = os.path.join(r, "dev_hdd0", "home", "00000001", "savedata", n)
        tfc.write_sfo(os.path.join(sd, "PARAM.SFO"), {"TITLE": "Save", "TITLE_ID": "BCUS98234"})
        open(os.path.join(sd, "GAME.DAT"), "wb").write(b"rpcs3-" + n.encode())
    return r, dump, upd


def _nx(it):
    v = next(it)
    if v is AssertionError:
        raise AssertionError('wizard asked more than the test expected')
    return v


def run_wizard(rpcs3, answers, root, dump=None, update=None, portable=True):
    import types
    a = types.SimpleNamespace(dump=dump or "", update=update or "", eboot="", build=os.path.join(root, "build"),
                              portable=portable, rpcs3=rpcs3, font_dir="", shaders="off")
    it = iter(list(answers) + [AssertionError])
    out, notes = io.StringIO(), []
    go = fr.interactive(a, root, "x.log", notes, ask=lambda q: _nx(it), out=out,
                        vcheck=lambda v: {"status": "error", "current": v, "latest": "", "url": ""})
    return go, a, out.getvalue(), notes


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # 1. locate found
        b1 = os.path.join(tmp, "t1")
        r, dump, upd = make_rpcs3(b1)
        f = ri.discover(r)
        check(f["ok"] and f["disc"] == dump and f["update"] == upd and len(f["saves"]) == 1, f"1 found: {f}")
        check(any(dump in ln for ln in ri.describe(f)) and any(upd in ln for ln in ri.describe(f)), "1 describe lists both folders")
        # 2. not found: an empty folder, and the usual places with an empty HOME
        e = os.path.join(tmp, "empty")
        os.makedirs(e)
        check(not ri.discover(e)["ok"] and ri.describe(ri.discover(e)) == [], "2 empty folder -> not found, no text")
        old = {k: os.environ.get(k) for k in ("HOME", "XDG_CONFIG_HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA")}
        try:
            os.environ.update(HOME=e, USERPROFILE=e)
            for k in ("XDG_CONFIG_HOME", "APPDATA", "LOCALAPPDATA"):
                os.environ.pop(k, None)
            check(not ri.discover(None)["ok"], "2 auto with an empty home -> not found")
        finally:
            for k, v in old.items():
                os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
        # 3. wrong update version is named, update not offered
        r3, _, _ = make_rpcs3(os.path.join(tmp, "t3"), app_ver="01.00")
        f3 = ri.discover(r3)
        check(f3["ok"] and not f3["update"] and "01.00" in f3["update_problem"] and "01.14" in f3["update_problem"], f"3 wrong version {f3}")
        # 4. guidance only for an encrypted EBOOT.BIN without EBOOT.elf
        g = ri.eboot_guidance(upd)
        check(T.ENCRYPTED in g and os.path.join(upd, "USRDIR", "EBOOT.BIN") in g, "4 guidance names the tool and the file")
        r4, _, u4 = make_rpcs3(os.path.join(tmp, "t4"), with_elf=True)
        check(ri.eboot_guidance(u4) == "", "4 no guidance with EBOOT.elf beside it")
        r4b, _, u4b = make_rpcs3(os.path.join(tmp, "t4b"))
        tfc.write_elf(os.path.join(u4b, "USRDIR", "EBOOT.BIN"), 64)
        check(ri.eboot_guidance(u4b) == "", "4 no guidance when EBOOT.BIN is already an ELF")
        # 5. wizard: defaults from RPCS3, guidance, then the decrypted EBOOT is accepted
        root5 = os.path.join(tmp, "root5")
        os.makedirs(root5)
        elf = os.path.join(upd, "USRDIR", "EBOOT.elf")
        state = {"n": 0}

        class Ans:
            def __iter__(self):
                return self

            def __next__(self):
                state["n"] += 1
                q = state["n"]
                if q == 1:
                    return ""           # use RPCS3's folders
                if q in (2, 3, 4):
                    return ""           # dump, update, EBOOT: all defaults
                if q == 5:              # the check failed (E1): the player runs RPCS3's tool, then presses Enter
                    tfc.write_elf(elf, 21_228_256)      # stands in for RPCS3 writing EBOOT.elf (outside the RPCS3 tree rules: test fixture)
                    return ""
                return "-" if q == 7 else "n"      # saves: no; fonts: none; version check: no; build: no
        import types
        a = types.SimpleNamespace(dump="", update="", eboot="", build=os.path.join(root5, "build"), portable=True,
                                  rpcs3=r, font_dir="", shaders="off")
        out, notes = io.StringIO(), []
        asked = []
        it = Ans()
        go = fr.interactive(a, root5, "x.log", notes, ask=lambda q: (asked.append(q), next(it))[1], out=out,
                            vcheck=lambda v: {"status": "error", "current": v, "latest": "", "url": ""})
        o = out.getvalue()
        check(a.dump == dump and a.update == upd, f"5 defaults came from RPCS3: {a.dump} {a.update}")
        check(T.ENCRYPTED.splitlines()[0] in o and "FAIL E1" in o, "5 guidance shown after the E1 failure")
        check(any(q == T.CHECK_AGAIN for q in asked), "5 asked to check again")
        check("OK   E1" in o and a.eboot == elf, f"5 decrypted EBOOT accepted: {a.eboot!r}")
        os.remove(elf)
        # 5b. (pass 67) input that has ended while a FAIL remains: the wizard returns with the FAIL lines, never spins
        import signal
        import time
        import types
        for label, ending in (("EOF", None),):
            r5b, _, u5b = make_rpcs3(os.path.join(tmp, "t5b" + label[:3]), with_elf=True)
            os.remove(os.path.join(u5b, "USRDIR", fc.UPDATE_EDAT))          # U3 stays a FAIL
            a5b = types.SimpleNamespace(dump="", update="", eboot="", build=os.path.join(tmp, "b5b"), portable=True,
                                        rpcs3=r5b, font_dir="", shaders="off")
            out5b, n5b, left = io.StringIO(), [], {"n": 0}

            def ask5b(q, ending=ending, left=left):
                left["n"] += 1
                if ending is None:          # a closed stdin: the first prompts are answered, then input() raises
                    if left["n"] > 4:
                        raise EOFError
                    return ""
                return ending(q)
            if hasattr(signal, "SIGALRM"):
                signal.alarm(10)
            t0 = time.time()
            go5b = fr.interactive(a5b, os.path.join(tmp, "r5b"), "x.log", n5b, ask=ask5b, out=out5b,
                                  vcheck=lambda v: {"status": "error", "current": v, "latest": "", "url": ""})
            if hasattr(signal, "SIGALRM"):
                signal.alarm(0)
            check(go5b is False and "FAIL U3" in out5b.getvalue() and time.time() - t0 < 5 and left["n"] < 20,
                  f"5b {label}: returns False with the FAIL line within seconds ({left['n']} questions)")
        # 6. saves: no / yes / conflict kept / conflict replaced
        r6, _, _ = make_rpcs3(os.path.join(tmp, "t6"), saves=("BCUS98234-AUTO", "BCUS98234-SAVE1"))
        snap6 = snapshot(r6)
        f6 = ri.discover(r6)
        sr = os.path.join(tmp, "port")
        os.makedirs(sr)
        said = []
        check(ri.import_saves(f6, sr, lambda q, d: False, said.append) == (0, 0) and os.listdir(sr) == [], "6 answer no: nothing copied")
        asked6 = []
        check(ri.import_saves(f6, sr, lambda q, d: (asked6.append((q, d)), True)[1], said.append) == (2, 0), "6 answer yes copies both")
        dst = ri.save_dest(sr, "BCUS98234-AUTO")
        check(open(os.path.join(dst, "GAME.DAT"), "rb").read() == b"rpcs3-BCUS98234-AUTO", "6 content copied")
        check(asked6 and asked6[0][1] is False, "6 the default of the copy question is no")
        open(os.path.join(dst, "GAME.DAT"), "wb").write(b"mine")
        qs = []
        # conflict: the first question (copy?) yes, the conflict questions no -> the player's save is kept
        check(ri.import_saves(f6, sr, lambda q, d: (qs.append(q), q == T.SAVES_ASK)[1], said.append) == (0, 2), "6 conflict kept")
        check(open(os.path.join(dst, "GAME.DAT"), "rb").read() == b"mine" and len(qs) == 3, "6 existing save untouched, asked per save")
        check(ri.import_saves(f6, sr, lambda q, d: True, said.append) == (2, 0)
              and open(os.path.join(dst, "GAME.DAT"), "rb").read() == b"rpcs3-BCUS98234-AUTO", "6 conflict replaced only on yes")
        check(ri.import_saves({"saves": []}, sr, lambda q, d: True, said.append) == (0, 0), "6 no saves in RPCS3")
        # 7. a save root inside the RPCS3 tree is refused; the tree is never written
        inside = os.path.join(r6, "dev_hdd0", "portcopy")
        res = ri.import_saves(f6, inside, lambda q, d: True, said.append)
        check(res == (0, 0) and not os.path.exists(inside), f"7 save root inside RPCS3 refused: {res}")
        check(snapshot(r6) == snap6, "7 RPCS3 tree unchanged after every save case")
        # 8. wizard saves question: yes copies into the port's save root (portable = install folder)
        r8, d8, u8 = make_rpcs3(os.path.join(tmp, "t8"), with_elf=True)
        snap8 = snapshot(r8)
        root8 = os.path.join(tmp, "root8")
        os.makedirs(root8)
        go, a8, o8, n8 = run_wizard(r8, ["", "", "", "", "y", "-", "n", "n"], root8)
        check(os.path.isfile(os.path.join(ri.save_dest(root8, "BCUS98234-AUTO"), "GAME.DAT")) and any("saves: copied 1" in n for n in n8),
              f"8 wizard copied the save: {n8}")
        check(snapshot(r8) == snap8, "8 RPCS3 tree unchanged by the wizard")
        # 9. the old prompts stay: --rpcs3 off, and nothing found
        for want in ("off", e):
            root9 = os.path.join(tmp, "root9-" + os.path.basename(want))
            os.makedirs(root9)
            _, _, o9, _ = run_wizard(want, [d8, u8, "", "-", "n", "n"], root9)
            check("RPCS3" not in o9 and "Found your" not in o9, f"9 {want}: no RPCS3 text, old prompts: {o9[:200]!r}")
        # 10. every line the import prints comes from rpcs3_text
        src = open(os.path.join(HERE, "rpcs3_import.py")).read()
        check('say("' not in src and "say(f\"" not in src, "10 rpcs3_import.py has no prose of its own")
    print("test_rpcs3_import:", "FAIL (%d)" % len(FAILS) if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
