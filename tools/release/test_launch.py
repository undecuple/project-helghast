#!/usr/bin/env python3
"""Host test for tools/release/launch.py (the logic behind `Project Helghast.exe`) and firstrun.py's per-step
fingerprints, on a synthetic install tree in a temp folder (no game data, no toolchain, no network, no Windows).
Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_launch.py

Cases: first-build without paths.json; missing-inputs when the dump is gone; resume without a stamp or kz3.exe; run
after a stamp written by firstrun's own after_stamp; --print JSON (game argv, cwd portable vs %APPDATA%, the four
variables, PS3_HDD0_ROOT only for an RPCS3 dev_hdd0/game/<id> update); rebuild reasons for a changed EBOOT, a lift
input (ppu_overrides.txt), the port version (VERSION.txt); firstrun markers: a lift-input change invalidates the lift
steps' markers but not load's; main(): "No" starts nothing, "Yes" runs the interactive build in a new console then
the game with its environment; resume uses --bar with the recorded paths; a failed build stops; the daily opt-in
version check (due once a day, off by default, never with check_updates=0); a newer result is asked once and a "No"
is remembered; --repair (pass 6) verifies the install against manifest.json (missing / changed file stops with a
message), then runs firstrun --bar with the recorded paths and never starts the game; "No" does nothing; a failed
repair returns 1; never built = the first-build question.
"""
import contextlib
import datetime
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def write(p, text="x"):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


class FakeUI:
    def __init__(self, answers):
        self.answers, self.asked, self.infos, self.urls = list(answers), [], [], []

    def ask(self, text):
        self.asked.append(text)
        return self.answers.pop(0) if self.answers else False

    def info(self, text):
        self.infos.append(text)

    def open_url(self, url):
        self.urls.append(url)


def make_tree(tmp):
    root = os.path.join(tmp, "Project Helghast")
    hdd0 = os.path.join(tmp, "rpcs3", "dev_hdd0")
    dump, update = os.path.join(tmp, "dump"), os.path.join(hdd0, "game", "BCUS98234")
    eboot = os.path.join(update, "USRDIR", "EBOOT.elf")
    write(os.path.join(dump, "PS3_GAME", "PARAM.SFO"))
    write(eboot, "fake elf")
    write(os.path.join(root, "port", "spu", "ppu_overrides.txt"), "a\n")
    write(os.path.join(root, "VERSION.txt"), "0.1.0")
    os.makedirs(os.path.join(root, "ps3recomp", "tools"))
    return root, dump, update, eboot, hdd0


def record(root, dump, update, eboot):
    c = fr.Ctx(root=root, build=os.path.join(root, "build"), dump=dump, update=update, eboot=eboot,
               facts={"eboot_elf": {"size": 8}})
    fr.after_record(c)
    return c


def stamp(c):
    write(c.b("obj", "kz3.exe"), "MZ")
    fr.after_stamp(c)


def printed(root, extra=(), **kw):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        la.main(["--root", root, "--print", *extra], ui=FakeUI([]), **kw)
    return json.loads(buf.getvalue())


REAL_KNOWN_FOLDER = la.known_folder


