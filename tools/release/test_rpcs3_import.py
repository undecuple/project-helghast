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
        tfc.add_update_patches(upd)
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
        # since installer simplification pass 1 the wizard no longer asks about saves (default no): nothing is copied
        go, a8, o8, n8 = run_wizard(r8, ["", "", "", "", ""], root8)
        check(go is True and not os.path.exists(ri.save_dest(root8, "BCUS98234-AUTO")) and any("saves: 1 found, not asked" in n for n in n8),
              f"8 wizard did not ask about or copy the save: {go} {n8}")
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
        # 11. games.yml lists the disc as an ISO file (RPCS3 PR #17855): decrypted .iso and .dec.iso offered, encrypted refused,
        # folder entry unchanged; the old game_path (negative control) refuses the ISO entry
        from iso_fixture import build_iso
        import rpcs3_locate as RL
        sfo = os.path.join(tmp, "iso.sfo")
        tfc.write_sfo(sfo, {"APP_VER": "01.00", "CATEGORY": "DG", "TITLE": "Synthetic", "TITLE_ID": "BCUS98234", "VERSION": "01.01"})
        files = {"PS3_DISC.SFB": b".SFB" + b"\0" * 8, "PS3_GAME/PARAM.SFO": open(sfo, "rb").read(),
                 "PS3_GAME/USRDIR/EBOOT.BIN": fc.SELF_MAGIC + b"\0" * 60,
                 "PS3_GAME/USRDIR/data_ps3.psarc": fc.PSARC_MAGIC + b"\0" * 6000}
        bad = dict(files, **{"PS3_GAME/USRDIR/EBOOT.BIN": b"\x11\x22\x33\x44" + b"\0" * 60})
        for tag, name, fl, good in (("a", "kz3.iso", files, True), ("b", "kz3.dec.iso", files, True), ("c", "kz3-enc.iso", bad, False)):
            r11, _, _ = make_rpcs3(os.path.join(tmp, "t11" + tag))
            iso = build_iso(os.path.join(tmp, "t11" + tag, name), fl)
            open(os.path.join(r11, "config", "games.yml"), "w").write(f"BCUS98234: {iso}\n")
            gp = RL.game_path(r11, "BCUS98234")
            check(gp["ok"] and gp.get("is_iso"), f"11{tag} game_path ok with is_iso for {name}: {gp}")
            f11 = ri.discover(r11)
            if good:
                check(f11["disc"] == iso and f11["disc_iso"] and any(iso in ln for ln in ri.describe(f11)), f"11{tag} ISO offered: {f11}")
            else:
                check(f11["disc"] == "" and "encrypted" in f11["disc_problem"] and any("cannot be used" in ln for ln in ri.describe(f11)),
                      f"11{tag} encrypted ISO refused with a reason: {f11}")
            check(f11["update"], f"11{tag} the update is still found")
            tx = RL.locate(r11, {"BCUS98234": "01.14"})
            check(tx["ok"], f"11{tag} locate ok")
        # folder entry unchanged
        f11 = ri.discover(r)
        check(f11["disc"] == dump and not f11["disc_iso"] and not f11["disc_problem"], "11d folder entry unchanged")
        # a non-ISO file is still refused with a plain text
        odd = os.path.join(tmp, "odd.bin")
        open(odd, "wb").write(b"x")
        open(os.path.join(r, "config", "games.yml"), "w").write(f"BCUS98234: {odd}\n")
        gp = RL.game_path(r, "BCUS98234")
        check(not gp["ok"] and gp["code"] == "missing_disc" and ".iso" in gp["text"], f"11e non-iso file refused: {gp}")
        # negative control: the old game_path logic (is_dir only) refuses an ISO entry
        check(not os.path.isdir(iso) and os.path.isfile(iso), "11f negative control: the old is_dir test rejects the ISO file")
        # 12. wizard modes (ported from the Killzone 2 port's tests): --eboot-kind / --identify / --resolve / --detect
        out = os.path.join(tmp, "res.txt")

        def kv(argv):
            rc = ri.main(argv + ["--out", out])
            d = {}
            for ln in open(out, encoding="utf-8-sig").read().splitlines():
                k, _, v = ln.partition("=")
                d.setdefault(k, []).append(v)
            return rc, {k: v[0] for k, v in d.items()}, d
        e12 = os.path.join(tmp, "e12")
        tfc.write_elf(os.path.join(e12, "good.elf"), 21_228_256)     # 1.14 size
        tfc.write_elf(os.path.join(e12, "old.elf"), 20_745_952)      # the disc's 1.00 EBOOT size
        tfc.write_elf(os.path.join(e12, "odd.elf"), 12_345)
        open(os.path.join(e12, "sce.elf"), "wb").write(fc.SELF_MAGIC + b"\0" * 60)
        open(os.path.join(e12, "junk.elf"), "wb").write(b"junk" * 16)
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "sce.elf"), "--identify"])
        check(rc == 1 and d["status"] == "encrypted" and "decrypted" in d["help"] and "level" not in d, f"12 SCE file: {d}")
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "old.elf"), "--identify", "--sha256", "--title", "BCUS98234"])
        check(rc == 0 and d["status"] == "ok" and d["version"] == "1.00" and d["level"] == "FAIL" and d["code"] == "E1"
              and "1.00" in d["text"] and d["size"] == "20745952" and len(d["sha256"]) == 64, f"12 1.00 EBOOT refused: {d}")
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "good.elf"), "--identify"])
        check(d["version"] == "1.14" and d["level"] in ("OK", "WARN") and d["level"] != "FAIL", f"12 1.14-size EBOOT not refused: {d}")
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "odd.elf"), "--identify"])
        check(d["level"] == "WARN", f"12 unknown size warns: {d}")
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "junk.elf"), "--identify"])
        check(d["level"] == "FAIL", f"12 not an ELF fails: {d}")
        rc, d, _ = kv(["--eboot-kind", os.path.join(e12, "good.elf")])      # no --identify: kind only
        check(rc == 0 and d == {"status": "ok"}, f"12 plain --eboot-kind: {d}")
        # resolve: everything there
        r12, dump12, upd12 = make_rpcs3(os.path.join(tmp, "t12"), with_elf=True)
        rc, d, _ = kv(["--resolve", "--rpcs3", r12])
        check(rc == 0 and d["status"] == "ok" and d["dump"] == dump12 and d["update"] == upd12
              and d["eboot"] == os.path.join(upd12, "USRDIR", "EBOOT.elf") and d["title"] == "BCUS98234", f"12 resolve ok: {d}")
        # update with only the encrypted EBOOT.BIN
        r12b, _, _ = make_rpcs3(os.path.join(tmp, "t12b"))
        rc, d, dd = kv(["--resolve", "--rpcs3", r12b])
        check(rc == 1 and d["status"] == "problems" and dd["problem"] == ["encrypted_eboot|"] and "encrypted" in d["help"], f"12 encrypted: {dd}")
        # the 1.00 EBOOT sitting as the update's EBOOT.elf
        r12c, _, u12c = make_rpcs3(os.path.join(tmp, "t12c"))
        tfc.write_elf(os.path.join(u12c, "USRDIR", "EBOOT.elf"), 20_745_952)
        rc, d, dd = kv(["--resolve", "--rpcs3", r12c])
        check(rc == 1 and d["status"] == "problems" and d["eboot"] == "" and dd["problem"][0].startswith("eboot_e1|")
              and "1.00" in dd["problem"][0], f"12 1.00 EBOOT as update elf refused: {dd}")
        # RPCS3 found but the update is 1.00
        r12d, _, _ = make_rpcs3(os.path.join(tmp, "t12d"), app_ver="01.00")
        rc, d, dd = kv(["--resolve", "--rpcs3", r12d])
        check(rc == 1 and d["status"] == "problems" and d["update"] == "" and dd["problem"][0].startswith("wrong_version|")
              and "01.00" in dd["problem"][0] and "not 1.14" in d["help"], f"12 wrong-version update: {dd}")
        # RPCS3 found, no update at all
        r12e, _, _ = make_rpcs3(os.path.join(tmp, "t12e"), with_update=False)
        rc, d, dd = kv(["--resolve", "--rpcs3", r12e])
        check(d["status"] == "problems" and any(x.startswith("missing_update|") for x in dd["problem"]), f"12 no update: {dd}")
        # no RPCS3 in the folder
        rc, d, _ = kv(["--resolve", "--rpcs3", e])
        check(rc == 1 and d["status"] == "not_found", f"12 not found: {d}")
        rc, d, _ = kv(["--detect", "--rpcs3", r12])
        check(rc == 0 and d["status"] == "ok" and d["root"], f"12 detect: {d}")
        saved = {k: os.environ.get(k) for k in ("HOME", "XDG_CONFIG_HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA")}
        try:
            os.environ.update(HOME=e, USERPROFILE=e)
            for k in ("XDG_CONFIG_HOME", "APPDATA", "LOCALAPPDATA"):
                os.environ.pop(k, None)
            rc, d, _ = kv(["--detect"])
        finally:
            for k, v in saved.items():
                os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
        check(rc == 1 and d["status"] == "not_found", f"12 detect none: {d}")
        # the result file is UTF-8 with BOM (Inno reads it), and the RPCS3 tree is untouched by every mode
        check(open(out, "rb").read(3) == b"\xef\xbb\xbf", "12 --out has a BOM")
        snap = snapshot(r12)
        kv(["--resolve", "--rpcs3", r12])
        check(snapshot(r12) == snap, "12 resolve writes nothing in the RPCS3 tree")
        # an unexpected error still leaves status=error in --out
        orig = ri.resolve
        ri.resolve = lambda h: 1 / 0
        try:
            import contextlib
            with contextlib.redirect_stderr(io.StringIO()):
                rc = ri.main(["--resolve", "--rpcs3", r12, "--out", out])
        finally:
            ri.resolve = orig
        check(rc == 3 and "status=error" in open(out, encoding="utf-8-sig").read(), "12 error -> status=error")
        # the help texts hold no decryption tool or menu path
        low = (T.HELP_ENCRYPTED + T.HELP_NO_ELF + T.HELP_UPDATE + T.HELP_VERSION + T.HELP_DISC).lower()
        check(not any(w in low for w in ("file >", "install packages", "keys", "decrypt it with")), "12 help names no tool/menu")
    print("test_rpcs3_import:", "FAIL (%d)" % len(FAILS) if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
