#!/usr/bin/env python3
"""Host test for tools/release/firstrun.py on synthetic fake steps (no game data, no toolchain: each step runs the
current Python with a one-line script). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_firstrun.py

Cases: PROGRESS/RESULT line format, the --status-file and a monotonic bar ending at 100; the real step table's weights (sum 100) and the
runner's normalisation; Ninja "[x/y]" parsing; resume after a failed step and after a cancel (no marker, earlier steps
skipped); a changed input fingerprint re-runs everything; log next to the setup, earlier log renamed by its start
time, fallback to the temp folder; home-folder redaction; the install's logs/ copy keeps five; --dry-run writes
nothing and lists every step; the RAM -> jobs rule; cleaning stays inside the build folder; --interactive (portable
zip): re-asks a missing folder, finds the EBOOT, runs the real check on synthetic fixtures, writes kz3-install.ini,
shows a console bar instead of raw lines and logs the answers; a failed check shows the update help and offers
check again / quit; the opt-in version check is asked first and logged; the console bar's compile count.
"""
import io
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun as fr  # noqa: E402

FAILS = []
PY = sys.executable


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAIL", what)


def py(code):
    return [PY, "-c", code]


def append(path, text):
    return py(f"open({path!r}, 'a').write({text!r})")


def run(tmp, steps, *extra, log_dir=None):
    out = io.StringIO()
    args = ["--dump", os.path.join(tmp, "dump"), "--root", os.path.join(tmp, "root"), "--jobs", "1",
            "--log-dir", log_dir or os.path.join(tmp, "dl"), *extra]
    rc = fr.main(args, steps=steps, out=out)
    return rc, out.getvalue()


