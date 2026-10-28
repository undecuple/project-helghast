#!/usr/bin/env python3
"""Focused test for ps3recomp patch 3010 (cellSaveData writes PARAM.SFO as a binary PSF, reads PSF and the old text file).

Extracts write_param_sfo / read_param_sfo from libs/system/cellSaveData.c of upstream (third_party/ps3recomp) and of the
tree under test (default work/ps3recomp-patched; OLD_TREE / NEW_TREE override), compiles them against stub types and runs
them in a temp directory. An independent Python PSF reader (layout from the PARAM.SFO page of the PS3 developer wiki,
not from the C code) checks every field of the file the tree writes: magic/version, table offsets, alphabetical keys,
4-byte alignment, formats, len/max_len, values. Also: reading the PSF back, reading an old text-format file (existing saves),
and that the next save rewrites it as PSF. Upstream writes text and the reader rejects it (the bug). Needs cc.
Prints one PASS/FAIL line; SKIP if the tree has no kz3_psf.h (patch 3010 not applied)."""
import os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
typedef uint32_t u32; typedef int32_t s32;
#define CELL_SAVEDATA_SYSP_TITLE_SIZE 128
#define CELL_SAVEDATA_SYSP_SUBTITLE_SIZE 128
#define CELL_SAVEDATA_SYSP_DETAIL_SIZE 1024
#define CELL_SAVEDATA_SYSP_LPARAM_SIZE 8
typedef struct { char title[128]; char subTitle[128]; char detail[1024]; u32 attribute; char reserved2[4];
                 char listParam[8]; char reserved[256]; } CellSaveDataSystemFileParam;