def main():
    # The tests pass APPDATA / LOCALAPPDATA in an environment dict. On Windows the real known folders would win over it
    # (launch.user_folder, by design), so they are switched off here and exercised in section 12.
    la.known_folder = lambda name: ""
    tmp = tempfile.mkdtemp(prefix="kz3-launch-")
    try:
        root, dump, update, eboot, hdd0 = make_tree(tmp)
        env = {"APPDATA": os.path.join(tmp, "AppData"), "PATH": "/bin"}
        check(la.decide(root)[0] == "first-build", "1 no paths.json -> first-build")
        p = printed(root, environ=env)
        check(p["action"] == "first-build" and "--interactive" in p["build"] and "--portable" not in p["build"],
              f"1 print first-build {p}")

        # 1b. the installer's folders in kz3-install.ini [paths]: the first build starts from them and checks first
        ini_p = os.path.join(root, "kz3-install.ini")
        write(ini_p, "[paths]\ndump = %s\nupdate = %s\neboot = %s\nfont_dir = auto\n" % (dump, update, eboot))
        fb = printed(root, environ=env)["build"]
        check("--check-first" in fb and "--interactive" in fb and fb[fb.index("--dump") + 1] == dump and
              fb[fb.index("--update") + 1] == update and fb[fb.index("--eboot") + 1] == eboot and
              fb[fb.index("--font-dir") + 1] == "auto" and fb[fb.index("--rpcs3") + 1] == "off",
              f"1b first-build with ini [paths] carries the folders {fb}")
        write(ini_p, "[paths]\ndump = \nupdate = %s\n" % update)
        check("--check-first" not in printed(root, environ=env)["build"], "1b ini without a dump -> plain --interactive")
        os.remove(ini_p)
        check("--check-first" not in printed(root, environ=env)["build"], "1b no ini -> plain --interactive")

        c = record(root, dump, update, eboot)
        os.rename(dump, dump + ".moved")
        a, why = la.decide(root)
        check(a == "missing-inputs" and dump in why, f"2 missing dump {a} {why}")
        write(os.path.join(root, "kz3-install.ini"), "[paths]\ndump = %s\nupdate = %s\n" % (dump, update))
        mi = la.firstrun_cmd(root, "missing-inputs", {"dump": dump, "update": update}, False, "py")
        check("--check-first" not in mi and "--interactive" in mi and "--dump" not in mi, f"2 missing-inputs still asks {mi}")
        os.remove(os.path.join(root, "kz3-install.ini"))
        os.rename(dump + ".moved", dump)

        check(la.decide(root)[0] == "resume", "3 no stamp -> resume")
        p = printed(root, environ=env)
        b = p["build"]
        check("--bar" in b and b[b.index("--dump") + 1] == dump and b[b.index("--eboot") + 1] == eboot
              and "--interactive" not in b, f"3 resume command {b}")

        stamp(c)
        os.remove(c.b("kz3.exe"))
        check(la.decide(root)[0] == "resume", "4 stamp without kz3.exe -> resume")
        stamp(c)
        check(la.decide(root) == ("run", ""), f"4 built -> run {la.decide(root)}")
        p = printed(root, environ=env)
        check(p["argv"] == [os.path.join(root, "build", "kz3.exe")], f"5 argv: kz3.exe alone {p.get('argv')}")
        gi = p["ini"]
        check(gi["eboot"] == eboot and gi["vfs_root"] == os.path.join(root, "vfs") and gi["hdd0_root"] == hdd0
              and gi["data_dir"] == p["cwd"] and gi["update_dir"] == os.path.join(update, "USRDIR"), f"5 ini {gi}")
        ini0 = la.read_game_ini(os.path.join(root, "build", "kz3.ini"))   # written by after_stamp (firstrun)
        check(ini0.get("eboot") == eboot and ini0.get("vfs_root") == os.path.join(root, "vfs"), f"5 stamp ini {ini0}")
        check(p["cwd"] == os.path.join(env["APPDATA"], "Project Helghast"), f"5 cwd appdata {p.get('cwd')}")
        e = p["env"]
        check(e.get("PS3_VFS_ROOT") == os.path.join(root, "vfs") and e.get("KZ3_UPDATE_DIR") == os.path.join(update, "USRDIR")
              and e.get("PS3_HDD0_ROOT") == hdd0
              and e.get("PS3_EDAT_CACHE") == os.path.join(root, "build", "cache", "edat"), f"5 env {e}")
        check(la.rpcs3_hdd0(os.path.join(tmp, "elsewhere", "BCUS98234")) == "", "5 no hdd0 outside RPCS3 layout")
        fr.write_install_ini(root, True, False)
        p = printed(root, environ=env)
        check(p["cwd"] == root and p["portable"], f"5 portable cwd {p.get('cwd')}")

        # 6. rebuild reasons
        st = os.stat(eboot)
        os.utime(eboot, (st.st_atime, st.st_mtime + 10))
        a, why = la.decide(root)
        check(a == "rebuild" and "EBOOT" in why, f"6 EBOOT touched {a} {why}")
        os.utime(eboot, (st.st_atime, st.st_mtime))
        check(la.decide(root)[0] == "run", "6 EBOOT restored -> run")
        write(os.path.join(root, "VERSION.txt"), "0.1.1")
        a, why = la.decide(root)
        check(a == "rebuild" and "updated" in why, f"6 version {a} {why}")
        write(os.path.join(root, "VERSION.txt"), "0.1.0")
        write(os.path.join(root, "port", "spu", "ppu_overrides.txt"), "b\n")
        a, why = la.decide(root)
        check(a == "rebuild" and "lift" in why, f"6 lift input {a} {why}")

        # 7. firstrun markers follow their keys
        c2 = fr.Ctx(root=root, build=c.build, dump=dump, update=update, eboot=eboot, facts=c.facts)
        steps = {s.name: (i, s) for i, s in enumerate(fr.STEPS)}
        for n in ("load", "ppu-lift", "compile"):
            fr.mark_done(c2, *steps[n], 1.0)
        write(os.path.join(root, "port", "spu", "ppu_lift_fixups-114.txt"), "end 0x1 0x2\n")
        c3 = fr.Ctx(root=root, build=c.build, dump=dump, update=update, eboot=eboot, facts=c.facts)
        check(fr.is_done(c3, *steps["load"]) and not fr.is_done(c3, *steps["ppu-lift"])
              and not fr.is_done(c3, *steps["compile"]), "7 lift change: load kept, lift + compile re-run")
        os.remove(os.path.join(root, "port", "spu", "ppu_lift_fixups-114.txt"))
        write(os.path.join(root, "VERSION.txt"), "0.2.0")
        c4 = fr.Ctx(root=root, build=c.build, dump=dump, update=update, eboot=eboot, facts=c.facts)
        check(fr.is_done(c4, *steps["ppu-lift"]) and not fr.is_done(c4, *steps["compile"]),
              "7 version change: lift kept, compile re-runs")
        write(os.path.join(root, "VERSION.txt"), "0.1.0")
        write(os.path.join(root, "port", "spu", "ppu_overrides.txt"), "a\n")
        check(la.decide(root)[0] == "run", "7 inputs restored -> run")

        # 8. main(): game start, build offers; the notice fonts go next to kz3.exe (RSX pass 38)
        write(os.path.join(root, "fonts", "Rajdhani-Medium.ttf"), "medium")
        write(os.path.join(root, "fonts", "Rajdhani-Bold.ttf"), "bold")
        calls = []

        def run(argv, **kw):
            calls.append((argv, kw))
            return 0
        with open(os.path.join(root, "build", "kz3.ini"), "a", encoding="utf-8") as f:
            f.write("window_size = 1080p\n")       # a hand edit: kept by the launcher's rewrite
        rc = la.main(["--root", root], ui=FakeUI([]), run=run, environ=env)
        check(rc == 0 and calls and calls[0][0] == [os.path.join(root, "build", "kz3.exe")]
              and calls[0][1]["cwd"] == root and "PS3_VFS_ROOT" not in calls[0][1]["env"]
              and calls[0][1]["env"]["PATH"] == "/bin", f"8 run game: kz3.exe alone, plain env {calls[:1]}")
        gi = la.read_game_ini(os.path.join(root, "build", "kz3.ini"))
        check(gi.get("vfs_root") == os.path.join(root, "vfs") and gi.get("data_dir") == root
              and gi.get("window_size") == "1080p", f"8 kz3.ini rewritten (portable data_dir, hand edit kept) {gi}")
        calls.clear()
        rc = la.main(["--root", root, "-x"], ui=FakeUI([]), run=run, environ=env)
        check(rc == 0 and calls and calls[0][0] == [os.path.join(root, "build", "kz3.exe"), eboot, "-x"],
              f"8 extra game arguments: the EBOOT first as before {calls[:1]}")

        # 8b. "Export pipeline template": a stale export is removed first; no new file or a non-zero exit is a failure
        exp = os.path.join(root, la.TEMPLATE_EXPORT)

        def export_run(game_rc, writes):
            def r(argv, **kw):
                if argv[0].endswith("kz3.exe"):
                    if writes:
                        write(exp, "new")
                    return game_rc
                return 0      # the pso_template.py check
            write(exp, "stale")
            ui = FakeUI([])
            ui.open_folder = lambda p: None
            return la.main(["--root", root, "--export-template"], ui=ui, run=r, environ=env), ui
        rc, ui = export_run(0, False)
        check(rc != 0 and not os.path.exists(exp), f"8b export that wrote nothing fails, stale file gone (rc {rc})")
        rc, ui = export_run(3, True)
        check(rc == 3, f"8b export with a non-zero exit fails (rc {rc})")
        rc, ui = export_run(0, True)
        check(rc == 0 and os.path.isfile(exp), f"8b export that wrote a new file succeeds (rc {rc})")
        os.remove(exp)
        vl = os.path.join(root, "vfs", "PS3_GAME")
        check(la._is_dir_link(vl) and os.path.realpath(vl) == os.path.realpath(os.path.join(dump, "PS3_GAME"))
              and os.path.isfile(os.path.join(vl, "PARAM.SFO")), "8 vfs/PS3_GAME links to the dump's PS3_GAME")
        check(la.ensure_vfs(root, dump) == (os.path.join(root, "vfs"), "") and la._is_dir_link(vl),
              "8 vfs link kept on the next start")
        dump2 = os.path.join(tmp, "dump2")
        write(os.path.join(dump2, "PS3_GAME", "PARAM.SFO"), "two")
        check(la.ensure_vfs(root, dump2)[1] == "" and open(os.path.join(vl, "PARAM.SFO")).read() == "two"
              and os.path.isfile(os.path.join(dump, "PS3_GAME", "PARAM.SFO")),
              "8 a new dump relinks; the old dump's files stay")
        la.ensure_vfs(root, dump)
        try:
            os.unlink(vl)
        except OSError:
            os.rmdir(vl)      # a Windows junction: RemoveDirectory removes the link only
        write(os.path.join(vl, "keep.txt"), "mine")
        v, err = la.ensure_vfs(root, dump2)
        check(err and os.path.isfile(os.path.join(vl, "keep.txt"))
              and os.path.isfile(os.path.join(dump2, "PS3_GAME", "PARAM.SFO")),
              f"8 a real PS3_GAME folder in vfs is never removed ({err})")
        os.remove(os.path.join(root, "vfs", "link.json"))
        v, err = la.ensure_vfs(root, dump)
        check("real folder" in err and os.path.isfile(os.path.join(vl, "keep.txt")),
              f"8 a real PS3_GAME folder in vfs is never removed ({err})")
        shutil.rmtree(vl)
        la.ensure_vfs(root, dump)
        check(os.path.isdir(os.path.join(root, "build", "cache", "edat")), "8 edat cache made")
        # 8c (round KZ3-03): link-free. A payload whose kz3_ini.h has KZ3_CACHE_ROOT reads the dump in place and
        # writes to <root>/vfs; the stamp records it; an old payload keeps the junction.
        st = os.path.join(root, "build", "build-stamp.json")
        check(not la.link_free(root) and la.game_env(root, {"dump": dump, "eboot": eboot}, False, env)[3]["PS3_VFS_ROOT"] == os.path.join(root, "vfs")
              and "KZ3_CACHE_ROOT" not in la.game_env(root, {"dump": dump, "eboot": eboot}, False, env)[3], "8c old payload: junction mode")
        write(os.path.join(root, "port", "kz3", "kz3_ini.h"), '{"cache_root", "KZ3_CACHE_ROOT", 1}')
        check(fr.cache_root_supported(root), "8c payload with the key is detected")
        c8 = record(root, dump, update, eboot)
        stamp(c8)
        check(json.load(open(st)).get("cache_root") is True and la.link_free(root), "8c stamp records cache_root")
        gi8 = la.game_ini(root, {"dump": dump, "update": update, "eboot": eboot}, False, env)
        check(gi8["vfs_root"] == dump and gi8["cache_root"] == os.path.join(root, "vfs"), f"8c ini: dump read, vfs written {gi8}")
        e8 = la.game_env(root, {"dump": dump, "update": update, "eboot": eboot}, False, env)[3]
        check(e8["PS3_VFS_ROOT"] == dump and e8["KZ3_CACHE_ROOT"] == os.path.join(root, "vfs"), f"8c env {e8}")
        ini8 = la.read_game_ini(os.path.join(root, "build", "kz3.ini"))
        check(ini8.get("vfs_root") == dump and ini8.get("cache_root") == os.path.join(root, "vfs"), f"8c written kz3.ini {ini8}")
        # no link made, an older start's link is dropped, its target and a real PS3_GAME folder stay
        vl8 = os.path.join(root, "vfs", "PS3_GAME")
        if os.path.lexists(vl8):
            la.ensure_vfs(root, dump, True)
            check(not os.path.lexists(vl8) and os.path.isfile(os.path.join(dump, "PS3_GAME", "PARAM.SFO")),
                  "8c old link dropped, dump untouched")
        check(la.ensure_vfs(root, dump, True) == (os.path.join(root, "vfs"), "") and not os.path.lexists(vl8),
              "8c link-free ensure_vfs makes only the folder")
        write(os.path.join(vl8, "keep.txt"), "mine")
        check(la.ensure_vfs(root, dump, True)[1] == "" and os.path.isfile(os.path.join(vl8, "keep.txt")), "8c real folder left alone")
        shutil.rmtree(vl8)
        os.remove(st)
        stamp(record(root, dump, update, eboot))   # keep later sections' state
        os.remove(os.path.join(root, "port", "kz3", "kz3_ini.h"))
        os.remove(st)
        stamp(record(root, dump, update, eboot))
        bf = os.path.join(root, "build", "fonts")
        check(sorted(os.listdir(bf)) == ["Rajdhani-Bold.ttf", "Rajdhani-Medium.ttf"]
              and open(os.path.join(bf, "Rajdhani-Bold.ttf")).read() == "bold", "8 notice fonts placed in build/fonts")
        check(fr.place_fonts(root, os.path.join(root, "build")) == [], "8 fonts already in place: nothing copied")
        write(os.path.join(root, "lang", "port_ui.tsv"), "k\ten\n")
        check(fr.place_lang(root, os.path.join(root, "build")) == ["port_ui.tsv"]
              and open(os.path.join(root, "build", "lang", "port_ui.tsv")).read() == "k\ten\n", "8 port_ui.tsv placed in build/lang")
        check(fr.place_lang(root, os.path.join(root, "build")) == [], "8 lang table already in place: nothing copied")
        write(os.path.join(root, "lang", "port_ui.tsv"), "k\ten\nj\tes\n")
        check(fr.place_lang(root, os.path.join(root, "build")) == ["port_ui.tsv"], "8 a changed lang table is copied again")
        write(os.path.join(root, "fonts", "Rajdhani-Bold.ttf"), "bold v2")
        check(fr.place_fonts(root, os.path.join(root, "build")) == ["Rajdhani-Bold.ttf"], "8 a changed font is copied again")
        # SDL3.dll next to kz3.exe (patch 4010): placed from the payload, left alone when identical, replaced when changed
        sd = os.path.join(root, "runtime", "SDL3.dll")
        check(not fr.place_sdl3(root, os.path.join(root, "build")) and not os.path.exists(os.path.join(root, "build", "SDL3.dll")),
              "8 an old payload without runtime/SDL3.dll: nothing placed, no error")
        write(sd, "dll v1")
        check(fr.place_sdl3(root, os.path.join(root, "build")) is True
              and open(os.path.join(root, "build", "SDL3.dll")).read() == "dll v1", "8 SDL3.dll placed in build/")
        check(fr.place_sdl3(root, os.path.join(root, "build")) is False, "8 SDL3.dll already in place: not copied")
        write(sd, "dll v2")
        check(fr.place_sdl3(root, os.path.join(root, "build")) is True
              and open(os.path.join(root, "build", "SDL3.dll")).read() == "dll v2", "8 a changed SDL3.dll (same size) is copied again")
        os.remove(os.path.join(root, "build", "build-stamp.json"))
        calls.clear()
        ui = FakeUI([False])
        rc = la.main(["--root", root], ui=ui, run=run, environ=env)
        check(rc == 0 and not calls and len(ui.asked) == 1 and "did not finish" in ui.asked[0], "8 No starts nothing")

        def run_build(argv, **kw):
            calls.append((argv, kw))
            if "--bar" in argv:
                stamp(c)
            return 0
        calls.clear()
        rc = la.main(["--root", root], ui=FakeUI([True]), run=run_build, environ=env)
        check(rc == 0 and len(calls) == 2 and "--bar" in calls[0][0] and calls[1][0][0].endswith("kz3.exe"),
              f"8 Yes: build then game {[x[0][:3] for x in calls]}")
        os.remove(os.path.join(root, "build", "build-stamp.json"))
        calls.clear()
        ui = FakeUI([True])
        rc = la.main(["--root", root], ui=ui, run=lambda argv, **kw: calls.append(argv) or 1, environ=env)
        check(rc == 1 and len(calls) == 1 and ui.infos and "did not finish" in ui.infos[0], "8 failed build stops")
        shutil.rmtree(os.path.join(root, "build"))
        calls.clear()
        rc = la.main(["--root", root], ui=FakeUI([True]), run=run, environ=env)
        check(rc == 1 and "--interactive" in calls[0][0] and "--portable" in calls[0][0],
              f"8 first build interactive portable, nothing built -> 1 ({rc})")

        # 9. version check
        c = record(root, dump, update, eboot)
        stamp(c)
        started = []
        day = datetime.date(2026, 10, 11)
        la.main(["--root", root], ui=FakeUI([]), run=run, start=lambda a, **k: started.append(a), today=day,
                environ=env)
        check(not started, "9 check_updates=0: no check")
        fr.write_install_ini(root, True, True)
        la.main(["--root", root], ui=FakeUI([]), run=run, start=lambda a, **k: started.append(a), today=day,
                environ=env)
        check(len(started) == 1 and "--current" in started[0] and "0.1.0" in started[0]
              and la.read_ini(root)["last_check"] == "2026-10-11", f"9 due check started {started}")
        la.main(["--root", root], ui=FakeUI([]), run=run, start=lambda a, **k: started.append(a), today=day,
                environ=env)
        check(len(started) == 1, "9 once a day")
        write(os.path.join(root, "logs", "version-check.txt"),
              "VERSION status=newer current=0.1.0 latest=v0.2.0 url=https://example.invalid/r setup=-\n")
        ui = FakeUI([False])
        la.main(["--root", root], ui=ui, run=run, today=day, environ=env)
        check(len(ui.asked) == 1 and "v0.2.0" in ui.asked[0] and la.read_ini(root)["dismissed"] == "v0.2.0",
              "9 newer asked, No remembered")
        ui = FakeUI([True])
        la.main(["--root", root], ui=ui, run=run, today=day, environ=env)
        check(not ui.asked, "9 dismissed version not asked again")
        write(os.path.join(root, "logs", "version-check.txt"),
              "VERSION status=newer current=0.1.0 latest=v0.3.0 url=https://example.invalid/r3 setup=-\n")
        ui = FakeUI([True])
        la.main(["--root", root], ui=ui, run=run, today=day, environ=env)
        check(ui.urls == ["https://example.invalid/r3"], f"9 Yes opens the page {ui.urls}")

        # 10. --repair ("Verify and rebuild", pass 6): never reaches the game; checks, then firstrun --bar
        os.remove(os.path.join(root, "logs", "version-check.txt"))
        fr.write_install_ini(root, True, False)   # no background version check from here on (never a real request)
        calls = []

        def run_repair(argv, **kw):
            calls.append(argv)
            stamp(c)
            return 0
        ui = FakeUI([True])
        rc = la.main(["--root", root, "--repair"], ui=ui, run=run_repair, environ=env)
        check(rc == 0 and len(calls) == 1 and "--bar" in calls[0] and calls[0][calls[0].index("--dump") + 1] == dump
              and not any(x.endswith("kz3.exe") or x == "--repair" for x in calls[0])
              and ui.infos and "Verified" in ui.infos[-1], f"10 repair: firstrun --bar, no game {calls} {ui.infos}")
        calls.clear()
        ui = FakeUI([False])
        rc = la.main(["--root", root, "--repair"], ui=ui, run=run_repair, environ=env)
        check(rc == 0 and not calls and len(ui.asked) == 1 and "rebuild what changed" in ui.asked[0], "10 No = nothing")
        good = os.path.join(root, "VERSION.txt")
        with open(good, "rb") as f:
            gh = hashlib.sha256(f.read()).hexdigest()
        man = {"format": 1, "files": [{"path": "VERSION.txt", "size": os.path.getsize(good), "sha256": gh},
                                      {"path": "kz3-setup/release/gone.py", "size": 1, "sha256": "0" * 64}]}
        write(os.path.join(root, "manifest.json"), json.dumps(man))
        check(la.verify_install(root) == (2, ["kz3-setup/release/gone.py (missing)"]), "10 verify: one missing")
        man["files"].append({"path": "run.cmd", "size": 9, "sha256": "0" * 64, "portable_only": True})
        write(os.path.join(root, "manifest.json"), json.dumps(man))
        check(la.verify_install(root) == (2, ["kz3-setup/release/gone.py (missing)"]),
              "10 verify: a portable_only entry (run.cmd) absent from an installed copy is not missing")
        man["files"].pop()
        write(os.path.join(root, "manifest.json"), json.dumps(man))
        check("window was closed" in la.did_not_finish(0xC000013A, "The build did not finish")
              and "code 1)" in la.did_not_finish(1, "The build did not finish"), "10 console closed: its own wording")
        nl = os.path.join(tmp, "lognote"); os.makedirs(nl, exist_ok=True)
        check("No log was written yet" in la.log_note(nl), "10b no first-run log yet: the dialog says so, no path")
        open(os.path.join(nl, la.FIRSTRUN_LOG), "w").close()
        check(la.FIRSTRUN_LOG in la.log_note(nl) and "issues" in la.log_note(nl), "10b first-run log present: path and report line")
        write(good, "0.1.9")
        check(la.verify_install(root)[1][0] == "VERSION.txt (changed)", "10 verify: one changed")
        ui = FakeUI([True])
        rc = la.main(["--root", root, "--repair"], ui=ui, run=run_repair, environ=env)
        check(rc == 1 and not calls and not ui.asked and "2 of the 2 files" in ui.infos[0], "10 damaged install stops")
        p = printed(root, environ=env, extra=["--repair"])
        check(len(p["repair"]["problems"]) == 2 and "--bar" in p["repair"]["build"], f"10 print {p.get('repair')}")
        write(good, "0.1.0")
        os.remove(os.path.join(root, "manifest.json"))
        fails = []
        rc = la.main(["--root", root, "--repair"], ui=FakeUI([True]), run=lambda a, **k: fails.append(a) or 1,
                     environ=env)
        check(rc == 1 and len(fails) == 1, "10 failed repair -> 1")
        shutil.rmtree(os.path.join(root, "build"))
        calls.clear()
        ui = FakeUI([True])
        la.main(["--root", root, "--repair"], ui=ui, run=lambda a, **k: calls.append(a) or 0, environ=env)
        check(calls and "--interactive" in calls[0] and "not built" in ui.asked[0], "10 never built -> first build")

        # 11. the data folder's one-time move after a rename: %APPDATA%/<old name> -> names.DATA_FOLDER (generic: the
        # test's old name is a fixture, names.OLD_DATA_FOLDERS is patched for the main() part)
        ad = os.path.join(tmp, "appdata11")
        OLD = "Old Name 11"
        old, new = os.path.join(ad, OLD), os.path.join(ad, la.SAVE_FOLDER)
        check(la.migrate_data({}, OLD, appdata=ad) == "" and la.migrate_data({}, OLD) == ""
              and la.migrate_data({"APPDATA": ad}, la.SAVE_FOLDER.upper()) == "", "11 nothing to move, no APPDATA, same name")
        write(os.path.join(old, "gamedata", "BCUS98234-SAVE", "PARAM.SFO"), "save")
        write(os.path.join(old, "keybinds.ini"), "keys")
        write(os.path.join(old, "online.ini"), "online")
        write(os.path.join(old, "cache", "shaders", "pso_keys.bin"), "pso")
        msg = la.migrate_data({"APPDATA": ad}, OLD)
        check(not os.path.exists(old) and open(os.path.join(new, "gamedata", "BCUS98234-SAVE", "PARAM.SFO")).read() == "save"
              and open(os.path.join(new, "keybinds.ini")).read() == "keys"
              and os.path.isfile(os.path.join(new, "online.ini"))
              and os.path.isfile(os.path.join(new, "cache", "shaders", "pso_keys.bin")) and "moved" in msg,
              f"11 old folder renamed whole: {msg}")
        check(la.migrate_data({"APPDATA": ad}, OLD) == "", "11 second start: nothing left to move")
        write(os.path.join(old, "keybinds.ini"), "older keys")
        write(os.path.join(old, "online.ini"), "older online")
        os.remove(os.path.join(new, "online.ini"))
        msg = la.migrate_data({}, OLD, appdata=ad)
        check(open(os.path.join(new, "keybinds.ini")).read() == "keys"
              and open(os.path.join(new, "online.ini")).read() == "older online"
              and os.path.isfile(os.path.join(old, "keybinds.ini")) and "keybinds.ini" in msg,
              f"11 both present: missing entries moved, nothing overwritten: {msg}")
        os.remove(os.path.join(old, "keybinds.ini"))
        check(la.migrate_data({}, OLD, appdata=ad) == f"data: moved 0 entries of {OLD} into {la.SAVE_FOLDER}"
              and not os.path.exists(old), "11 empty old folder removed")
        # via main(): a normal install moves it before anything else and logs it; a portable one leaves it alone
        write(os.path.join(old, "keybinds.ini"), "keys")
        shutil.rmtree(new)
        saved_old = la.names.OLD_DATA_FOLDERS
        la.names.OLD_DATA_FOLDERS = (OLD,)
        try:
            fr.write_install_ini(root, True, False)
            la.main(["--root", root], ui=FakeUI([False]), run=lambda a, **k: 0, environ={"APPDATA": ad})
            check(os.path.isdir(old) and not os.path.exists(new), "11 portable install: APPDATA untouched")
            fr.write_install_ini(root, False, False)
            la.main(["--root", root], ui=FakeUI([False]), run=lambda a, **k: 0, environ={"APPDATA": ad})
        finally:
            la.names.OLD_DATA_FOLDERS = saved_old
        with open(os.path.join(root, "logs", "launcher.log"), encoding="utf-8") as f:
            logged = f.read()
        lines = [ln.split(" ", 1)[1] for ln in logged.splitlines()]
        check(lines[0].startswith("start: ") and lines[1].startswith(f"version: {la.names.VERSION}"),
              f"13a launcher.log: the version line follows start ({lines[:2]})")
        os.makedirs(os.path.join(root, "build"), exist_ok=True)
        write(os.path.join(root, "build", "build-stamp.json"),
              json.dumps({"version": "0.1.2-x", "patchset": "abc123", "built": "2026-10-28T10:00:00"}))
        check(la.version_line(root) == f"version: {la.names.VERSION} (build stamp: version 0.1.2-x, patchset abc123, built 2026-10-28T10:00:00)",
              "13b version line carries the build stamp when it exists")
        check(not os.path.exists(old) and os.path.isfile(os.path.join(new, "keybinds.ini"))
              and f"data: moved {OLD} to {la.SAVE_FOLDER}" in logged, "11 main() moves it and logs it")
        # 12: known folders (SHGetKnownFolderPath) win over the variables; no result falls back to them, then to ""
        saved_kf = la.known_folder
        try:
            la.known_folder = {"AppData": "K:/Roaming", "LocalAppData": "K:/Local"}.get
            env = {"APPDATA": "E:/r", "LOCALAPPDATA": "E:/l"}
            check(la.user_folder(env, "AppData") == "K:/Roaming" and la.user_folder(env, "LocalAppData") == "K:/Local",
                  "12a known folders win over the environment")
            check(la.data_dir("R", False, env) == os.path.join("K:/Roaming", la.SAVE_FOLDER)
                  and la.cache_dir("R", False, env) == os.path.join("K:/Local", la.SAVE_FOLDER),
                  "12b data_dir and cache_dir use them")
            la.known_folder = lambda name: ""
            check(la.user_folder(env, "AppData") == "E:/r" and la.user_folder(env, "LocalAppData") == "E:/l",
                  "12c no known folder: the variables")
            check(la.user_folder({}, "AppData") == "" and la.data_dir("R", False, {}) == "R",
                  "12d neither: empty, data stays in the install folder")
        finally:
            la.known_folder = saved_kf
        real = REAL_KNOWN_FOLDER("AppData")
        check(real == "" if sys.platform != "win32" else os.path.isabs(real) and os.path.isdir(real),
              f"12e real known_folder: empty off Windows, an existing absolute folder on Windows ({real!r})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_launch:", "FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