def progress(text):
    return [dict(kv.split("=", 1) for kv in l.split()[1:]) for l in text.splitlines() if l.startswith("PROGRESS ")]


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-firstrun-test-")
    try:
        os.makedirs(os.path.join(tmp, "dump"))
        os.makedirs(os.path.join(tmp, "root"))
        cnt = os.path.join(tmp, "count.txt")
        gate = os.path.join(tmp, "gate")

        # 1. progress format, bar monotonic and complete
        steps = [fr.Step("a", "A", 1, lambda c: [append(cnt, "a")]),
                 fr.Step("b", "B", 2, lambda c: [append(cnt, "b"), append(cnt, "B")]),
                 fr.Step("c", "C", 1, lambda c: [])]
        rc, out = run(tmp, steps)
        p = progress(out)
        check(rc == 0, f"1 rc {rc}")
        check(all(set(x) >= {"step", "name", "done", "total", "elapsed", "pct"} for x in p), "1 progress fields")
        check(all(re.fullmatch(r"\d+/3", x["step"]) for x in p), "1 step=N/M")
        pcts = [float(x["pct"]) for x in p]
        check(pcts == sorted(pcts) and pcts[-1] == 100.0, f"1 pct monotonic to 100: {pcts}")
        check(any(x["name"] == "b" and x["done"] == "1" and x["total"] == "2" for x in p), "1 per-command progress")
        check("RESULT ok=1 step=-" in out, "1 RESULT ok")

        sf = os.path.join(tmp, "status.txt")
        rc, out = run(tmp, steps, "--build", os.path.join(tmp, "b1s"), "--status-file", sf)
        st = open(sf).read().splitlines()
        check(len(st) == 2 and st[0].startswith("LOG path=") and st[1].startswith("RESULT ok=1"),
              f"1 status file holds LOG + last line: {st}")

        # 2. weights
        check(sum(s.weight for s in fr.STEPS) == 100, "2 real STEPS weights sum to 100")
        check(len({s.name for s in fr.STEPS}) == len(fr.STEPS) and fr.STEPS[0].name == "check" and fr.STEPS[0].always,
              "2 check runs first, always; unique names")
        r = fr.Runner(fr.Ctx(root=tmp, build=tmp), steps, None)
        check(abs(sum(r.frac) - 1.0) < 1e-9, "2 normalised weights sum to 1")

        # 3. ninja
        nin = "import sys\nfor i in range(1,5): print(f'[{i}/4] Building CXX obj{i}.o')"
        rc, out = run(tmp, [fr.Step("compile", "Compiling", 1, lambda c: [py(nin)], ninja=True)],
                      "--build", os.path.join(tmp, "b3"))
        nd = [(x["done"], x["total"]) for x in progress(out) if x["total"] == "4"]
        check(nd == [("1", "4"), ("2", "4"), ("3", "4"), ("4", "4")], f"3 ninja parsed: {nd}")
        check(rc == 0 and "[3/4] Building CXX obj3.o" in out, "3 ninja output passed through")

        # 4. resume after a failure
        b4 = os.path.join(tmp, "b4")
        open(cnt, "w").close()
        gated = py(f"import os,sys; sys.exit(0 if os.path.exists({gate!r}) else 3)")
        steps4 = [fr.Step("one", "One", 1, lambda c: [append(cnt, "1")]),
                  fr.Step("two", "Two", 1, lambda c: [gated, append(cnt, "2")]),
                  fr.Step("three", "Three", 1, lambda c: [append(cnt, "3")])]
        rc, out = run(tmp, steps4, "--build", b4)
        check(rc == 1 and "RESULT ok=0 step=two" in out, f"4 failure reported (rc {rc})")
        check(os.path.exists(os.path.join(b4, ".firstrun", "01-one.done")), "4 marker for the finished step")
        check(not os.path.exists(os.path.join(b4, ".firstrun", "02-two.done")), "4 no marker for the failed step")
        open(gate, "w").close()
        rc, out = run(tmp, steps4, "--build", b4)
        check(rc == 0 and open(cnt).read() == "123", f"4 resumed at step two: rc {rc}, ran {open(cnt).read()!r}")

        # 5. cancel (Ctrl+C inside a step) leaves no marker; the next run resumes there
        b5 = os.path.join(tmp, "b5")
        open(cnt, "w").close()
        state = {"cancel": True}

        def maybe_cancel(c):
            if state["cancel"]:
                raise KeyboardInterrupt()
        steps5 = [fr.Step("one", "One", 1, lambda c: [append(cnt, "1")]),
                  fr.Step("two", "Two", 1, lambda c: [append(cnt, "2")], after=maybe_cancel),
                  fr.Step("three", "Three", 1, lambda c: [append(cnt, "3")])]
        rc, out = run(tmp, steps5, "--build", b5)
        check(rc == 130 and "RESULT ok=0 step=cancelled" in out, f"5 cancel rc {rc}")
        check(not os.path.exists(os.path.join(b5, ".firstrun", "02-two.done")), "5 no marker for the cancelled step")
        state["cancel"] = False
        rc, out = run(tmp, steps5, "--build", b5)
        check(rc == 0 and open(cnt).read() == "1223", f"5 resumed: ran {open(cnt).read()!r}")

        # 5b. --stop-file (the wizard's Stop button): the running child ends at once, no marker, rc 130, resume
        b5b, stopf = os.path.join(tmp, "b5b"), os.path.join(tmp, "stop.flag")
        open(cnt, "w").close()
        slow = py(f"import time; open({stopf!r}, 'w').close(); time.sleep(60)")
        state5b = {"stop": True}
        steps5b = [fr.Step("one", "One", 1, lambda c: [append(cnt, "1")]),
                   fr.Step("two", "Two", 1, lambda c: [slow if state5b["stop"] else append(cnt, "2")]),
                   fr.Step("three", "Three", 1, lambda c: [append(cnt, "3")])]
        t5 = time.monotonic()
        rc, out = run(tmp, steps5b, "--build", b5b, "--stop-file", stopf)
        check(rc == 130 and "RESULT ok=0 step=cancelled" in out and time.monotonic() - t5 < 20,
              f"5b stop file ends the step: rc {rc} after {time.monotonic() - t5:.1f} s")
        check(not os.path.exists(os.path.join(b5b, ".firstrun", "02-two.done")), "5b no marker for the stopped step")
        os.remove(stopf)
        state5b["stop"] = False
        rc, out = run(tmp, steps5b, "--build", b5b, "--stop-file", stopf)
        check(rc == 0 and open(cnt).read() == "123", f"5b resumed: ran {open(cnt).read()!r}")

        # 6. a changed input (another update folder) makes every marker stale
        open(cnt, "w").close()
        os.makedirs(os.path.join(tmp, "upd"), exist_ok=True)
        rc, out = run(tmp, steps5, "--build", b5, "--update", os.path.join(tmp, "upd"))
        check(rc == 0 and open(cnt).read() == "123", f"6 new inputs re-run all: {open(cnt).read()!r}")

        # 7. log location, rotation by start time, fallback
        dl = os.path.join(tmp, "dl")
        log = os.path.join(dl, fr.DEFAULT_LOG_NAME)
        check(os.path.isfile(log) and f"LOG path={log}" in out, "7 log next to the setup")
        first = open(log).readline()
        check(first.startswith("# started "), "7 log starts with its start time")
        stamp = first.split()[2].replace("-", "").replace(":", "").replace("T", "-")
        run(tmp, steps5, "--build", b5)
        rotated = [n for n in os.listdir(dl) if n.startswith("Project-Helghast-setup-")]
        check(any(n.startswith(f"Project-Helghast-setup-{stamp}") for n in rotated), f"7 earlier log renamed: {rotated}")
        body = open(log).read()
        check("$ " in body and "# exit 0" in body and "## step 1/3 one" in body, "7 log has commands, exits, steps")
        notdir = os.path.join(tmp, "a-file")
        open(notdir, "w").close()
        tdir = os.path.join(tmp, "tmpdir")
        os.makedirs(tdir)
        old_tmp = tempfile.tempdir
        tempfile.tempdir = tdir
        try:
            rc, out = run(tmp, steps5, "--build", b5, log_dir=notdir)
        finally:
            tempfile.tempdir = old_tmp
        check(os.path.isfile(os.path.join(tdir, fr.DEFAULT_LOG_NAME)), "7 fallback to the temp folder")
        check("log folder not writable" in open(os.path.join(tdir, fr.DEFAULT_LOG_NAME)).read(), "7 fallback noted")

        # 8. redaction of the home folder
        home = os.path.join(tmp, "Users", "Jane Doe")
        old_env = dict(os.environ)
        os.environ["USERPROFILE"] = home
        os.environ["HOME"] = home
        try:
            red = fr.redactor()
            check(red(f"{home}/Downloads/x") == "%USERPROFILE%/Downloads/x", "8 redacts the home path")
            check(red(home.replace("/", "\\") + "\\x") == "%USERPROFILE%\\x", "8 redacts backslash form")
            check(red("[1/9] Building C object C_/Users/Jane_Doe/AppData/x.c.obj") ==
                  "[1/9] Building C object C_/Users/<user>/AppData/x.c.obj", "8 redacts CMake's mangled user path")
            check(red(r"# dump C:\Users\Other Name\Downloads\b") == r"# dump C:\Users\<user>\Downloads\b",
                  "8 redacts another account's folder")
            check(red("D:/Games/kz3") == "D:/Games/kz3", "8 leaves non-user paths alone")
            # the path goes in argv, not in a repr()'d program text: repr doubles Windows backslashes in the logged command line
            say = py("import sys; print(sys.argv[1])") + [os.path.join(home, 'rpcs3', 'dev_hdd0')]
            rc, out = run(tmp, [fr.Step("say", "Say", 1, lambda c: [say])], "--build", os.path.join(tmp, "b8"))
            body = open(log).read()
            check("Jane Doe" not in body and "%USERPROFILE%" in body, "8 log redacted")
        finally:
            os.environ.clear()
            os.environ.update(old_env)

        # 9. install logs/ copy keeps five
        logs = os.path.join(tmp, "root", "logs")
        for i in range(7):
            open(os.path.join(logs, f"firstrun-20200101-00000{i}.log"), "w").close()
        fr.copy_to_install_logs(log, logs)
        kept = sorted(os.listdir(logs))
        check(len(kept) == fr.KEEP_LOG_COPIES and "firstrun-20200101-000000.log" not in kept, f"9 kept: {kept}")

        # 10. --dry-run on the real table writes nothing and lists all steps with commands
        b10 = os.path.join(tmp, "b10")
        dl10 = os.path.join(tmp, "dl10")
        os.makedirs(dl10)
        out = io.StringIO()
        rc = fr.main(["--dump", os.path.join(tmp, "dump"), "--update", os.path.join(tmp, "upd"),
                      "--eboot", os.path.join(tmp, "upd", "USRDIR", "EBOOT.elf"), "--root", os.path.join(tmp, "root"),
                      "--build", b10, "--jobs", "2", "--log-dir", dl10, "--dry-run",
                      "--low-memory", "off"], out=out)   # auto turns on at <= 8 GiB (CI runners): one job, no "-j 2"
        o = out.getvalue()
        check(rc == 0 and not os.path.exists(b10) and os.listdir(dl10) == [], "10 dry-run writes nothing")
        check(all(f" {s.name} " in o for s in fr.STEPS) and "firstrun_check.py" in o and "ppu_lifter.py" in o
              and "--build" in o and "-j 2" in o, "10 plan lists steps and commands")
        check(sum(float(m) for m in re.findall(r"(\d+\.\d)%", o)) > 99.9, "10 plan weights add to 100%")
        nolog = os.path.join(tmp, "nolog", "sub")
        fr.main(["--dump", os.path.join(tmp, "dump"), "--build", b10, "--log-dir", nolog, "--dry-run"], out=io.StringIO())
        check(not os.path.exists(os.path.join(tmp, "nolog")) and not os.path.exists(b10),
              "10 dry-run creates no log folder")

        # 11. RAM rule, clean boundary
        G = fr.GIB
        check([fr.jobs_for(x) for x in (None, 4 * G, 9 * G, 11.5 * G, 64 * G)] == [1, 1, 1, 2, 8], "11 jobs rule (64 chunks)")
        check([fr.jobs_for(x, clang=True, cores=12) for x in (None, 2 * G, 4 * G, 8.5 * G, 64 * G)] == [1, 1, 1, 5, 12]
              and fr.jobs_for(64 * G, clang=True, cores=32) == 16, "11 clang jobs rule (pass 12 measurements)")
        cj = fr.Ctx(root=tmp, build=os.path.join(tmp, "bj"), jobs=12)
        check(" -j 4 " in " ".join(fr.cmd_ppu_lift(cj)[-1]) + " " and "-j 4" in " ".join(fr.cmd_spu_lift(cj)[0])
              and fr.cmd_compile(cj)[0][-1] == "12", "11 lifts capped at 4 workers, compile uses all jobs")
        # 11a. low-memory mode (step 9u): auto at <= 8 GiB total RAM, one lift worker, a log line
        check([fr.low_memory_for("auto", x)[0] for x in (None, 4 * G, 7.9 * G, 8 * G, 8.1 * G, 32 * G)]
              == [False, True, True, True, False, False]
              and fr.low_memory_for("on", 64 * G)[0] and not fr.low_memory_for("off", 4 * G)[0],
              "11a low-memory rule: auto <= 8 GiB, on/off override")
        cj.low_memory = True
        check(" -j 1 " in " ".join(fr.cmd_ppu_lift(cj)[-1]) + " " and "-j 1" in " ".join(fr.cmd_spu_lift(cj)[0]),
              "11a low-memory lifts use one worker")
        cj.low_memory = False
        lo = io.StringIO()
        fr.main(["--dump", os.path.join(tmp, "dump"), "--root", os.path.join(tmp, "root"), "--low-memory", "on",
                 "--build", os.path.join(tmp, "blm"), "--log-dir", os.path.join(tmp, "dlm"), "--dry-run"], out=lo)
        check("# low-memory build: ON" in lo.getvalue() or "-j 1" in lo.getvalue(), f"11a dry run shows it: {lo.getvalue()[:300]!r}")
        # 11b. EBOOT version of the tree (pass 12): kz3ver.py's DEFAULT picks the lift constants and the env
        vr = os.path.join(tmp, "verroot")
        c = fr.Ctx(root=vr, build=os.path.join(vr, "b"))
        check(c.ver == "114", "11b no kz3ver.py: 114")
        os.makedirs(os.path.join(vr, "kz3-setup", "spu"))
        with open(os.path.join(vr, "kz3-setup", "spu", "kz3ver.py"), "w") as f:
            f.write('DEFAULT = "100"\n')
        c = fr.Ctx(root=vr, build=os.path.join(vr, "b"))
        lift = " ".join(fr.cmd_ppu_lift(c)[-1])
        check(c.ver == "100" and "--toc 0x132c960 --code-end 0xdfbbd4" in lift and c.env()["KZ3_EBOOT_VER"] == "100",
              "11b kz3ver DEFAULT 100: 1.00 constants and env")
        r = fr.Runner(c, [], None, out=io.StringIO())
        c.facts = {"eboot_elf": {"version": "1.14"}}
        check("built for the 1.00 EBOOT" in r.version_mismatch(), "11b 1.14 EBOOT on a 1.00 tree refused")
        c.facts = {"eboot_elf": {"version": "1.00"}}
        check(r.version_mismatch() == "", "11b matching EBOOT accepted")
        c.facts = {"eboot_elf": {"version": None}}
        check(r.version_mismatch() == "", "11b unknown size: the check's warning only")
        b11 = os.path.join(tmp, "b11")
        os.makedirs(os.path.join(b11, "obj"))
        outside = os.path.join(tmp, "outside")
        os.makedirs(outside)
        c = fr.Ctx(root=tmp, build=b11)
        fr.safe_clean(c, outside)
        fr.safe_clean(c, b11)
        fr.safe_clean(c, os.path.join(b11, "obj"))
        check(os.path.isdir(outside) and os.path.isdir(b11) and not os.path.exists(os.path.join(b11, "obj")),
              "11 clean only inside the build folder")
        # 11c. install over an earlier version (pass 18): changed payload files touched, toolchain change cleans obj
        r11 = os.path.join(tmp, "r11c")
        b11c = os.path.join(r11, "build")
        os.makedirs(os.path.join(r11, "port", "kz3"))
        os.makedirs(os.path.join(b11c, "obj"))
        for n in ("a.c", "b.c"):
            with open(os.path.join(r11, "port", "kz3", n), "w") as f:
                f.write(n)
            os.utime(os.path.join(r11, "port", "kz3", n), (1000, 1000))

        def man(entries):
            with open(os.path.join(r11, "manifest.json"), "w") as f:
                json.dump({"files": [{"path": p, "sha256": h} for p, h in entries]}, f)
        man([("port/kz3/a.c", "1"), ("port/kz3/b.c", "1"), ("toolchain/clang.exe", "1")])
        c = fr.Ctx(root=r11, build=b11c)
        check(fr.prepare_sources(c) == [] and os.path.getmtime(os.path.join(r11, "port", "kz3", "a.c")) == 1000,
              "11c no record (first build): nothing touched")
        fr.record_sources(c)
        man([("port/kz3/a.c", "2"), ("port/kz3/b.c", "1"), ("toolchain/clang.exe", "1")])
        cl = fr.prepare_sources(c)
        check(cl == [] and c.touched == ["port/kz3/a.c"]
              and os.path.getmtime(os.path.join(r11, "port", "kz3", "a.c")) > 1000
              and os.path.getmtime(os.path.join(r11, "port", "kz3", "b.c")) == 1000, "11c the changed source touched")
        man([("port/kz3/a.c", "1"), ("port/kz3/b.c", "1"), ("toolchain/clang.exe", "2")])
        check(fr.prepare_sources(c) == [c.b("obj")], "11c a changed toolchain cleans obj")
        check(fr.Ctx(root=tmp, build=os.path.join(tmp, "bx")) and fr.payload_hashes(tmp) == {}, "11c no manifest: {}")
        # 12. --interactive (portable zip's text UI), with the real firstrun_check on synthetic fixtures
        import types
        import firstrun_check as fc
        import test_firstrun_check as tfc
        fc.MIN_PSARC_BYTES = 1024             # the fixtures' psarc is a few KB
        t12 = os.path.join(tmp, "i12")
        d12, u12 = tfc.make_dump(t12), tfc.make_update(t12)
        root12, dl12 = os.path.join(t12, "root"), os.path.join(t12, "dl")
        os.makedirs(root12)
        os.makedirs(dl12)
        answers = iter([os.path.join(t12, "nope"), d12, u12, "", "", "n", "n", "", ""])   # font: skip; cleanup: keep
        asked = []

        def ask(q):
            asked.append(q)
            return next(answers)
        o12 = io.StringIO()
        rc = fr.main(["--interactive", "--rpcs3", "off", "--portable", "--root", root12, "--log-dir", dl12, "--jobs", "1",
                      "--log-name", "kz3-firstrun.log"], steps=steps, out=o12, ask=ask)
        o = o12.getvalue()
        check(rc == 0 and "not found" in o and "OK   E1" in o and "Done in" in o and "[" + "#" * 30 + "]" in o
              and "PROGRESS " not in o, f"12 interactive build: rc {rc} {o[-400:]!r}")
        check(any("Decrypted EBOOT" in q and "EBOOT.elf" in q for q in asked), "12 EBOOT default offered")
        ini = open(os.path.join(root12, "kz3-install.ini")).read()
        check("portable = 1" in ini and "check_updates = 0" in ini and "prepare = 0" in ini, f"12 kz3-install.ini: {ini!r}")
        lg = open(os.path.join(dl12, "kz3-firstrun.log")).read()
        check(lg.startswith("# started") and "# interactive check" in lg and "E1:" in lg and "RESULT ok=1" in lg,
              "12 one log with header, the check and the build")

        a12 = types.SimpleNamespace(dump="", update="", eboot="", build=os.path.join(t12, "b"), portable=False)
        res = iter([(1, "FAIL U0: update folder not found\n"), (0, "OK   D1: fine\n")])
        answers = iter([d12, u12, "", "", "", "y", "y", "", "n"])
        notes, o12 = [], io.StringIO()
        go = fr.interactive(a12, root12, "x.log", notes, ask=lambda q: next(answers), out=o12,
                            check=lambda *x: next(res),
                            vcheck=lambda cur: {"status": "newer", "current": cur, "latest": "v9.0.0",
                                                "url": "https://example.invalid/r", "detail": ""})
        o = o12.getvalue()
        check(go is False and "own console" in o and "v9.0.0" in o and a12.update == u12, "12 failed check, help, retry")
        check(any("version check (player asked): status=newer" in n for n in notes), f"12 version check logged {notes}")
        ini = open(os.path.join(root12, "kz3-install.ini")).read()
        check("portable = 0" in ini and "check_updates = 1" in ini and "prepare = 0" in ini,
              f"12 ini after opt-in (Enter at the shader question = warm while playing): {ini!r}")
        answers = iter([d12, u12, "", "q"])
        go = fr.interactive(a12, root12, "x.log", [], ask=lambda q: next(answers), out=io.StringIO(),
                            check=lambda *x: (1, "FAIL D1: x\n"))
        check(go is False, "12 quit after a failed check")

        bar = io.StringIO()
        cb = fr.ConsoleBar(bar, tty=False)
        for ln in ("LOG path=/x/y.log", "some tool output",
                   "PROGRESS step=7/8 name=compile done=312 total=1204 elapsed=754 pct=60.3",
                   "RESULT ok=0 step=compile elapsed=800"):
            print(ln, file=cb)
        b = bar.getvalue()
        check("Compiling 312 / 1,204" in b and "12:34" in b and "some tool output" not in b and "issues" in b
              and "Log: /x/y.log" in b, f"12 console bar: {b!r}")

        # 13. the loader gets the EBOOT under the fixed name whatever the player's file is called (pass 53)
        for nm in ("EBOOT-114.elf", "EBOOT (1).elf", "EBOOT.elf"):
            src = os.path.join(tmp, "src13")
            os.makedirs(src, exist_ok=True)
            ef = os.path.join(src, nm)
            with open(ef, "wb") as f:
                f.write(b"\x7fELFstub")
            c13 = fr.Ctx(root=tmp, build=os.path.join(tmp, "b13"), eboot=ef)
            cmds = fr.cmd_load(c13)
            loader_in = cmds[0][cmds[0].index("-o") - 1]
            check(os.path.basename(loader_in) == "EBOOT.elf" and os.path.dirname(loader_in) == c13.b("analysis")
                  and open(loader_in, "rb").read() == b"\x7fELFstub", f"13 staged name for {nm}: {loader_in!r}")
            check(c13.eboot == ef and os.path.exists(ef), f"13 player's file untouched for {nm}")

        # 14. a decrypted ISO as the dump: the check step copies PS3_DISC.SFB and PS3_GAME into <root>/game/disc,
        # the later steps see that folder, the bar stays monotonic, a second run copies nothing, failures are plain
        from iso_fixture import build_iso
        t14 = os.path.join(tmp, "t14")
        os.makedirs(os.path.join(t14, "root"))
        iso14 = os.path.join(t14, "game.iso")
        files14 = {"PS3_DISC.SFB": b".SFB" + b"\0" * 8, "PS3_GAME/PARAM.SFO": b"\0PSF" + b"s" * 30,
                   "PS3_GAME/ICON0.PNG": b"png", "PS3_GAME/TROPDIR/NPWR01569_00/TROPHY.TRP": b"trp",
                   "PS3_GAME/USRDIR/EBOOT.BIN": b"SCE\0" + b"e" * 20,
                   "PS3_GAME/USRDIR/data_ps3.psarc": b"PSAR" + bytes(range(256)) * 80}
        build_iso(iso14, files14, multi={"PS3_GAME/USRDIR/data_ps3.psarc": 4096})
        seen14 = os.path.join(t14, "seen.txt")
        steps14 = [fr.Step("check", "Check", 1, lambda c: [], always=True, after=fr.after_check),
                   fr.Step("b", "B", 2, lambda c: [append(seen14, c.dump + "\n")])]
        out14 = io.StringIO()
        args14 = ["--dump", iso14, "--root", os.path.join(t14, "root"), "--jobs", "1", "--log-dir", os.path.join(t14, "dl")]
        rc = fr.main(args14, steps=steps14, out=out14)
        dest14 = os.path.join(t14, "root", "game", "disc")
        check(rc == 0, f"14 rc {rc}: {out14.getvalue()[-300:]}")
        got14 = {os.path.relpath(os.path.join(d, n), dest14).replace(os.sep, "/"): open(os.path.join(d, n), "rb").read()
                 for d, _, ns in os.walk(dest14) for n in ns}
        want14 = {k.replace("DATA_PS3.PSARC", "data_ps3.psarc"): v for k, v in files14.items()}
        check(got14 == want14, f"14 extracted tree equals the ISO's files: {sorted(got14)}")
        check(open(seen14).read().strip() == dest14, "14 later steps see the extracted folder as the dump")
        pj = json.load(open(os.path.join(t14, "root", "build", "paths.json")))
        check(pj["dump"] == dest14 and pj["iso"] == iso14, "14 paths.json records the folder and the ISO")
        pr14 = progress(out14.getvalue())
        pc14 = [float(x["pct"]) for x in pr14]
        check(pc14 == sorted(pc14) and pc14[-1] == 100.0, f"14 pct monotonic to 100: {pc14}")
        check(any(x["name"] == "check" and x["total"] == "1000" and x["done"] == "1000" for x in pr14), "14 copy progress ends at 1000/1000")
        check(fr.steps_for(tmp, "off", iso=True)[0].weight > fr.STEPS[0].weight, "14 the check step weighs more for an ISO")
        out14 = io.StringIO()
        rc = fr.main(args14, steps=steps14, out=out14)
        check(rc == 0 and "0.00 GiB to copy" in out14.getvalue() and not any(
            n.endswith(".part") for _, _, ns in os.walk(dest14) for n in ns), "14 a second run copies nothing")
        # a file cut short is copied again; the others are kept
        victim = os.path.join(dest14, "PS3_GAME", "USRDIR", "data_ps3.psarc")
        with open(victim, "r+b") as f:
            f.truncate(100)
        c14 = fr.Ctx(root=os.path.join(t14, "root"), build=os.path.join(t14, "b"), iso=iso14, dump=iso14)
        said = []
        fr.iso_extract(c14, say=said.append)
        check(open(victim, "rb").read() == files14["PS3_GAME/USRDIR/data_ps3.psarc"] and c14.dump == dest14, "14 a short file is copied again")
        # not enough room
        import collections
        shutil.rmtree(dest14)
        c14 = fr.Ctx(root=os.path.join(t14, "root"), build=os.path.join(t14, "b"), iso=iso14, dump=iso14)
        try:
            fr.iso_extract(c14, say=said.append, free=lambda p: collections.namedtuple("U", "free")(1000))
            check(False, "14 low disk space refused")
        except fr.StepFailed as e:
            check("Not enough free disk space" in str(e) and "GiB" in str(e) and not os.path.exists(os.path.join(dest14, "PS3_GAME")),
                  f"14 low disk space refused before writing: {e}")
        # encrypted-looking and non-ISO images
        enc = bytearray(2048 * 40)
        enc[0:4] = b"\0\0\0\2"
        enc[8:24] = b"\0\0\0\0\0\0\0\x0f\0\0\0\x10\0\0\0\x1e"
        bad14 = os.path.join(t14, "enc.iso")
        open(bad14, "wb").write(bytes(enc))
        c14 = fr.Ctx(root=os.path.join(t14, "root"), build=os.path.join(t14, "b"), iso=bad14, dump=bad14)
        try:
            fr.iso_extract(c14, say=said.append)
            check(False, "14 encrypted ISO refused")
        except fr.StepFailed as e:
            check(str(e) == "this ISO is encrypted; use a decrypted dump", f"14 encrypted ISO message: {e}")
        open(bad14, "wb").write(b"not an iso" * 5000)
        try:
            fr.iso_extract(c14, say=said.append)
            check(False, "14 non-ISO refused")
        except fr.StepFailed as e:
            check("cannot read the ISO" in str(e), f"14 non-ISO message: {e}")
        # the interactive prompt takes a .iso file as the dump (and still re-asks a missing path)
        picked = []
        answers = iter([os.path.join(t14, "nope.iso"), iso14, u12, "", "q"])
        a14 = types.SimpleNamespace(dump="", update="", eboot="", build=os.path.join(t14, "b"), portable=False)
        go = fr.interactive(a14, root12, "x.log", [], ask=lambda q: next(answers), out=io.StringIO(),
                            check=lambda d, u, e: (picked.append(d), (1, "FAIL D1: x\n"))[1])
        check(go is False and picked == [iso14], f"14 interactive accepts a decrypted .iso as the dump: {picked}")
        # 15 future-dated port files (an installer that kept the build machine's UTC times as local ones): found and
        # refused with the plain message before configure/compile; files at the right time pass
        t15 = os.path.join(tmp, "t15")
        os.makedirs(os.path.join(t15, "port", "kz3"))
        f15 = os.path.join(t15, "port", "kz3", "CMakeLists.txt")
        open(f15, "w").close()
        c15 = types.SimpleNamespace(root=t15, eboot="")
        check(fr.future_files(c15) == (0, None), "15 a file at the right time is not future-dated")
        later = time.time() + 3 * 3600
        os.utime(f15, (later, later))
        n15, w15 = fr.future_files(c15)
        check(n15 == 1 and w15 == f15, f"15 a file 3 h in the future is found: {n15} {w15}")
        try:
            fr.refuse_future_files(c15)
            check(False, "15 refuse_future_files raises")
        except fr.StepFailed as e:
            check("dated in the future" in str(e) and "time zone" in str(e), f"15 plain message: {e}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_firstrun:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
