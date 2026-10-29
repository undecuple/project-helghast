#!/usr/bin/env python3
"""Host test (public issue 20 follow-up): an install folder moved or renamed after a build must not
reuse the build tree. Builds in A with fake steps that behave like cmake (refuse a cache made in another folder) and
ninja (create the output folders named in build.ninja), moves the folder to B, runs again: configure and compile run
again from an empty obj/, the lift steps stay done, and nothing is created under A. Also: the launcher's decide() sees
the move, paths below the old folder follow it, a recorded path with [ ] ; is not offered again. No game data."""
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import firstrun as fr   # noqa: E402
import launch as la     # noqa: E402

FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def py(code):
    return [sys.executable, "-c", code]


def steps_for_root():
    def lift(c):
        return [py(f"import os; os.makedirs({c.b()!r}, exist_ok=True); open({c.b('lift.count')!r}, 'a').write('x')")]

    def configure(c):
        code = (
            "import os,sys\n"
            f"obj={c.b('obj')!r}; home={os.path.join(c.root, 'port', 'kz3')!r}\n"
            "cache=os.path.join(obj,'CMakeCache.txt')\n"
            "if os.path.exists(cache):\n"
            "    old=[l for l in open(cache) if l.startswith('CMAKE_HOME_DIRECTORY')][0].split('=',1)[1].strip()\n"
            "    if os.path.normpath(old)!=os.path.normpath(home): sys.exit('cmake: cache from another folder')\n"
            "os.makedirs(obj, exist_ok=True)\n"
            "open(cache,'w').write('CMAKE_HOME_DIRECTORY:INTERNAL='+home+'\\nCMAKE_CACHEFILE_DIR:INTERNAL='+obj+'\\n')\n"
            "open(os.path.join(obj,'build.ninja'),'w').write(os.path.join(obj,'sub'))\n"
            f"open({c.b('configure.count')!r}, 'a').write('x')\n")
        return [py(code)]

    def compile_(c):
        code = ("import os\n"
                f"d=open({c.b('obj', 'build.ninja')!r}).read()\nos.makedirs(d, exist_ok=True)\n"
                f"open({c.b('compile.count')!r}, 'a').write('x')\n")
        return [py(code)]

    return [fr.Step("lift", "Lift", 1, lift, keys=("lift",)),
            fr.Step("configure", "Configure", 1, configure, keys=("lift", "runtime")),
            fr.Step("compile", "Compile", 1, compile_, keys=("lift", "runtime"))]


def count(root, name):
    try:
        return len(open(os.path.join(root, "build", name + ".count")).read())
    except OSError:
        return 0


def go(root, tmp, dump):
    out = io.StringIO()
    rc = fr.main(["--dump", dump, "--root", root, "--jobs", "1", "--log-dir", os.path.join(tmp, "dl"),
                  "--bar"], steps=steps_for_root(), out=out)
    return rc, out.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-moved-")
    try:
        dump = os.path.join(tmp, "dump")
        os.makedirs(dump)
        A, B = os.path.join(tmp, "A", "Project Helghast"), os.path.join(tmp, "B", "Project Helghast")
        os.makedirs(A)
        rc, out = go(A, tmp, dump)
        check(rc == 0 and (count(A, "configure"), count(A, "compile"), count(A, "lift")) == (1, 1, 1), f"1 build in A: rc={rc}")
        rc, out = go(A, tmp, dump)
        check(rc == 0 and count(A, "configure") == 1 and count(A, "compile") == 1, "1b a second run in A skips everything")
        os.makedirs(os.path.dirname(B))
        shutil.move(A, B)
        shutil.rmtree(os.path.dirname(A))
        rc, out = go(B, tmp, dump)
        check(rc == 0, f"2 run in B succeeds (no 'cache from another folder'): rc={rc} {out[-200:]!r}")
        check(count(B, "configure") == 2 and count(B, "compile") == 2, "2 configure and compile ran again in B")
        check(count(B, "lift") == 1, "2 the lift step stayed done (its outputs hold no absolute path)")
        check(not os.path.exists(os.path.dirname(A)), "2 nothing was created under the old folder")
        check("moved or renamed" in out, f"2 one console line says why: {out[:200]!r}")
        rc, out = go(B, tmp, dump)
        check(count(B, "configure") == 2 and "moved or renamed" not in out, "3 a further run in B skips everything, no note")

        # decide() sees a move; paths below the old folder follow it
        C = os.path.join(tmp, "C")
        iso_dir = os.path.join(C, "game", "disc")
        os.makedirs(os.path.join(C, "build", "obj"))
        os.makedirs(iso_dir)
        eb = os.path.join(tmp, "EBOOT.elf")
        open(eb, "wb").write(b"x")
        ctx0 = fr.Ctx(root=C, build=os.path.join(C, "build"), dump=iso_dir, update=dump, eboot=eb)
        json.dump({"dump": iso_dir, "update": dump, "eboot": eb, "facts": {}}, open(os.path.join(C, "build", "paths.json"), "w"))
        json.dump({"parts": ctx0.parts(), "eboot_stat": fr.eboot_stat(eb), "root": C}, open(os.path.join(C, "build", "build-stamp.json"), "w"))
        open(os.path.join(C, "build", "kz3.exe"), "wb").write(b"x")
        check(la.decide(C)[0] == "run", f"4 decide: run in the folder it was built in: {la.decide(C)}")
        D = os.path.join(tmp, "D")
        shutil.move(C, D)
        act, why = la.decide(D)
        pj = json.load(open(os.path.join(D, "build", "paths.json")))
        check(act == "rebuild" and "moved or renamed" in why, f"4 decide: rebuild after the move: {act} {why}")
        check(pj["dump"] == os.path.join(D, "game", "disc") and pj["update"] == dump, f"4 paths.json follows: {pj['dump']}")
        check(fr.Ctx(root=D, build=os.path.join(D, "build"), dump=pj["dump"], update=dump, eboot=eb).parts()["base"]
              == ctx0.parts()["base"], "4 the fingerprint of an in-root dump survives the move")

        # a recorded path with [ ] ; is not offered again
        inc = os.path.join(tmp, "inc")
        os.makedirs(inc)
        open(os.path.join(inc, "kz3-install.ini"), "w").write(
            "[paths]\ndump=C:\\a[b]\\dump\nupdate=C:\\upd\neboot=\nfont_dir=\n")
        check(la.setup_paths(inc) == {}, "5 kz3-install.ini [paths] with '[' in dump is dropped")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_moved_root:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
