#!/usr/bin/env python3
"""Host test for step 9s (release pass 29): the player's EBOOT.elf linked into kz3.exe and the copied PS3 fonts.

C half: port/kz3/kz3_embed.h (SHA-256 + the known EBOOT hashes) and port/kz3/kz3_eboot_blob.c (the .incbin object)
compiled with the host C compiler: the SHA-256 against hashlib at the padding edges, a random blob linked in through
a generated kz3_eboot_embed.h (the way port/kz3/CMakeLists.txt writes it) read back and hashed, the same TU without
the header (no EBOOT built in -> NULL), and the header's known hashes equal tools/release/eboot_sha256.json.
Python half: firstrun's -DKZ3_EMBED_EBOOT (only when the payload has kz3_eboot_blob.c), build-stamp's
eboot_embedded, the launcher's kz3.ini without an eboot line for such a build, the font copy (copy_ps3_fonts,
place_ps3_fonts, --font-copy no, --font-dir auto). No game data, no network. The C half says SKIP without a host
cc, or when the runtime files are not in this tree (KZ3_EMBED_SRC=<dir with kz3_embed.h> points it elsewhere).
Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_embed_eboot.py
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import firstrun as fr  # noqa: E402
import launch as la  # noqa: E402

FAILS = []
EXE = ".exe" if os.name == "nt" else ""

TEST_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include "kz3_embed.h"
int main(int argc, char** argv)
{
    char hex[65];
    if (argc == 3 && argv[1][0] == 'h') {          /* h FILE: SHA-256 of a file */
        FILE* f = fopen(argv[2], "rb");
        if (!f) return 2;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char* b = malloc(n ? (size_t)n : 1);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) return 3;
        fclose(f);
        kz3_sha256_hex(b, (size_t)n, hex);
        printf("%s\n", hex);
        return 0;
    }
    const unsigned char* p; size_t n;              /* e: the built-in blob */
    const char* want = kz3_embedded_eboot(&p, &n);
    if (!want) { printf("none\n"); return 0; }
    kz3_sha256_hex(p, n, hex);
    printf("%zu %s %s %s\n", n, hex, want, kz3_eboot_version_of(hex));
    return 0;
}
"""


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def write(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def c_half(tmp):
    src = os.environ.get("KZ3_EMBED_SRC") or os.path.join(REPO, "port", "kz3")
    if not os.path.isfile(os.path.join(src, "kz3_embed.h")):
        print(f"SKIP the C half: no kz3_embed.h in {src} (the runtime half of step 9s is not in this tree)")
        return
    cc = next((shutil.which(c) for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if not cc:
        print("SKIP the C half: no host C compiler")
        return
    with open(os.path.join(src, "kz3_embed.h"), encoding="utf-8") as f:
        head = f.read()
    with open(os.path.join(HERE, "eboot_sha256.json"), encoding="utf-8") as f:
        known = json.load(f)
    for ver in ("1.00", "1.14"):
        check(f'{{"{ver}", "{known[ver]["sha256"]}"}}' in head, f"kz3_embed.h knows {ver} as eboot_sha256.json does")
    main_c = os.path.join(tmp, "t.c")
    write(main_c, TEST_C)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", src]
    plain = os.path.join(tmp, "plain" + EXE)
    r = subprocess.run([cc, *flags, main_c, os.path.join(src, "kz3_eboot_blob.c"), "-o", plain],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"kz3_embed.h + kz3_eboot_blob.c compile warning-free, no EBOOT built in {r.stderr[-300:]}")
    if r.returncode:
        return
    out = subprocess.run([plain, "e"], capture_output=True, text=True).stdout.strip()
    check(out == "none", f"without kz3_eboot_embed.h kz3_embedded_eboot returns NULL ({out})")
    bad = []
    for n in (0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 1000, 100003):
        data = os.urandom(n)
        p = os.path.join(tmp, f"d{n}")
        write(p, data)
        got = subprocess.run([plain, "h", p], capture_output=True, text=True).stdout.strip()
        if got != hashlib.sha256(data).hexdigest():
            bad.append(n)
    check(not bad, f"kz3_sha256_hex equals hashlib at 13 lengths around the padding edges (wrong: {bad})")
    # The blob as CMake writes it: a path with a space, forward slashes, the configure-time hash
    blob = os.urandom(3 * 1024 * 1024 + 17)
    bp = os.path.join(tmp, "dir with space", "EBOOT.elf")
    write(bp, blob)
    gen = os.path.join(tmp, "gen")
    want = hashlib.sha256(blob).hexdigest()
    write(os.path.join(gen, "kz3_eboot_embed.h"),
          f'#define KZ3_EMBED_EBOOT_FILE "{bp.replace(os.sep, "/")}"\n#define KZ3_EMBED_EBOOT_SHA256 "{want}"\n')
    emb = os.path.join(tmp, "emb" + EXE)
    r = subprocess.run([cc, *flags, "-I", gen, main_c, os.path.join(src, "kz3_eboot_blob.c"), "-o", emb],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"the .incbin object compiles and links {r.stderr[-300:]}")
    if r.returncode:
        return
    out = subprocess.run([emb, "e"], capture_output=True, text=True).stdout.split()
    check(out[:3] == [str(len(blob)), want, want], f"the built-in bytes read back whole and hash as recorded ({out[:2]})")
    check(out[3:] == ["unknown"], "a blob that is no known EBOOT is named unknown")
    check(os.path.getsize(emb) > len(blob) > os.path.getsize(plain), "the bytes are in the executable")


def py_half(tmp):
    root = os.path.join(tmp, "inst")
    build = os.path.join(root, "build")
    eboot = os.path.join(tmp, "upd", "USRDIR", "EBOOT.elf")
    write(eboot, b"\x7fELF" + os.urandom(1000))
    c = fr.Ctx(root=root, build=build, dump=os.path.join(tmp, "dump"), update=os.path.join(tmp, "upd"), eboot=eboot)
    has = lambda: any(x.startswith("-DKZ3_EMBED_EBOOT=") for x in fr.cmd_configure(c)[0])  # noqa: E731
    check(not fr.embed_supported(root) and not has(), "a payload without kz3_eboot_blob.c: no -DKZ3_EMBED_EBOOT")
    write(os.path.join(root, "port", "kz3", "kz3_eboot_blob.c"), "/* */\n")
    check(fr.embed_supported(root) and has(), "a payload with kz3_eboot_blob.c: -DKZ3_EMBED_EBOOT=<the lifted EBOOT>")
    check(fr.file_sha256(eboot) == hashlib.sha256(open(eboot, "rb").read()).hexdigest() and fr.file_sha256("") == "",
          "file_sha256 (build-stamp eboot_embedded)")
    paths = {"dump": c.dump, "update": c.update, "eboot": eboot}
    v = la.game_ini(root, paths, False, {}, appdata=os.path.join(tmp, "appdata"), build=build)
    check(v["eboot"] == eboot, "kz3.ini keeps the eboot line for an exe built before 9s (no eboot_embedded)")
    write(os.path.join(build, "build-stamp.json"), json.dumps({"eboot_embedded": fr.file_sha256(eboot)}))
    v = la.game_ini(root, paths, False, {}, appdata=os.path.join(tmp, "appdata"), build=build)
    check(v["eboot"] == "", "an exe with the EBOOT built in gets no eboot value")
    p = la.write_game_ini(root, v, build=build)
    text = open(p, encoding="utf-8").read()
    check("\neboot" not in text and "vfs_root = " in text, "... and kz3.ini has no eboot line, the other keys stay")

    # Fonts (step 9s): the chosen already-extracted folder copied into <build>/fonts/ps3
    src = os.path.join(tmp, "rpcs3", "dev_flash", "data", "font")
    for n in ("SCE-PS3-RD-R-LATIN.TTF", "SCE-PS3-NR-R-JPN.TTF", "notes.txt"):
        write(os.path.join(src, n), os.urandom(64))
    write(os.path.join(src, "sub", "SCE-PS3-X.TTF"), b"x")
    c.font_dir = src
    got = fr.place_ps3_fonts(c)
    dest = fr.ps3_font_dest(build)
    check(got == dest and sorted(os.listdir(dest)) == ["SCE-PS3-NR-R-JPN.TTF", "SCE-PS3-RD-R-LATIN.TTF"],
          f"the font files (top level only) are copied into build/fonts/ps3 ({sorted(os.listdir(dest))})")
    check(c.font_src == src, "the original folder is recorded as font_src")
    os.remove(os.path.join(src, "SCE-PS3-NR-R-JPN.TTF"))
    c.font_dir = src
    check(fr.copy_ps3_fonts(src, dest) == (0, 1) and os.listdir(dest) == ["SCE-PS3-RD-R-LATIN.TTF"],
          "a second copy mirrors: unchanged files stay, a file gone from the source goes")
    c.font_dir = dest
    check(fr.place_ps3_fonts(c) == dest, "a resume that reads the copy back leaves it as is")
    c.font_dir, c.font_copy, c.font_src = src, False, ""
    check(fr.place_ps3_fonts(c) == src, "--font-copy no: kz3.ini points at the original folder")
    c.font_dir = ""
    check(fr.place_ps3_fonts(c) == "", "no font folder: nothing copied")
    upd = os.path.join(tmp, "rpcs3", "dev_hdd0", "game", "BCUS98234")
    os.makedirs(upd)
    out = fr.main(["--dump", c.dump, "--update", upd, "--root", root, "--font-dir", "auto", "--dry-run"],
                  out=open(os.devnull, "w"))
    check(out == 0, "--font-dir auto is accepted (dry run)")
    check(fr.font_guess(upd) == src, "auto = RPCS3's dev_flash/data/font beside the update")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        c_half(os.path.join(tmp, "c"))
        py_half(os.path.join(tmp, "py"))
    print("FAIL" if FAILS else "PASS", f"({len(FAILS)} failed)" if FAILS else "")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
