#!/usr/bin/env python3
"""Host test for tools/release/shaders.py (Prepare shaders) and its hooks in firstrun.py and
launch.py, on a synthetic install tree in a temp folder: no game data, no game run, no Windows, no network.
Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_shaders.py

Cases: the two pad routes (menu-fast.pad's presses, event limit, order, planned time); where the cache goes (the
install folder portable, %APPDATA%/Project Helghast otherwise, never the dump; the warm-up's own working folder; the
same RSX_SHADER_CACHE_DIR for normal starts); warm/cold decisions (no marker, exe changed, version changed, key file
gone) and --force; a fake timed session (progress lines firstrun parses, early end on the library save, the full
wait without one, a game that ends early, no keys recorded) and that only a good session writes the marker; the
warm-up profile (continue record cleared, a broken one removed -> first-boot route); firstrun's optional step
(--shaders on/off/auto from kz3-install.ini, a failed warm-up keeps RESULT ok=1); launch.py --prepare-shaders.
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402
import shaders as sh  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(HERE), "assets"))
import profile_edit as pe  # noqa: E402

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def write(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def make_install(tmp, portable=False):
    root, dump = os.path.join(tmp, "root"), os.path.join(tmp, "dump")
    eboot = os.path.join(tmp, "upd", "USRDIR", "EBOOT.elf")
    write(eboot)
    write(os.path.join(dump, "PS3_GAME", "PARAM.SFO"))
    write(os.path.join(root, "build", "kz3.exe"), b"MZ exe 1")
    write(os.path.join(root, "build", "paths.json"),
          json.dumps({"dump": dump, "update": os.path.join(tmp, "upd"), "eboot": eboot, "facts": {}}))
    write(os.path.join(root, "kz3-install.ini"), f"[install]\nportable = {int(portable)}\n")
    return root, dump


class FakeProc:
    def __init__(self, exit_at=None, clock=None):
        self.pid, self.returncode, self.exit_at, self.clock = 4242, None, exit_at, clock
        self.terminated = self.killed = False

    def poll(self):
        if self.returncode is None and self.exit_at is not None and self.clock() >= self.exit_at:
            self.returncode = 3
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        return self.returncode


class FakeGame:
    """Clock + start for Session: the game 'records keys' at t=10 and 'saves the library' at lib_at (wall time)."""

    def __init__(self, shaders, lib_at=None, exit_at=None, keys=True):
        self.t, self.shaders, self.lib_at, self.exit_at, self.keys = 0.0, shaders, lib_at, exit_at, keys
        self.started = []

    def clock(self):
        return self.t

    def wall(self):
        return 1_000_000.0 + self.t

    def sleep(self, s):
        self.t += s
        if self.keys and self.t >= 10 and not os.path.exists(os.path.join(self.shaders, sh.KEYS)):
            write(os.path.join(self.shaders, sh.KEYS), b"P" * 300)
        if self.lib_at is not None and self.t >= self.lib_at:
            p = os.path.join(self.shaders, sh.LIBRARY)
            write(p, b"L" * 10)
            os.utime(p, (self.wall(), self.wall()))
            self.lib_at = None

    def start(self, argv, cwd, env, out, window):
        self.started.append((argv, cwd, env, window))
        self.proc = FakeProc(self.exit_at, self.clock)
        return self.proc

    def session(self, out):
        return sh.Session(out=out, clock=self.clock, wall=self.wall, sleep=self.sleep, start=self.start)


def main():
    la.known_folder = lambda name: ""   # the env dicts below stand for APPDATA; on Windows the real known folder would win
    # 1. routes
    for route in ("profile", "first-boot"):
        text, planned = sh.route_script(route)
        ev = [ln.split(":", 1) for ln in text.split()]
        times = [float(t) for t, _ in ev]
        check(0 < len(ev) <= 1024 and times == sorted(times) and max(times) < planned,
              f"1 {route}: {len(ev)} events sorted, last {max(times)} < planned {planned}")
        check(planned - sh.IN_LEVEL in times and any("/" in b for _, b in ev), f"1 {route}: in-level walk from the start")
    text, planned = sh.route_script("first-boot")
    check("25:0x0008" in text.split() and planned >= sh.FIRST_LEVEL_START, "1 first-boot: START at 25 s, long route")
    mf = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "pad", "menu-fast.pad")
    if os.path.isfile(mf):
        with open(mf) as f:
            pad = [ln.split("#")[0].strip() for ln in f]
        pad = [(int(p.split(":")[0]), int(p.split(":")[1], 0)) for p in pad if p]
        check(pad == sh.MENU_FAST, "1 profile route = tools/pad/menu-fast.pad's presses")
    else:
        print("SKIP 1 menu-fast.pad not in this tree")

    o = io.StringIO()
    sh.main(["--route-script", "profile"], out=o)
    check(o.getvalue().startswith("# profile route") and "42:0x4000" in o.getvalue().split(), "1 --route-script")

    with tempfile.TemporaryDirectory() as tmp:
        # 2. where things go
        root, dump = make_install(os.path.join(tmp, "a"))
        appdata = os.path.join(tmp, "AppData")
        env0 = {"APPDATA": appdata, "PATH": "/bin"}
        argv, cwd, env, added, w = sh.warm_env(root, env0)
        data = os.path.join(appdata, "Project Helghast")
        check(added["RSX_SHADER_CACHE_DIR"] == os.path.join(data, "cache", "shaders") == w["shaders"],
              f"2 shader cache under %APPDATA% ({added['RSX_SHADER_CACHE_DIR']})")
        check(cwd == os.path.join(data, "cache", "shader-warmup") and not cwd.startswith(dump)
              and not w["shaders"].startswith(dump), "2 warm-up cwd in the cache folder, nothing under the dump")
        check(env["PAD_NO_KEYBOARD"] == "1" and env["KZ3_ERROR_NOTICE_TIMEOUT"] == "15" and env["PATH"] == "/bin"
              and env["PS3_VFS_ROOT"] == os.path.join(root, "vfs"), "2 warm-up env = the game env + unattended switches")
        _, gcwd, _, gadded = la.game_env(root, la.load_json(os.path.join(root, "build", "paths.json")), False, env0)
        check(gcwd == data and gadded["RSX_SHADER_CACHE_DIR"] == w["shaders"], "2 normal starts use the same cache")
        rootp, _ = make_install(os.path.join(tmp, "p"), portable=True)
        _, cwdp, _, addedp, _ = sh.warm_env(rootp, env0)
        check(addedp["RSX_SHADER_CACHE_DIR"] == os.path.join(rootp, "cache", "shaders")
              and cwdp == os.path.join(rootp, "cache", "shader-warmup"), "2 portable: in the install folder")
        check(sh.warm_env(os.path.join(tmp, "none"), env0) is None, "2 not built -> None")

        # 3. warm/cold
        ident = sh.exe_id(w["exe"])
        check(sh.warm_state(w["shaders"], ident, "1.0.0") == (False, "no warm-up recorded"), "3 cold without marker")
        write(os.path.join(w["shaders"], sh.KEYS), b"P" * 300)
        write(os.path.join(w["shaders"], sh.MARKER), json.dumps({"exe": ident, "version": "1.0.0", "finished": "t"}))
        check(sh.warm_state(w["shaders"], ident, "1.0.0")[0], "3 warm with marker, same exe and version")
        check(not sh.warm_state(w["shaders"], {"size": 1, "sha256": "0"}, "1.0.0")[0], "3 exe changed -> cold")
        check(not sh.warm_state(w["shaders"], ident, "1.1.0")[0], "3 version changed -> cold")
        os.remove(os.path.join(w["shaders"], sh.KEYS))
        check("missing" in sh.warm_state(w["shaders"], ident, "1.0.0")[1], "3 key file gone -> cold")
        os.remove(os.path.join(w["shaders"], sh.MARKER))

        # 4. the timed session
        g = FakeGame(w["shaders"], lib_at=None)
        o = io.StringIO()
        ok, why, facts = g.session(o).run(argv, cwd, env, w["shaders"], 30, os.path.join(tmp, "g.log"))
        lines = o.getvalue().splitlines()
        bars = [ln for ln in lines if fr.NINJA_RE.match(ln)]
        check(ok and "no library save" in why and g.t >= 30 + sh.SAVE_WAIT and bars[-1].startswith(f"[{30 + sh.SAVE_WAIT}/"),
              f"4 no library save: waits {sh.SAVE_WAIT} s past the route, ok ({why}, t={g.t})")
        check(len(bars) > 10 and all(int(fr.NINJA_RE.match(b).group(2)) == 30 + sh.SAVE_WAIT for b in bars),
              "4 progress lines in firstrun's [x/y] form over the planned total")
        # close_politely: SIGTERM (terminate) off Windows, WM_CLOSE posts only on Windows (no terminate there)
        check(g.proc.terminated == (os.name != "nt") and not g.proc.killed and g.started[0][3] == "minimized",
              "4 ends the game (polite close first: terminate off Windows, WM_CLOSE on Windows), minimized")
        os.remove(os.path.join(w["shaders"], sh.KEYS))
        g = FakeGame(w["shaders"], lib_at=50)
        ok, why, facts = g.session(io.StringIO()).run(argv, cwd, env, w["shaders"], 30, os.path.join(tmp, "g.log"))
        check(ok and why == "library saved" and g.t < 30 + sh.SAVE_WAIT and facts["library_saved_after_route"],
              f"4 ends early once the library is saved after the route (t={g.t})")
        g = FakeGame(w["shaders"], exit_at=20)
        ok, why, _ = g.session(io.StringIO()).run(argv, cwd, env, w["shaders"], 30, os.path.join(tmp, "g.log"))
        check(not ok and "ended early" in why and g.t < 30, f"4 game ends early -> failed ({why})")
        os.remove(os.path.join(w["shaders"], sh.KEYS))
        g = FakeGame(w["shaders"], keys=False)
        ok, why, _ = g.session(io.StringIO()).run(argv, cwd, env, w["shaders"], 30, os.path.join(tmp, "g.log"))
        check(not ok and "no shader keys" in why, "4 no keys recorded -> failed")

        # 5. main: run, skip, force, invalidation, failure, --print
        def run_main(*extra, game=None):
            game = game or FakeGame(w["shaders"], lib_at=sh.FIRST_LEVEL_START + sh.IN_LEVEL + 5)
            o = io.StringIO()
            rc = sh.main(["--root", root, *extra], out=o, session=game.session(o), environ=env0)
            return rc, o.getvalue(), game
        rc, o, game = run_main()
        m = la.load_json(os.path.join(w["shaders"], sh.MARKER)) or {}
        check(rc == 0 and game.started and m.get("route") == "first-boot" and m.get("version") == "0.0.0-dev"
              and m.get("exe") == ident, f"5 cold: runs the first-boot route, marker written ({rc})")
        st = game.started[0]
        check("PAD_SCRIPT" in st[2] and "25:0x0008" in st[2]["PAD_SCRIPT"] and st[1] == cwd and os.path.isdir(cwd)
              and os.path.isdir(w["shaders"]), "5 PAD_SCRIPT set, folders created")
        rc, o, game = run_main()
        check(rc == 0 and not game.started and "skipped" in o, "5 warm: skipped, the game is not started")
        rc, o, game = run_main("--force")
        check(rc == 0 and game.started, "5 --force runs anyway")
        write(w["exe"], b"MZ exe 2")
        rc, o, game = run_main()
        check(rc == 0 and game.started and "kz3.exe changed" in o, "5 a new kz3.exe runs it again")
        write(os.path.join(root, "VERSION.txt"), "1.2.0")
        rc, o, game = run_main(game=FakeGame(w["shaders"], exit_at=5))
        check(rc == 2 and game.started and not os.path.exists(os.path.join(w["shaders"], sh.MARKER)),
              "5 new version: runs; a failed session leaves no marker (next start offers it again)")
        o = io.StringIO()
        sh.main(["--root", root, "--print"], out=o, environ=env0)
        p = json.loads(o.getvalue())
        check(p["run"] and not p["warm"] and p["env"]["RSX_SHADER_CACHE_DIR"] == w["shaders"]
              and p["total_seconds"] == p["planned_seconds"] + sh.SAVE_WAIT, "5 --print JSON")
        rc = sh.main(["--root", os.path.join(tmp, "none")], out=io.StringIO(), environ=env0)
        check(rc == 1, "5 not built -> exit 1")

        # 6. the warm-up profile
        prof = os.path.join(cwd, "gamedata", "dev_hdd0", "home", "00000001", "savedata", "KZ3PROFILE", "PROFILE.DAT")
        data = bytearray(pe.SIZE)
        struct.pack_into(">I", data, 0, pe.VERSION_WORD)
        struct.pack_into(">i", data, pe.CONT_LEVEL, 200)
        struct.pack_into(">I", data, pe.CONT_SIZE, 0x84)
        write(prof, bytes(data))
        logs = []
        r = sh.prepare_profile(cwd, log=logs.append)
        ci = pe.continue_info(bytearray(open(prof, "rb").read()))
        check(r == "profile" and ci["level_id"] == -1 and ci["rec_size"] == 0 and logs, "6 continue record cleared")
        o = io.StringIO()
        sh.main(["--root", root, "--print"], out=o, environ=env0)
        check(json.loads(o.getvalue())["route"] == "profile", "6 a warm-up profile -> profile route")
        write(prof, b"short")
        r = sh.prepare_profile(cwd, log=logs.append)
        check(r == "first-boot" and not os.path.exists(os.path.dirname(prof)) and os.path.isdir(cwd),
              "6 broken profile removed (only its save folder) -> first-boot route")

        # 7. firstrun: the optional step
        names = lambda st: [s.name for s in st]  # noqa: E731
        check("shaders" not in names(fr.steps_for(root, "auto")), "7 auto without an ini key -> off (wizard default since pass 17)")
        write(os.path.join(root, "kz3-install.ini"), "[install]\nportable = 0\n[shaders]\nprepare = 0\n")
        check("shaders" not in names(fr.steps_for(root, "auto")) and "shaders" in names(fr.steps_for(root, "on"))
              and "shaders" not in names(fr.steps_for(rootp, "off")), "7 ini prepare=0 / --shaders on / off")
        check(fr.SHADER_STEP.always and fr.SHADER_STEP.optional and fr.SHADER_STEP.ninja
              and "--root" in fr.cmd_shaders(fr.Ctx(root=root, build="b"))[0], "7 step: always, optional, [x/y]")
        py = sys.executable
        steps = [fr.Step("one", "One", 1, lambda c: [[py, "-c", "print(1)"]]),
                 fr.Step("shaders", "Preparing shaders", 1, lambda c: [[py, "-c", (
                     "import sys; print('[5/10] Preparing shaders'); sys.exit(2)")]], always=True, ninja=True,
                     optional=True)]
        o = io.StringIO()
        with contextlib.redirect_stderr(io.StringIO()):
            rc = fr.main(["--dump", dump, "--root", root, "--log-dir", tmp, "--jobs", "1"], steps=steps, out=o)
        o = o.getvalue()
        check(rc == 0 and "RESULT ok=1" in o and "optional, the port is built" in o
              and "name=shaders done=5 total=10" in o, "7 a failed warm-up keeps the build ok, progress parsed")
        bar = io.StringIO()
        fr.ConsoleBar(bar, tty=False).line("PROGRESS step=9/9 name=shaders done=100 total=460 elapsed=1 pct=97")
        check("Preparing shaders (about 6:00 left)" in bar.getvalue(), f"7 console bar label {bar.getvalue()!r}")

        # 8. launch.py --prepare-shaders
        class UI:
            def __init__(self, answers):
                self.answers, self.asked, self.infos = list(answers), [], []

            def ask(self, t):
                self.asked.append(t)
                return self.answers.pop(0)

            def info(self, t):
                self.infos.append(t)
        write(os.path.join(root, "build", "build-stamp.json"), "{}")
        calls = []
        ui = UI([True])
        la.decide = lambda r: ("run", "")
        rc = la.main(["--root", root, "--prepare-shaders"], ui=ui, run=lambda a, **k: calls.append(a) or 0,
                     environ=env0)
        check(rc == 0 and len(calls) == 1 and calls[0][1].endswith("shaders.py") and "--force" in calls[0],
              "8 --prepare-shaders runs shaders.py --force")
        calls.clear()
        rc = la.main(["--root", root, "--prepare-shaders"], ui=UI([False]), run=lambda a, **k: calls.append(a) or 0,
                     environ=env0)
        check(rc == 0 and not calls, "8 No starts nothing")
        la.decide = lambda r: ("first-build", "The port is not built on this computer yet")
        ui = UI([])
        rc = la.main(["--root", root, "--prepare-shaders"], ui=ui, run=lambda a, **k: calls.append(a) or 0,
                     environ=env0)
        check(rc == 1 and not calls and ui.infos and "build it" in ui.infos[0], "8 not built -> says so, runs nothing")

    print("test_shaders: " + ("PASS" if not FAILS else f"FAIL ({len(FAILS)})"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
