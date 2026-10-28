#!/usr/bin/env python3
"""Host test for the release toolchain: tools/release/toolchain.json, fetch_toolchain.py, assemble_payload.py
--toolchain and firstrun.py's use of the payload toolchain (pass 8). No network: the "downloads" are file:// URLs
to zips built in a temp folder. Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_fetch_toolchain.py

Cases: the real manifest validates (TO-FILL allowed) and fails --strict while unpinned; placeholders expand; the
validator refuses http URLs, bad hashes, duplicate names, unsafe dests, a group without a licence source. Fetch: a
TO-FILL entry is refused without --fill (download deleted) and filled with it (hash written, the rest of the file
unchanged); a later hash mismatch is refused and deleted; a published-list mismatch is refused; zip-slip, symlink
and strip_top problems are refused. Payload: the fetched trees land in toolchain/<dest>/ minus the prune globs, the
licence texts in licenses/<group>/, the components in manifest.json; refused: a TO-FILL hash, a cache fetched with
another hash, a planted game file. firstrun: payload_tools finds the bundled tools; configure uses
cmake/llvm-mingw.cmake, LLVM_MINGW_ROOT, the bundled cmake/ninja and -O2 Release flags.
"""
import contextlib
import fnmatch
import hashlib
import io
import json
import os
import pathlib
import shutil
import stat
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import assemble_payload as ap  # noqa: E402
import fetch_toolchain as ft  # noqa: E402
import firstrun as fr  # noqa: E402
import test_assemble_payload as tap  # noqa: E402
import trial_build as tb  # noqa: E402

FAILS = []
SCHEMES = ("https", "file")


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  {detail}"[:800]))
    if not cond:
        FAILS.append(what)


def make_zip(path, members, symlink=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            z.writestr(name, data)
        if symlink:
            zi = zipfile.ZipInfo(symlink)
            zi.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(zi, "target")
    return path


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def url(p):
    return pathlib.Path(os.path.abspath(p)).as_uri()   # file:///C:/... on Windows too


def fetch(args, schemes=SCHEMES):
    buf = io.StringIO()
    rc = ft.main(args, out=buf, schemes=schemes)
    return rc, buf.getvalue()


def payload(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            rc = ap.main(args, out=buf)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 2
            print(e, file=buf)
    return rc, buf.getvalue()


def write_manifest(path, comps):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"format": 1, "about": "test", "components": comps}, f, indent=1)
        f.write("\n")


def comp(name, group, u, archive="zip", **kw):
    c = {"name": name, "group": group, "version": "1.0", "pin": "confirmed", "release_page": "https://example.org/",
         "url": u, "sha256": "TO-FILL", "archive": archive, "license": "MIT"}
    if archive == "zip":
        c.update({"dest": name, "strip_top": True, "license_globs": [], "prune": [], "notice": f"{name} {{version}}"})
    c.update(kw)
    return c


def test_real_manifest():
    m = ft.load()
    probs = ft.validate(m)
    check(not probs, "real toolchain.json validates (TO-FILL allowed)", probs)
    check(ft.validate(m, strict=True) != [] or all(c["sha256"] != ft.TO_FILL for c in m["components"]),
          "--strict refuses TO-FILL / TO-CONFIRM")
    names = {c["name"] for c in m["components"]}
    check({"llvm-mingw", "cmake", "ninja", "python"} <= names, "manifest pins llvm-mingw, cmake, ninja, python", names)
    for c in m["components"]:
        e = ft.expand(c)
        check("{" not in e["url"], f"{c['name']}: url placeholders expand", e["url"])
        if c["archive"] == "zip":
            check("{" not in e["notice"] and e["notice"], f"{c['name']}: NOTICE line present and expanded")
    lm = ft.expand([c for c in m["components"] if c["name"] == "llvm-mingw"][0])
    check(lm["url"].endswith(f"llvm-mingw-{lm['version']}-ucrt-x86_64.zip"), "llvm-mingw: UCRT x86_64 Windows-host zip",
          lm["url"])
    py = [c for c in m["components"] if c["name"] == "python"][0]
    check(py.get("packages") == [], "python: no extra packages (first-run tools use the standard library)")
    rc, out = fetch(["--check"])
    check(rc == 0 and "TO-FILL" in out, "--check on the real manifest passes", out)


