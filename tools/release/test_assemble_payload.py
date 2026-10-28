#!/usr/bin/env python3
"""Host test for tools/release/assemble_payload.py on a fake repository tree in a temp folder (no game data, no git
needed). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_assemble_payload.py

Cases: a clean fake tree assembles; only allowlisted files land in the payload (work/ outputs other than the toolkit,
Game/, tests, fw_spu/, scratch/ and caches stay out); the manifest lists every file with the right size and SHA-256
and the total; the same tree gives the same manifest twice; --dry-run lists and writes nothing; a missing toolkit is
refused. Negative controls, each refused with nothing written: a planted PSARC-headed file, an EBOOT.elf, a .prx, a
Linux home path, a Windows user path, a secret, an oversized binary. A symlink in a copied tree is not followed.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assemble_payload as ap  # noqa: E402

FAILS = []
# Built from pieces so this file never matches the export scrub itself.
HOME_LINUX = "/" + "home" + "/someone/Projects/kz3"
HOME_WIN = "C:" + "\\Users\\someone\\Documents"
SECRET = "gh" + "p_" + "A" * 24


def check(cond, what, detail=""):
    tail = "" if cond else "  " + (detail if len(detail) <= 600 else "..." + detail[-600:])  # the end holds MISSING/SCRUB lines
    print(("ok   " if cond else "FAIL ") + what + tail)
    if not cond:
        FAILS.append(what)


def put(root, rel, data=b"x\n"):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    return p


def fake_repo(root):
    for rel in ("tools/release/firstrun.py", "tools/release/firstrun_check.py", "tools/release/version_check.py", "tools/release/crash_report.py",
                "tools/smoke/pso_template.py", "tools/release/launch.py", "tools/release/lift_fixups.py", "tools/release/shaders.py", "tools/release/names.py", "tools/release/support.py", "tools/release/support_text.py", "tools/release/rpcs3_import.py", "tools/release/rpcs3_text.py", "tools/release/rpcs3_locate.py",
                "tools/assets/profile_edit.py",
                "tools/release/eboot_sha256.json", "tools/release/test_firstrun.py",
                "tools/release/assemble_payload.py", "tools/jobmods.py", "tools/elf_info.py", "tools/psarc.py",
                "tools/spu/lift_spu.py", "tools/spu/gen_spu_register.py", "tools/spu/kz3ver.py",
                "tools/spu/test_spu_lift.py", "re/data/spu_map_114.tsv", "re/data/spu_seeds_114.tsv",
                "re/data/spu_coverage_seeds.tsv", "re/data/other_114.tsv", "re/data/campaign_levels.tsv", "port/kz3/CMakeLists.txt", "port/kz3/main.cpp",
                "port/spu/ppu_overrides.txt", "port/vfs/kz3_overlay.c", "port/vfs/test_vfs_overlay.c",
                "port/net/kz3_net.c", "port/net/test_kz3_net.c",
                "port/kz3/__pycache__/x.pyc", "cmake/mingw-w64.cmake", "cmake/notes.txt",
                "public/LICENSE", "public/README.md", "public/NOTICE.md", "installer/run.cmd",
                "work/ps3recomp-patched/LICENSE", "work/ps3recomp-patched/CMakeLists.txt",
                "work/ps3recomp-patched/.kz3-patchset", "work/ps3recomp-patched/tools/ppu_lifter.py",
                "work/ps3recomp-patched/runtime/a.c", "work/ps3recomp-patched/tests/sync/CMakeLists.txt",
                "work/ps3recomp-patched/third_party/at3_standalone/COPYING.LGPLv2.1",
                "work/ps3recomp-patched/fw_spu/spu_libsre/lifted/x.c", "work/ps3recomp-patched/scratch/junk.cpp",
                "work/ps3recomp-patched/docs/a.md", "work/recomp/ppu_0001.c", "work/analysis/functions.json",
                "build/kz3-win/kz3.exe", "Game/PS3_GAME/PARAM.SFO", "docs/in-progress/porting-status.md"):
        put(root, rel, f"// {rel}\n")
    put(root, "work/ps3recomp-patched/.kz3-patchset", "abc123-def\n")
    # sources the glue's CMakeLists names outside port/kz3 (pass 16: the payload must carry every one of them)
    put(root, "port/kz3/CMakeLists.txt", "target_sources(kz3 PRIVATE main.cpp ../net/kz3_net.c ../vfs/kz3_overlay.c)\n")
    # the notice fonts (installer_art.py --fetch-fonts' folder) and their pins in installer/art/art.json
    pins = {}
    for fn in ("Rajdhani-Medium.ttf", "Rajdhani-Bold.ttf"):
        data = b"\0\1\0\0" + fn.encode() + bytes(64)
        put(root, "work/release/fonts/" + fn, data)
        pins[fn] = hashlib.sha256(data).hexdigest()
    put(root, "work/release/fonts/OFL-rajdhani.txt", "SIL OPEN FONT LICENSE Version 1.1\n")
    put(root, "work/release/sdl3/SDL3.dll", fake_dll(3 << 20))   # over the 2 MiB binary cap on purpose
    put(root, "work/release/sdl3/LICENSE.txt", "Copyright (C) 1997-2026 Sam Lantinga\n")
    put(root, "installer/art/art.json", json.dumps({"format": 1, "fonts_sha256": pins}))


def fake_dll(size, machine=0x8664, flags=0x2000):
    """A minimal PE header: MZ, e_lfanew = 64, PE signature, machine, characteristics (0x2000 = DLL)."""
    head = bytearray(64) + b"PE\0\0" + machine.to_bytes(2, "little") + bytes(16) + flags.to_bytes(2, "little")
    head[:2] = b"MZ"
    head[60:64] = (64).to_bytes(4, "little")
    return bytes(head) + bytes(size - len(head))


def run(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            rc = ap.main(args, out=buf)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 2
            print(e, file=buf)
    return rc, buf.getvalue()


def listing(d):
    out = set()
    for dd, _, names in os.walk(d):
        for n in names:
            out.add(os.path.relpath(os.path.join(dd, n), d).replace(os.sep, "/"))
    return out


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-payload-")
    try:
        repo = os.path.join(tmp, "repo")
        fake_repo(repo)
        # the exported public repo keeps public/'s files at its root: collect() must find them there too
        root_repo = os.path.join(tmp, "root-layout")
        shutil.copytree(repo, root_repo)
        for n in ("LICENSE", "README.md", "NOTICE.md"):
            os.replace(os.path.join(root_repo, "public", n), os.path.join(root_repo, n))
        files, missing = ap.collect(root_repo)
        dests = {d for d, _ in files}
        check({"licenses/LICENSE.txt", "README.md", "NOTICE.md"} <= dests, "root layout: LICENSE, README, NOTICE found", str(missing))
        check(not [m for m in missing if m.startswith("public/")], "root layout: nothing reported missing under public/", str(missing))
        os.remove(os.path.join(root_repo, "LICENSE"))
        check("public/LICENSE" in ap.collect(root_repo)[1], "root layout: a missing LICENSE is still refused")
        out = os.path.join(tmp, "payload")
        rc, txt = run(["--repo", repo, "--out", out, "--version", "0.1.0"])
        check(rc == 0 and "scrub clean" in txt, "clean fake tree assembles", txt)
        got = listing(out)
        want = {"kz3-setup/release/firstrun.py", "kz3-setup/release/firstrun_check.py",
                "kz3-setup/release/version_check.py", "kz3-setup/release/crash_report.py", "kz3-setup/release/eboot_sha256.json", "kz3-setup/jobmods.py",
                "kz3-setup/pso_template.py", "kz3-setup/release/launch.py", "kz3-setup/release/lift_fixups.py", "kz3-setup/release/shaders.py", "kz3-setup/release/names.py", "kz3-setup/release/support.py", "kz3-setup/release/support_text.py", "kz3-setup/release/rpcs3_import.py", "kz3-setup/release/rpcs3_text.py", "kz3-setup/release/rpcs3_locate.py",
                "kz3-setup/assets/profile_edit.py",
                "kz3-setup/elf_info.py", "kz3-setup/spu/lift_spu.py", "kz3-setup/spu/gen_spu_register.py",
                "kz3-setup/spu/kz3ver.py", "re/data/spu_map_114.tsv", "re/data/spu_seeds_114.tsv",
                "re/data/spu_coverage_seeds.tsv", "re/data/campaign_levels.tsv", "port/kz3/CMakeLists.txt",
                "port/kz3/main.cpp", "port/spu/ppu_overrides.txt", "port/vfs/kz3_overlay.c", "port/net/kz3_net.c",
                "cmake/mingw-w64.cmake",
                "ps3recomp/LICENSE", "ps3recomp/CMakeLists.txt", "ps3recomp/.kz3-patchset",
                "ps3recomp/tools/ppu_lifter.py", "ps3recomp/runtime/a.c", "ps3recomp/tests/sync/CMakeLists.txt",
                "ps3recomp/third_party/at3_standalone/COPYING.LGPLv2.1", "licenses/LICENSE.txt",
                "licenses/ps3recomp-LICENSE.txt", "licenses/at3_standalone-COPYING.LGPLv2.1.txt",
                "README.md", "NOTICE.md", "run.cmd", "manifest.json", "VERSION.txt",
                "fonts/Rajdhani-Medium.ttf", "fonts/Rajdhani-Bold.ttf", "licenses/Rajdhani-OFL.txt",
                "runtime/SDL3.dll", "licenses/SDL3-LICENSE.txt"}
        check(got == want, "payload holds exactly the allowlisted files",
              f"extra {sorted(got - want)} missing {sorted(want - got)}")
        # profile_edit.py finds its level table where it looks for it in the repo (parents[2]/re/data)
        pe = Path(out) / "kz3-setup" / "assets" / "profile_edit.py"
        check((pe.parents[2] / "re" / "data" / "campaign_levels.tsv").is_file(),
              "campaign_levels.tsv sits where profile_edit.py's LEVELS_TSV resolves in the payload", str(pe))

        with open(os.path.join(out, "manifest.json"), encoding="utf-8") as f:
            man = json.load(f)
        ok = man["count"] == len(want) - 2 and man["ps3recomp_patchset"] == "abc123-def" and man["version"] == "0.1.0"
        ok &= open(os.path.join(out, "VERSION.txt")).read() == "0.1.0\n"
        ok &= [e["path"] for e in man["files"] if e.get("portable_only")] == ["run.cmd"]   # release pass 51
        tot = 0
        for e in man["files"]:
            p = os.path.join(out, *e["path"].split("/"))
            with open(p, "rb") as f:
                b = f.read()
            ok &= len(b) == e["size"] and hashlib.sha256(b).hexdigest() == e["sha256"]
            tot += e["size"]
        check(ok and tot == man["total_bytes"], "manifest sizes, SHA-256 and total match the files", man)

        rc, txt = run(["--repo", repo, "--out", out])
        check(rc != 0 and "not empty" in txt, "existing payload is not overwritten without --replace", txt)
        rc, txt = run(["--repo", repo, "--out", out, "--replace", "--version", "0.1.0"])
        with open(os.path.join(out, "manifest.json"), encoding="utf-8") as f:
            man2 = json.load(f)
        check(rc == 0 and man2 == man, "same sources give the same manifest (--replace)", txt)

        rc, txt = run(["--repo", repo, "--out", os.path.join(tmp, "v"), "--version", "1.0; rm"])
        check(rc == 2 and not os.path.exists(os.path.join(tmp, "v")), "bad --version refused", txt)
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 0 and "kz3-setup/release/firstrun.py" in txt and "fw_spu" not in txt
              and not os.path.exists(os.path.join(tmp, "repo", "payload")), "--dry-run lists and writes nothing", txt)

        # a symlink inside a copied tree is skipped, never followed
        target = put(tmp, "outside/secret_data.c", "outside\n")
        try:
            os.symlink(target, os.path.join(repo, "port", "kz3", "link.c"))
            rc, txt = run(["--repo", repo, "--dry-run"])
            check(rc == 0 and "link.c" not in txt, "symlinks in trees are not followed", txt)
            os.remove(os.path.join(repo, "port", "kz3", "link.c"))
        except (OSError, NotImplementedError):
            print("skip symlink case (no symlink support)")

        # a source CMakeLists names outside port/kz3 that no rule carries is refused (pass 16: ../net/kz3_net.c)
        cm = os.path.join(repo, "port", "kz3", "CMakeLists.txt")
        with open(cm, encoding="utf-8") as f:
            cm_text = f.read()
        put(repo, "port/kz3/CMakeLists.txt", cm_text + "target_sources(kz3 PRIVATE ../elsewhere/extra.c)\n")
        put(repo, "port/elsewhere/extra.c", "// extra\n")
        rc, txt = run(["--repo", repo, "--out", os.path.join(tmp, "cm"), "--version", "1.0"])
        check(rc == 1 and "MISSING port/elsewhere/extra.c" in txt and not os.path.exists(os.path.join(tmp, "cm")),
              "a CMakeLists source outside the rules is refused", txt)
        put(repo, "port/kz3/CMakeLists.txt", cm_text)
        shutil.rmtree(os.path.join(repo, "port", "elsewhere"))

        # a toolkit patched from another port/patches set is refused; the matching stamp passes (pass 16)
        put(repo, "port/patches/0001-a.patch", "--- a\n+++ b\n")
        put(repo, "port/patches/0002-b.patch", "--- c\n+++ d\n")
        rc, txt = run(["--repo", repo, "--out", os.path.join(tmp, "st"), "--version", "1.0"])
        check(rc == 1 and "was made from another port/patches set" in txt
              and not os.path.exists(os.path.join(tmp, "st")), "a stale toolkit (patchset stamp) is refused", txt)
        want = hashlib.sha256(b"--- a\n+++ b\n--- c\n+++ d\n").hexdigest()[:16]
        put(repo, "work/ps3recomp-patched/.kz3-patchset", want + "-def\n")
        rc, txt = run(["--repo", repo, "--out", os.path.join(tmp, "st"), "--version", "1.0"])
        check(rc == 0 and "scrub clean" in txt, "the matching patchset stamp passes", txt)
        shutil.rmtree(os.path.join(repo, "port", "patches"))
        put(repo, "work/ps3recomp-patched/.kz3-patchset", "abc123-def\n")

        # negative controls: each planted file refuses the payload and nothing is written
        planted = [
            ("port/kz3/data_ps3.dat", b"PSAR\0\1\0\4zlib" + b"\0" * 64, "PSARC"),
            ("port/spu/EBOOT.elf", b"\x7fELF\2\2\1" + b"\0" * 64, "game-data name"),
            ("work/ps3recomp-patched/libs/libsre.prx", b"\x7fELF\2\2\1" + b"\0" * 64, "game-data name"),
            ("port/vfs/paths.c", f'const char *p = "{HOME_LINUX}/Game";\n', "home path"),
            ("port/kz3/win.txt", f"log at {HOME_WIN}\\x.log\n", "home path"),
            ("cmake/token.cmake", f"set(T {SECRET})\n", "secret"),
            ("port/kz3/blob.bin", b"\0" * (ap.MAX_BINARY + 1), "too big"),
        ]
        for rel, data, why in planted:
            p = put(repo, rel, data)
            o2 = os.path.join(tmp, "refused")
            rc, txt = run(["--repo", repo, "--out", o2])
            check(rc == 1 and why in txt and rel.split("/")[-1] in txt and not os.path.exists(o2)
                  and not os.path.exists(o2 + ".partial"), f"refused: {rel} ({why})", txt)
            os.remove(p)
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 0, "clean again after removing the planted files", txt)

        exe = os.path.join(tmp, "Project Helghast.exe")
        with open(exe, "wb") as f:
            f.write(b"MZ" + b"\0" * 4000)
        out2 = os.path.join(tmp, "payload2")
        rc, txt = run(["--repo", repo, "--out", out2, "--launcher", exe])
        with open(os.path.join(out2, "manifest.json"), encoding="utf-8") as f:
            names = [e["path"] for e in json.load(f)["files"]]
        check(rc == 0 and "Project Helghast.exe" in names, "--launcher adds the launcher exe to the payload", txt)
        rc, txt = run(["--repo", repo, "--dry-run", "--launcher", exe + ".missing"])
        check(rc == 1 and "MISSING" in txt, "a missing --launcher is refused", txt)

        # the notice fonts: hash-checked against art.json, both required, --fonts picks another folder
        fdir = os.path.join(repo, "work", "release", "fonts")
        medium = os.path.join(fdir, "Rajdhani-Medium.ttf")
        with open(medium, "rb") as f:
            orig = f.read()
        with open(medium, "wb") as f:
            f.write(orig + b"x")
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 1 and "Rajdhani-Medium.ttf: SHA-256 differs" in txt, "a font that differs from its pin is refused",
              txt)
        os.remove(medium)
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 1 and "MISSING" in txt and "--fetch-fonts" in txt, "a missing font is refused with the fix", txt)
        alt = os.path.join(tmp, "fonts-alt")
        shutil.copytree(fdir, alt)
        with open(os.path.join(alt, "Rajdhani-Medium.ttf"), "wb") as f:
            f.write(orig)
        rc, txt = run(["--repo", repo, "--dry-run", "--fonts", alt])
        check(rc == 0, "--fonts DIR is used instead of work/release/fonts", txt)
        os.remove(os.path.join(alt, "OFL-rajdhani.txt"))
        rc, txt = run(["--repo", repo, "--dry-run", "--fonts", alt])
        check(rc == 1 and "OFL text" in txt, "fonts without their OFL text are refused", txt)
        with open(medium, "wb") as f:
            f.write(orig)

        # SDL3.dll (pads, patch 4010): required, 64-bit PE DLL, own size cap, its licence text beside it
        sdir = os.path.join(repo, "work", "release", "sdl3")
        dll = os.path.join(sdir, "SDL3.dll")
        with open(dll, "rb") as f:
            good = f.read()
        os.remove(dll)
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 1 and "MISSING" in txt and "SDL3.dll" in txt and "stage_sdl3.py" in txt,
              "a payload without SDL3.dll is refused with the fix", txt)
        for what, blob in (("32-bit", fake_dll(1000, machine=0x14c)), ("not a DLL", fake_dll(1000, flags=0x0002)),
                           ("not a PE", b"\x7fELF" + bytes(100)), ("too big", fake_dll(ap.SDL3_MAX_BYTES + 1))):
            with open(dll, "wb") as f:
                f.write(blob)
            rc, txt = run(["--repo", repo, "--dry-run"])
            check(rc == 1 and "SCRUB FAIL runtime/SDL3.dll" in txt, f"a {what} SDL3.dll is refused", txt)
        with open(dll, "wb") as f:
            f.write(good)
        os.remove(os.path.join(sdir, "LICENSE.txt"))
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 1 and "LICENSE.txt" in txt, "SDL3.dll without its licence text is refused", txt)
        put(repo, "work/release/sdl3/LICENSE.txt", "Copyright (C) 1997-2026 Sam Lantinga\n")
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 0, "SDL3.dll + licence restored: payload assembles again", txt)

        put(repo, "port/kz3/notes.txt", "see %USERPROFILE%\\logs and C:\\Users\\<name>\\x, /home/ is fine\n")
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 0, "placeholders (%USERPROFILE%, <name>, bare /home/) are not home paths", txt)

        shutil.rmtree(os.path.join(repo, "work", "ps3recomp-patched"))
        rc, txt = run(["--repo", repo, "--dry-run"])
        check(rc == 1 and "MISSING work/ps3recomp-patched" in txt and "ps3recomp-patched.sh" in txt,
              "missing toolkit is refused with the fix", txt)

        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "assemble_payload.py"), encoding="utf-8") as f:
            own = f.read()
        check(not [w for w, rx in ap.TEXT_DENY if rx.search(own)], "assemble_payload.py does not trip its own scrub")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"assemble_payload: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
