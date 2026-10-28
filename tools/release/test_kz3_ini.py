#!/usr/bin/env python3
"""Host test for "no launch options" (release pass 26): port/kz3/kz3_ini.h (the runtime's kz3.ini reader, compiled
with the host C compiler), the launcher's kz3.ini writer (launch.write_game_ini / game_ini) read back by that same C
code, firstrun's CPU check (cpu_fma: compiler probe, Windows feature call, /proc/cpuinfo; -DKZ3_SPU_FMA in the
configure command), the CPU-change marker reset, the optional cleanup of build intermediates and the firmware-font
helpers. No game data, no toolchain beyond a host cc (the C half says SKIP without one), no network.
Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_kz3_ini.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def write(p, text="x", mode="w"):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, mode + ("b" if isinstance(text, bytes) else ""), **({} if isinstance(text, bytes) else {"encoding": "utf-8"})) as f:
        f.write(text)


def c_half(tmp):
    cc = next((shutil.which(c) for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if not cc:
        print("SKIP the C half: no host C compiler")
        return
    exe = os.path.join(tmp, "test_kz3_ini")
    r = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(REPO, "port", "kz3"),
                        os.path.join(HERE, "test_kz3_ini.c"), "-o", exe], capture_output=True, text=True)
    check(r.returncode == 0, f"kz3_ini.h compiles warning-free ({os.path.basename(cc)}) {r.stderr[-300:]}")
    if r.returncode:
        return

    def dump(text):
        p = os.path.join(tmp, "k.ini")
        write(p, text)
        out = subprocess.run([exe, "dump", p], capture_output=True, text=True).stdout
        return dict(ln.split("=", 1) for ln in out.splitlines() if "=" in ln)

    d = dump(b"\xef\xbb\xbf; comment\r\n# other\r\n[game]\r\n  EBOOT  =  \"C:\\g\\EBOOT.elf\"  \r\n"
             b"vfs_root=rel\\vfs\r\n[display]\r\nwindow_size = 1080p\r\nlang=\r\nlang = french\r\n"
             b"font_dir = a ; b\r\nunknown = 1\r\n= 3\r\n[ eboot = x\r\n")
    check(d.get("eboot") == "C:\\g\\EBOOT.elf", f"BOM, CRLF, sections, case-insensitive key, quotes trimmed {d}")
    check(d.get("vfs_root") == "rel\\vfs" and d.get("window_size") == "1080p", f"no spaces, [display] key {d}")
    check(d.get("lang") == "french", f"an empty value is unset; the next non-empty one counts {d}")
    check(d.get("font_dir") == "a ; b" and "unknown" not in d, f"no inline comments; unknown keys ignored {d}")
    check(dump("eboot = first\neboot = second\n").get("eboot") == "first", "the first non-empty value counts")
    check(dump("") == {} and subprocess.run([exe, "dump", os.path.join(tmp, "none.ini")], capture_output=True,
                                            text=True).stdout.strip() == "nofile", "empty file / no file")
    check(subprocess.run([exe, "long"], capture_output=True, text=True).stdout.strip() == "refused",
          "a value longer than the buffer is refused, not cut")

    def res(base, v):
        return subprocess.run([exe, "resolve", base, v], capture_output=True, text=True).stdout.strip()
    check(res("C:/x", "D:\\g\\E.elf") == "D:\\g\\E.elf" and res("C:/x", "\\\\srv\\s") == "\\\\srv\\s"
          and res("C:/x", "/abs") == "/abs" and res("C:/x", "PS3_GAME/USRDIR/EBOOT.elf") == "C:/x/PS3_GAME/USRDIR/EBOOT.elf",
          "resolve: drive, UNC and rooted paths kept, relative = exe folder + value")

    # The launcher's file, read by the runtime's parser: every key the same.
    root = os.path.join(tmp, "Install Dir")
    vals = {"eboot": os.path.join(tmp, "upd", "USRDIR", "EBOOT.elf"), "vfs_root": os.path.join(root, "vfs"),
            "update_dir": "", "hdd0_root": "", "edat_cache": "e", "shader_cache": "s", "data_dir": root,
            "font_dir": os.path.join(tmp, "dev_flash", "data", "font"), "lang": "", "window_size": "2560x1440"}
    p = la.write_game_ini(root, vals)
    rt = subprocess.run([exe, "dump", p], capture_output=True, text=True).stdout
    got = dict(ln.split("=", 1) for ln in rt.splitlines())
    check(got == {k: v for k, v in vals.items() if v}, f"write_game_ini -> kz3_ini.h: same values {got}")
    check(la.read_game_ini(p) == got, "launch.read_game_ini agrees with the runtime's reader")
    envs = {k for k in ("PS3_VFS_ROOT", "KZ3_FONT_DIR", "KZ3_LANG", "KZ3_WINDOW_SIZE", "KZ3_UPDATE_DIR", "PS3_HDD0_ROOT",
                        "PS3_EDAT_CACHE", "RSX_SHADER_CACHE_DIR", "KZ3_CACHE_ROOT")}
    with open(os.path.join(REPO, "port", "kz3", "kz3_ini.h"), encoding="utf-8") as f:
        h = f.read()
    # cache_root (round KZ3-03) lands in the header with work/release/cache-root/cache-root.patch; until the maintainer
    # applies it the key is only required once the header has it (remove this line then).
    pend = set()
    check(all(f'"{e}"' in h for e in envs - pend) and all(f'"{k}"' in h for _, k in la.GAME_INI_KEYS if k not in pend),
          "every key the launcher writes and every launcher variable is in kz3_ini.h's table")


def cpu_half(tmp):
    def fake(out, rc=0):
        return lambda *a, **k: types.SimpleNamespace(returncode=rc, stdout=out)
    tc = os.path.join(tmp, "llvm-mingw")
    write(os.path.join(tc, "bin", "x86_64-w64-mingw32-clang.exe"), "")
    yes = "#define __x86_64__ 1\n#define __AVX__ 1\n#define __AVX2__ 1\n#define __FMA__ 1\n"
    no = "#define __x86_64__ 1\n#define __SSE4_2__ 1\n#define __AVX__ 1\n"
    check(fr.cpu_fma(tc, run=fake(yes))[0] is True, "compiler probe: __AVX__ + __FMA__ -> default build")
    check(fr.cpu_fma(tc, run=fake(no))[0] is False, "compiler probe: AVX without FMA -> OFF")
    check(fr.cpu_fma("", system="Windows", pf=lambda n: n == 40)[0] is True
          and fr.cpu_fma("", system="Windows", pf=lambda n: 0)[0] is False, "Windows: IsProcessorFeaturePresent(40)")
    ci = os.path.join(tmp, "cpuinfo")
    write(ci, "processor : 0\nflags : fpu sse4_1 avx fma avx2\n")
    check(fr.cpu_fma(tc + "x", system="Linux", cpuinfo=ci)[0] is True, "/proc/cpuinfo: avx + fma")
    write(ci, "processor : 0\nflags : fpu sse4_1 sse4_2\n")
    check(fr.cpu_fma("", system="Linux", cpuinfo=ci)[0] is False, "/proc/cpuinfo: no avx")
    check(fr.cpu_fma("", system="Linux", cpuinfo=ci + "x")[0] is None, "unknown -> None (CMake default)")
    check(fr.cpu_fma(tc, run=fake("", 1), system="Linux", cpuinfo=ci)[0] is False, "a failed probe falls back")
    c = fr.Ctx(root=tmp, build=os.path.join(tmp, "b"), spu_fma=False)
    check("-DKZ3_SPU_FMA=OFF" in fr.cmd_configure(c)[0], "configure: -DKZ3_SPU_FMA=OFF without FMA3")
    c.spu_fma = True
    check("-DKZ3_SPU_FMA=ON" in fr.cmd_configure(c)[0], "configure: -DKZ3_SPU_FMA=ON with it")
    c.spu_fma = None
    check(not any("KZ3_SPU_FMA" in x for x in fr.cmd_configure(c)[0]), "configure: unknown = no flag")


def build_half(tmp):
    b = os.path.join(tmp, "inst", "build")
    c = fr.Ctx(root=os.path.join(tmp, "inst"), build=b, spu_fma=False)
    sizes = {"obj/x.o": 3000, "obj/kz3.exe": 500, "recomp/f.c": 2000, "spu/s.c": 700, "analysis/a.json": 50}
    for rel, n in sizes.items():
        write(os.path.join(b, rel), b"\0" * n)
    for rel in ("kz3.exe", "kz3.ini", "paths.json", "fonts/R.ttf", "cache/edat/x", "logs/l.txt"):
        write(os.path.join(b, rel), "k")
    write(os.path.join(b, "build-stamp.json"), json.dumps({"spu_fma": True}))
    for i, st in enumerate(fr.STEPS):
        write(fr.marker_path(c, i, st), "{}")
    check(fr.cpu_changed(c, fr.STEPS), "CPU answer changed since the build -> True")
    left = {st.name for i, st in enumerate(fr.STEPS) if os.path.isfile(fr.marker_path(c, i, st))}
    check(left == {"check", "load", "inventory", "ppu-lift", "spu-lift"}, f"... configure/compile/stamp markers go {left}")
    check(not fr.cpu_changed(fr.Ctx(root=c.root, build=b, spu_fma=True), fr.STEPS), "same answer -> nothing")
    freed = fr.cleanup_build(c, fr.STEPS)
    check(freed == sum(sizes.values()), f"cleanup frees the four folders' bytes ({freed})")
    check(not any(os.path.exists(os.path.join(b, d)) for d in fr.CLEAN_DIRS), "obj, recomp, spu, analysis gone")
    check(all(os.path.isfile(os.path.join(b, r)) for r in ("kz3.exe", "kz3.ini", "paths.json", "fonts/R.ttf",
                                                           "cache/edat/x", "logs/l.txt", "build-stamp.json")),
          "kz3.exe, kz3.ini, paths.json, fonts, cache, logs, the stamp kept")
    left = {st.name for i, st in enumerate(fr.STEPS) if os.path.isfile(fr.marker_path(c, i, st))}
    check(left == {"check"}, f"markers of the removed outputs gone (a later rebuild is a full one) {left}")
    check("cleaned" in json.load(open(os.path.join(b, "build-stamp.json"))), "the stamp records the cleanup")
    os.remove(os.path.join(b, "kz3.exe"))
    check(fr.cleanup_build(c, fr.STEPS) == 0, "no kz3.exe -> nothing deleted")

    # Firmware fonts: has_fonts, the RPCS3 guess from the update folder.
    r3 = os.path.join(tmp, "rpcs3")
    upd = os.path.join(r3, "dev_hdd0", "game", "BCUS98234")
    os.makedirs(upd)
    check(fr.font_guess(upd) == "", "no fonts -> no guess")
    write(os.path.join(r3, "dev_flash", "data", "font", "SCE-PS3-RD-R-LATIN.TTF"), "f")
    check(fr.font_guess(upd) == os.path.join(r3, "dev_flash", "data", "font"), "RPCS3 dev_flash/data/font guessed")


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-ini-")
    try:
        c_half(tmp)
        cpu_half(tmp)
        build_half(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_kz3_ini:", "FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