def test_validator():
    good = {"format": 1, "components": [comp("a", "a", "https://x/a.zip", license_globs=["LICENSE"])]}
    check(ft.validate(good) == [], "minimal manifest validates", ft.validate(good))
    cases = {
        "http url": [comp("a", "a", "http://x/a.zip", license_globs=["L"])],
        "bad hash": [comp("a", "a", "https://x/a.zip", license_globs=["L"], sha256="abc")],
        "duplicate name": [comp("a", "a", "https://x/a.zip", license_globs=["L"]),
                           comp("a", "a", "https://x/b.zip", license_globs=["L"], dest="b")],
        "unsafe dest": [comp("a", "a", "https://x/a.zip", license_globs=["L"], dest="../up")],
        "no licence source": [comp("a", "a", "https://x/a.zip")],
        "licence file without name": [comp("a", "a", "https://x/a.zip", license_globs=["L"]),
                                      comp("al", "a", "https://x/L", archive="file")],
        "bad placeholder": [comp("a", "a", "https://x/{nope}.zip", license_globs=["L"])],
    }
    for what, comps in cases.items():
        probs = ft.validate({"format": 1, "components": comps})
        check(probs != [], f"validator refuses: {what}", probs)


def test_fetch_and_payload(tmp):
    src = os.path.join(tmp, "src")
    tz = make_zip(os.path.join(src, "tc-1.0.zip"), {"tc-1.0/bin/x86_64-w64-mingw32-clang": "clang\n",
                                                    "tc-1.0/i686-w64-mingw32/lib/a.a": "x",
                                                    "tc-1.0/LICENSE.TXT": "tc licence\n"})
    lic = os.path.join(src, "COPYING")
    with open(lic, "w") as f:
        f.write("ninja licence\n")
    nz = make_zip(os.path.join(src, "ninja-win.zip"), {"ninja": "ninja\n"})
    shalist = os.path.join(src, "sums.txt")
    with open(shalist, "w") as f:
        f.write(f"{sha(tz)}  tc-1.0.zip\n{'0' * 64}  other.zip\n")
    man = os.path.join(tmp, "toolchain.json")
    comps = [comp("tc", "tc", url(tz), license_globs=["LICENSE*"], prune=["i686-w64-mingw32/*"],
                  sha256_list=url(shalist)),
             comp("nj", "nj", url(nz), strip_top=False),
             comp("nj-license", "nj", url(lic), archive="file", license_file="nj-COPYING.txt")]
    write_manifest(man, comps)
    cache = os.path.join(tmp, "cache")
    before = open(man).read()

    rc, out = fetch(["--manifest", man, "--cache", cache])
    check(rc == 1 and "re-run with --fill" in out, "TO-FILL refused without --fill", out)
    check(not os.path.exists(os.path.join(cache, "downloads", "tc", "tc-1.0.zip")), "refused download deleted")
    rc, out = fetch(["--manifest", man, "--cache", cache], schemes=("https",))
    check(rc == 1 and "must use https" in out, "file:// refused with the real schemes", out)

    rc, out = fetch(["--manifest", man, "--cache", cache, "--fill"])
    check(rc == 0 and out.count("FILL ") == 3, "--fill fetches and records three hashes", out)
    after = open(man).read()
    m = json.loads(after)
    check([c["sha256"] for c in m["components"]] == [sha(tz), sha(nz), sha(lic)], "filled hashes are the files'")
    check(after.replace(sha(tz), "TO-FILL").replace(sha(nz), "TO-FILL").replace(sha(lic), "TO-FILL") == before,
          "fill changed nothing else in the manifest")
    tree = os.path.join(cache, "trees", "tc")
    check(os.path.isfile(os.path.join(tree, "bin", "x86_64-w64-mingw32-clang")), "strip_top unpacked tc")
    check(os.path.isfile(os.path.join(cache, "trees", "nj", "ninja")), "ninja unpacked as is")
    fetched = ft.read_fetched(cache)
    check(fetched.get("tc", {}).get("sha256") == sha(tz), "fetched.json records the hash")

    rc, out = fetch(["--manifest", man, "--cache", cache])
    check(rc == 0, "second fetch with the pinned hashes passes", out)

    # The upstream file changes under the same URL: refused, deleted, cache tree kept.
    make_zip(nz, {"ninja": "tampered\n"})
    rc, out = fetch(["--manifest", man, "--cache", cache, "--only", "nj"])
    check(rc == 1 and "mismatch" in out, "hash mismatch refused", out)
    check(not os.path.exists(os.path.join(cache, "downloads", "nj", "ninja-win.zip")), "mismatched download deleted")
    check(open(os.path.join(cache, "trees", "nj", "ninja")).read() == "ninja\n", "old tree untouched by a refusal")
    make_zip(nz, {"ninja": "ninja\n"})
    if sha(nz) != m["components"][1]["sha256"]:      # zip timestamps: re-pin the rebuilt file
        txt = open(man).read().replace(m["components"][1]["sha256"], "TO-FILL")
        open(man, "w").write(txt)
        fetch(["--manifest", man, "--cache", cache, "--only", "nj", "--fill"])

    # Published list disagrees.
    with open(shalist, "w") as f:
        f.write(f"{'1' * 64}  tc-1.0.zip\n")
    rc, out = fetch(["--manifest", man, "--cache", cache, "--only", "tc"])
    check(rc == 1 and "not the published one" in out, "published SHA-256 list mismatch refused", out)
    with open(shalist, "w") as f:
        f.write(f"{sha(tz)}  tc-1.0.zip\n")

    # Unsafe archives.
    for what, members, link, strip in (("zip-slip", {"top/../../evil": "x"}, None, True),
                                       ("absolute member", {"/abs": "x"}, None, False),
                                       ("symlink", {"top/a": "x"}, "top/link", True),
                                       ("two top folders", {"a/x": "1", "b/y": "2"}, None, True)):
        bz = make_zip(os.path.join(src, f"bad-{what}.zip".replace(" ", "-")), members, link)
        try:
            ft.unpack_zip(bz, os.path.join(tmp, "badout"), strip)
            ok = False
        except ValueError:
            ok = True
        check(ok and not os.path.exists(os.path.join(tmp, "badout")), f"unpack refuses {what}")
    check(not os.path.exists(os.path.join(tmp, "evil")), "nothing written outside by the zip-slip case")

    # ---- payload
    repo = os.path.join(tmp, "repo")
    tap.fake_repo(repo)
    shutil.rmtree(os.path.join(repo, "Game"))
    shutil.rmtree(os.path.join(repo, "build"))
    out_dir = os.path.join(tmp, "payload")
    ap.TOOLCHAIN_SCHEMES = SCHEMES
    rc, out = payload(["--repo", repo, "--out", out_dir, "--toolchain", cache, "--toolchain-manifest", man,
                       "--version", "1.2.3"])
    check(rc == 0, "payload with --toolchain assembles", out)
    files = tap.listing(out_dir)
    check("toolchain/tc/bin/x86_64-w64-mingw32-clang" in files, "compiler tree in toolchain/tc/")
    check(not any(f.startswith("toolchain/tc/i686-w64-mingw32/") for f in files), "pruned target dropped")
    check("toolchain/nj/ninja" in files, "ninja in toolchain/nj/")
    check("licenses/tc/tc-tree-LICENSE.TXT" in files and "licenses/nj/nj-COPYING.txt" in files,
          "licence texts in licenses/<group>/", sorted(f for f in files if f.startswith("licenses/")))
    pm = json.load(open(os.path.join(out_dir, "manifest.json")))
    check([c["name"] for c in pm.get("toolchain", [])] == ["tc", "nj", "nj-license"], "manifest.json lists components")
    check(any(f["path"] == "toolchain/nj/ninja" for f in pm["files"]), "toolchain files hashed in manifest.json")

    # Refusals: a cache fetched with another hash, a TO-FILL entry, a planted game file.
    m2 = json.load(open(man))
    m2["components"][0]["sha256"] = "2" * 64
    man2 = os.path.join(tmp, "toolchain2.json")
    write_manifest(man2, m2["components"])
    out2 = os.path.join(tmp, "payload2")
    rc, out = payload(["--repo", repo, "--out", out2, "--toolchain", cache, "--toolchain-manifest", man2])
    check(rc == 1 and "not fetched with the manifest's hash" in out and not os.path.exists(out2),
          "stale cache refused, nothing written", out)
    m2["components"][0]["sha256"] = "TO-FILL"
    write_manifest(man2, m2["components"])
    rc, out = payload(["--repo", repo, "--out", out2, "--toolchain", cache, "--toolchain-manifest", man2])
    check(rc == 1 and "TO-FILL" in out and not os.path.exists(out2), "TO-FILL refused for a payload", out)
    tap.put(cache, "trees/tc/share/x.psarc", b"PSAR\0\0")
    rc, out = payload(["--repo", repo, "--out", out2, "--toolchain", cache, "--toolchain-manifest", man])
    check(rc == 1 and "x.psarc" in out and not os.path.exists(out2), "game file in the toolchain refused", out)
    os.remove(os.path.join(cache, "trees", "tc", "share", "x.psarc"))
    return out_dir