%s
%s
int main(int argc, char** argv) {
    CellSaveDataSystemFileParam p; memset(&p, 0, sizeof p);
    if (!strcmp(argv[1], "write")) {
        snprintf(p.title, sizeof p.title, "%%s", argv[3]); snprintf(p.subTitle, sizeof p.subTitle, "%%s", argv[4]);
        snprintf(p.detail, sizeof p.detail, "%%s", argv[5]); p.attribute = (u32)strtoul(argv[6], 0, 0);
        snprintf(p.listParam, sizeof p.listParam, "%%s", argv[7]);
        write_param_sfo(argv[2], &p); return 0;
    }
    if (!strcmp(argv[1], "cut")) {   /* parse the first N bytes of the file, placed so they end at a PROT_NONE page */
        FILE* f = fopen(argv[2], "rb"); static uint8_t all[8192]; size_t got = fread(all, 1, sizeof all, f); fclose(f);
        size_t n = (size_t)strtoul(argv[3], 0, 0); if (n > got) n = got;
        size_t pg = (size_t)sysconf(_SC_PAGESIZE), span = (n + pg - 1) / pg * pg;
        uint8_t* m = mmap(0, span + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(m + span, pg, PROT_NONE);   /* any read past the buffer faults */
        uint8_t* b = m + span - n; memcpy(b, all, n);
        char t[128], s2[128], d[1024], l[8]; t[0] = s2[0] = d[0] = l[0] = 0; uint32_t at = 0;
#ifdef KZ3_PSF_H
        int r = kz3_psf_parse(b, n, t, sizeof t, s2, sizeof s2, d, sizeof d, &at, l, sizeof l);
#else
        int r = 1;
#endif
        printf("%%d\n", r); munmap(m, span + pg); return 0;
    }
    read_param_sfo(argv[2], &p);
    printf("%%s|%%s|%%s|%%u|%%s\n", p.title, p.subTitle, p.detail, p.attribute, p.listParam);
    return 0;
}
'''

def build(tree, d, unbounded=False):
    src = open(os.path.join(tree, "libs/system/cellSaveData.c")).read()
    parts = [X.function(src, r"static void %s\(const char\* save_path, [^)]*\)\n\{" % n) for n in ("write_param_sfo", "read_param_sfo")]
    hp = os.path.join(tree, "libs/system/kz3_psf.h")
    if unbounded:   # negative control: the parser without the NUL-bounds check
        h = "\n".join(l for l in open(hp).read().split("\n") if "memchr" not in l)
        hp = f"{d}/kz3_psf_unbounded.h"; open(hp, "w").write(h)
    inc = f'#include "{os.path.abspath(hp)}"\n' if os.path.exists(hp) else ""
    code = inc + HARNESS % ("", "\n".join(parts))
    code = code.replace('#include <stdint.h>', '#include <stdint.h>\n#include <stddef.h>', 1)
    open(f"{d}/h.c", "w").write(code)
    r = subprocess.run(["cc", "-O0", "-w", f"{d}/h.c", "-o", f"{d}/h"], capture_output=True, text=True)
    if r.returncode: sys.exit("compile error: " + r.stderr[:600])
    return f"{d}/h"

def read_psf(b):
    """Independent PSF reader. Returns {key: (fmt, len, max_len, bytes)} or raises ValueError."""
    if b[:4] != b"\0PSF": raise ValueError("not a PSF (magic)")
    if b[4:8] != b"\x01\x01\x00\x00": raise ValueError("version not 1.1")
    kt, dt, n = struct.unpack_from("<III", b, 8)
    if kt != 0x14 + 16 * n: raise ValueError("key table not right after the index")
    if dt % 4 or dt < kt: raise ValueError("data table misaligned")
    out, keys = {}, []
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", b, 0x14 + 16 * i)
        k = b[kt + ko:b.index(b"\0", kt + ko)].decode()
        keys.append(k)
        if mx % 4 or ln > mx: raise ValueError(f"{k}: bad len/max_len {ln}/{mx}")
        out[k] = (fmt, ln, mx, b[dt + do:dt + do + mx])
    if keys != sorted(keys): raise ValueError("keys not alphabetical")
    return out

def check(tree, unbounded=False):
    """Returns (ok, why) for the tree's writer and reader."""
    with tempfile.TemporaryDirectory() as d:
        exe = build(tree, d, unbounded)
        save = f"{d}/BCUS98234-SAVE/GAME0"; os.makedirs(save)
        title, sub, detail, lp = "Killzone 3", "Chapter 2 é", "Checkpoint 7\nline two", "CONFIG"
        subprocess.run([exe, "write", save, title, sub, detail, "0x20", lp], check=True)
        raw = open(f"{save}/PARAM.SFO", "rb").read()
        try: f = read_psf(raw)
        except ValueError as e: return False, f"writer: {e}"
        want_keys = ["ACCOUNT_ID", "ATTRIBUTE", "CATEGORY", "DETAIL", "PARAMS", "PARAMS2", "PARENTAL_LEVEL",
                     "SAVEDATA_DIRECTORY", "SAVEDATA_LIST_PARAM", "SUB_TITLE", "TITLE"]
        if sorted(f) != want_keys: return False, f"writer: keys {sorted(f)}"
        def txt(k, maxlen):
            fmt, ln, mx, data = f[k]
            if fmt != 0x0204 or mx != maxlen or data[ln - 1] != 0 or ln > mx: raise ValueError(f"{k} fmt/len {fmt:#x} {ln}/{mx}")
            return data[:ln - 1].decode()
        def i32(k):
            fmt, ln, mx, data = f[k]
            if (fmt, ln, mx) != (0x0404, 4, 4): raise ValueError(f"{k} fmt/len")
            return struct.unpack("<I", data[:4])[0]
        def raws(k, n):
            fmt, ln, mx, data = f[k]
            if (fmt, ln, mx) != (0x0004, n, n): raise ValueError(f"{k} fmt/len {fmt:#x} {ln}/{mx}")
            return data
        try:
            assert txt("CATEGORY", 4) == "SD"
            assert txt("TITLE", 128) == title and txt("SUB_TITLE", 128) == sub and txt("DETAIL", 1024) == detail
            assert txt("SAVEDATA_LIST_PARAM", 8) == lp and txt("SAVEDATA_DIRECTORY", 64) == "GAME0"
            assert i32("ATTRIBUTE") == 0x20 and i32("PARENTAL_LEVEL") == 0
            assert len(raws("ACCOUNT_ID", 16)) == 16 and len(raws("PARAMS", 1024)) == 1024 and len(raws("PARAMS2", 12)) == 12
        except (AssertionError, ValueError) as e: return False, f"writer field check: {e!r}"
        if len(raw) != struct.unpack_from("<I", raw, 12)[0] + sum(v[2] for v in f.values()): return False, "writer: size"
        rd = subprocess.run([exe, "read", save], capture_output=True, text=True, check=True).stdout
        if rd != f"{title}|{sub}|{detail}|32|{lp}\n": return False, f"reader on PSF: {rd!r}"
        # old text format: still loads, and the next save turns it into a PSF
        old = f"{d}/old"; os.makedirs(old)
        open(f"{old}/PARAM.SFO", "w").write("TITLE=Old T\nSUB_TITLE=Old S\nDETAIL=Old D\nATTRIBUTE=5\nLIST_PARAM=LP\n")
        rd = subprocess.run([exe, "read", old], capture_output=True, text=True, check=True).stdout
        if rd != "Old T|Old S|Old D|5|LP\n": return False, f"reader on old text: {rd!r}"
        subprocess.run([exe, "write", old, "Old T", "Old S", "Old D", "5", "LP"], check=True)
        try: read_psf(open(f"{old}/PARAM.SFO", "rb").read())
        except ValueError as e: return False, f"rewrite of old file: {e}"
        # corrupt file: the key table starts 3 bytes before the end, so the key has no NUL inside the buffer. The buffer
        # ends at a PROT_NONE page, so an unbounded strcmp faults (no sanitizer needed: runs in every dev shell).
        bad = bytearray(raw); bad[-3:] = b"TIT"; struct.pack_into("<I", bad, 8, len(bad) - 3)
        open(f"{d}/bad.sfo", "wb").write(bad)
        r = subprocess.run([exe, "cut", f"{d}/bad.sfo", str(len(bad))], capture_output=True, text=True)
        if r.returncode or r.stdout.strip() != "1": return False, f"key without NUL: rc={r.returncode} {r.stdout.strip()!r} {r.stderr[:120]!r}"
        # overlong text is cut to max_len-1 with its NUL
        subprocess.run([exe, "write", save, "T" * 127, "S", "D", "0", "L" * 7], check=True)
        f = read_psf(open(f"{save}/PARAM.SFO", "rb").read())
        if f["SAVEDATA_LIST_PARAM"][1] != 8 or f["TITLE"][1] != 128: return False, "max-length cut"
    return True, ""

old_t, new_t = X.trees()
if not os.path.exists(os.path.join(new_t, "libs/system/kz3_psf.h")):
    print("SKIP: tree under test has no kz3_psf.h (patch 3010 not applied)"); sys.exit(0)
ok_new, why_new = check(new_t)
ok_old, why_old = check(old_t)
ok_ctl, why_ctl = check(new_t, unbounded=True)
print(f"negative control (parser without the bounds check): {'ok' if ok_ctl else why_ctl}")
print(f"tree under test: {'ok' if ok_new else why_new}; upstream: {'ok' if ok_old else why_old}")
X.verdict("3010 savedata PARAM.SFO is a binary PSF", not ok_old, ok_new, not ok_old and not ok_ctl)