def test_firstrun(tmp, payload_dir):
    exe = ".exe" if os.name == "nt" else ""
    root = os.path.join(tmp, "install")
    for rel in (f"toolchain/llvm-mingw/bin/x86_64-w64-mingw32-clang{exe}", f"toolchain/cmake/bin/cmake{exe}",
                f"toolchain/ninja/ninja{exe}", "toolchain/python/python.exe"):
        tap.put(root, rel, "x")
    found = fr.payload_tools(root)
    check(set(found) >= {"llvm_mingw", "cmake", "ninja"}, "payload_tools finds compiler, cmake, ninja", found)
    check(("python" in found) == (os.name == "nt"), "bundled python used on Windows only")
    check(fr.payload_tools(os.path.join(tmp, "nothing")) == {}, "no toolchain folder: nothing found")
    c = fr.Ctx(root=root, build=os.path.join(root, "build"), toolchain=found["llvm_mingw"], cmake=found["cmake"],
               ninja=found["ninja"])
    cfg = fr.cmd_configure(c)[0]
    check(cfg[0] == found["cmake"], "configure runs the bundled cmake")
    check(f"-DCMAKE_TOOLCHAIN_FILE={fr.cm_path(os.path.join(root, 'cmake', 'llvm-mingw.cmake'))}" in cfg,
          "configure uses cmake/llvm-mingw.cmake", cfg)
    check(f"-DLLVM_MINGW_ROOT={fr.cm_path(found['llvm_mingw'])}" in cfg, "configure passes LLVM_MINGW_ROOT")
    check(f"-DCMAKE_MAKE_PROGRAM={fr.cm_path(found['ninja'])}" in cfg, "configure passes the bundled ninja")
    check("-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" in cfg and "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" in cfg,
          "Release at -O2 like the verified build")
    check(fr.cmd_compile(c)[0][0] == found["cmake"], "compile runs the bundled cmake")
    lift = fr.cmd_ppu_lift(c)[-1]
    check(lift[lift.index("--chunks") + 1] == "64" if "--chunks" in lift else False,
          "PPU lift asks for 64 chunks like tools/lift.sh", lift)
    cross = fr.cmd_configure(fr.Ctx(root=root, build=os.path.join(root, "build")))[0]
    check(cross[0] == "cmake" and any(a.endswith("mingw-w64.cmake") for a in cross), "no toolchain: NixOS cross file")
    # Pass 11: -D paths go to CMake with forward slashes on Windows; untouched elsewhere.
    check(fr.cm_path("Z:\\home\\a b\\toolchain\\llvm-mingw", sep="\\") == "Z:/home/a b/toolchain/llvm-mingw"
          and fr.cm_path("/a\\b", sep="/") == "/a\\b", "cm_path: forward slashes for CMake on Windows only")
    repo = os.path.dirname(os.path.dirname(HERE))
    tf = os.path.join(repo, "cmake", "llvm-mingw.cmake")
    text = open(tf).read() if os.path.isfile(tf) else ""
    check("x86_64-w64-mingw32-" in text and "clang" in text and "LLVM_MINGW_ROOT" in text and '"-static"' in text,
          "cmake/llvm-mingw.cmake: prefixed clang, LLVM_MINGW_ROOT, -static")
    # Stage 0 (2026-10-12): llvm-mingw defaults to Windows 7 headers; the runtime needs 0x602+ declarations.
    check("_WIN32_WINNT=0x0A00" in text, "cmake/llvm-mingw.cmake: Windows 10 API level (_WIN32_WINNT)")
    real = json.load(open(os.path.join(HERE, "toolchain.json")))
    llvm = next((c for c in real["components"] if c["name"] == "llvm-mingw"), {})
    check(any(fnmatch.fnmatchcase("lib/clang/23/lib/linux/libclang_rt.asan-x86_64.so", g) for g in llvm.get("prune", [])),
          "llvm-mingw prune drops the Linux compiler-rt libraries (ELF, refused by the payload scrub)")
    check(os.path.isfile(os.path.join(payload_dir, "cmake", "mingw-w64.cmake")), "payload carries cmake/*.cmake")


FAKE_CMAKE = """#!{py}
import os, sys
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "calls.txt"), "a") as f:
    f.write(" ".join(sys.argv[1:]) + "\\n")
if sys.argv[1] == "--build":
    os.makedirs(sys.argv[2], exist_ok=True)
    open(os.path.join(sys.argv[2], "kz3.exe"), "wb").write(b"MZ" + b"\\0" * 62)
sys.exit(int(os.environ.get("FAKE_CMAKE_RC", "0")))
"""


def test_trial_build(tmp):
    if os.name == "nt":
        print("ok   trial_build: SKIP on Windows (fake cmake is a script)")
        return
    root = os.path.join(tmp, "trial-root")
    cm = tap.put(root, "toolchain/cmake/bin/cmake", FAKE_CMAKE.format(py=sys.executable))
    os.chmod(cm, 0o755)
    tap.put(root, "toolchain/llvm-mingw/bin/x86_64-w64-mingw32-clang", "x")
    build = os.path.join(tmp, "trial-build")
    buf = io.StringIO()
    rc = tb.main(["--root", root, "--build", build, "--jobs", "2"], out=buf)
    check(rc == 2 and "missing" in buf.getvalue(), "trial_build refuses a build dir without lift outputs")
    tap.put(build, "recomp/ppu_recomp.h", "//")
    tap.put(build, "spu/gen/x.c", "//")
    buf = io.StringIO()
    rc = tb.main(["--root", root, "--build", build, "--jobs", "2"], out=buf)
    rep = json.load(open(os.path.join(build, "trial.json")))
    calls = open(os.path.join(root, "toolchain", "cmake", "bin", "calls.txt")).read().splitlines()
    check(rc == 0 and [st["name"] for st in rep["steps"]] == ["configure", "compile"] and rep["exe_bytes"] == 64,
          "trial_build runs configure + compile and records them", buf.getvalue() + str(rep))
    check(len(calls) == 2 and "llvm-mingw.cmake" in calls[0] and calls[1].startswith("--build"),
          "trial_build uses firstrun's configure (llvm-mingw.cmake) and compile", calls)
    os.environ["FAKE_CMAKE_RC"] = "1"
    try:
        rc = tb.main(["--root", root, "--build", build, "--jobs", "2"], out=io.StringIO())
    finally:
        del os.environ["FAKE_CMAKE_RC"]
    rep = json.load(open(os.path.join(build, "trial.json")))
    check(rc == 1 and len(rep["steps"]) == 1, "trial_build stops at a failing configure")


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-toolchain-")
    try:
        test_real_manifest()
        test_validator()
        pd = test_fetch_and_payload(tmp)
        test_firstrun(tmp, pd)
        test_trial_build(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'PASS' if not FAILS else 'FAIL'}: {len(FAILS)} failure(s)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
