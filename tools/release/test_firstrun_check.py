#!/usr/bin/env python3
"""Host test for tools/release/firstrun_check.py on synthetic fixtures (no game data: a temp dir holds a made-up
PARAM.SFO, a 4-byte 'SCE' EBOOT.BIN, a 'PSAR' header and a 64-byte PPC64 ELF header). Exit 0 = PASS, 1 = FAIL.

    python3 tools/release/test_firstrun_check.py

Cases: a complete dump + update + 1.14-sized ELF passes; wrong TITLE_ID fails; missing PARAM.SFO fails; a dump
whose EBOOT.BIN is already an ELF passes; a v1.00-sized ELF fails E1 on the 1.14 build; a wrong-machine ELF fails; a truncated psarc
fails; no --update warns; --json has the documented shape; H1 (EBOOT SHA-256): the shipped hashes file
with empty values warns "unknown yet", a matching hash is OK, a mismatch warns with the size and both hashes (exit 0),
an unreadable hashes file warns, a failed E1 gives no H1.
"""
import json
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firstrun_check as fc  # noqa: E402

FAILS = []


def write_sfo(path, fields):
    """Minimal PARAM.SFO writer (format 0x204 utf8 strings, 0x404 ints), the inverse of fc.read_sfo."""
    keys = b"".join(k.encode() + b"\0" for k in fields)
    data, idx, doff = b"", b"", 0
    koff = 0
    for k, v in fields.items():
        if isinstance(v, int):
            raw, fmt, ln = struct.pack("<I", v), 0x404, 4
        else:
            raw = v.encode() + b"\0"
            fmt, ln = 0x204, len(raw)
        padded = raw + b"\0" * ((4 - len(raw) % 4) % 4)
        idx += struct.pack("<HHIII", koff, fmt, ln, len(padded), doff)
        koff += len(k) + 1
        data += padded
        doff += len(padded)
    kt = 20 + len(idx)
    dt = kt + len(keys)
    dt += (4 - dt % 4) % 4
    hdr = struct.pack("<4sIIII", b"\0PSF", 0x101, kt, dt, len(fields))
    blob = hdr + idx + keys
    blob += b"\0" * (dt - len(blob)) + data
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(blob)


def write_elf(path, size, machine=fc.EM_PPC64, cls=2, data=2):
    h = bytearray(64)
    h[:4] = fc.ELF_MAGIC
    h[4], h[5], h[6] = cls, data, 1
    h[18:20] = struct.pack(">H" if data == 2 else "<H", machine)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(h)
        f.truncate(size)


def make_dump(root, title_id="BCUS98234", eboot_magic=fc.SELF_MAGIC, psarc_bytes=4096):
    d = os.path.join(root, "dump")
    write_sfo(os.path.join(d, "PS3_GAME", "PARAM.SFO"),
              {"APP_VER": "01.00", "CATEGORY": "DG", "TITLE": "Synthetic", "TITLE_ID": title_id, "VERSION": "01.01"})
    os.makedirs(os.path.join(d, "PS3_GAME", "USRDIR"))
    if eboot_magic == fc.ELF_MAGIC:
        write_elf(os.path.join(d, "PS3_GAME", "USRDIR", "EBOOT.BIN"), 64)
    else:
        open(os.path.join(d, "PS3_GAME", "USRDIR", "EBOOT.BIN"), "wb").write(eboot_magic + b"\0" * 60)
    with open(os.path.join(d, "PS3_GAME", "USRDIR", "data_ps3.psarc"), "wb") as f:
        f.write(fc.PSARC_MAGIC)
        f.truncate(psarc_bytes)
    return d


def add_update_edat(upd):
    """The update's data archive (U3): every fixture that models a complete update calls this."""
    open(os.path.join(upd, "USRDIR", fc.UPDATE_EDAT), "wb").write(b"NPD\0" + b"\0" * 60)


def make_update(root, app_ver="01.14", elf_size=21_228_256, title_id="BCUS98234"):
    u = os.path.join(root, "hdd0", "game", title_id)
    write_sfo(os.path.join(u, "PARAM.SFO"),
              {"APP_VER": app_ver, "CATEGORY": "GD", "TITLE": "Synthetic", "TITLE_ID": title_id})
    os.makedirs(os.path.join(u, "USRDIR"))
    open(os.path.join(u, "USRDIR", "EBOOT.BIN"), "wb").write(fc.SELF_MAGIC + b"\0" * 60)
    add_update_edat(u)
    write_elf(os.path.join(u, "USRDIR", "EBOOT.elf"), elf_size)
    return u


def run(args):
    """Run main() with captured stdout; returns (exit code, stdout)."""
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fc.main(args)
    return rc, buf.getvalue()


def check(name, cond, detail=""):
    if cond:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name} {detail}")
        FAILS.append(name)


def levels(out, code):
    return {ln.split()[0] for ln in out.splitlines() if ln.split()[1:2] == [code + ":"]}


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-firstrun-")
    try:
        small = ["--min-psarc-bytes", "1024", "--skip-archive-hash"]
        d = make_dump(tmp)
        u = make_update(tmp)
        rc, out = run(["--dump", d, "--update", u] + small)
        check("complete set passes", rc == 0 and "FAIL" not in out, out)
        check("1.14 ELF recognised", "1.14" in out and levels(out, "E1") == {"OK"}, out)
        check("update APP_VER ok", levels(out, "U1") == {"OK"}, out)

        rc, out = run(["--dump", d, "--update", u, "--json"] + small)
        j = json.loads(out)
        check("json shape", j["ok"] is True and {"code", "level", "message"} <= set(j["checks"][0])
              and j["facts"]["eboot_elf"]["version"] == "1.14" and j["facts"]["disc"]["TITLE_ID"] == "BCUS98234", out)

        # U3 (pass 66): the update without added_data_ps3.psarc.edat fails, naming the file and why; the installer's
        # update page requires the same file (test_install_update_page.py)
        u3 = make_update(os.path.join(tmp, "noedat"))
        os.remove(os.path.join(u3, "USRDIR", fc.UPDATE_EDAT))
        rc, out = run(["--dump", d, "--update", u3] + small)
        check("update without the data archive fails U3 naming the file", rc == 1 and "FAIL U3" in out
              and fc.UPDATE_EDAT in out and "menu screens" in out, out)
        check("update with the data archive passes U3", levels(run(["--dump", d, "--update", u] + small)[1], "U3") == {"OK"})

        rc, out = run(["--dump", d] + small)
        check("no --update warns", "WARN U0" in out, out)
        check("default ELF falls back to the dump (missing -> FAIL E1)", rc == 1 and "FAIL E1" in out, out)

        rc, out = run(["--dump", d, "--update", u, "--eboot", os.path.join(tmp, "nope.elf")] + small)
        check("missing --eboot fails", rc == 1 and "FAIL E1" in out, out)

        rc, out = run(["--dump", make_dump(os.path.join(tmp, "eu"), title_id="BCES01070"), "--update", u] + small)
        check("wrong TITLE_ID fails", rc == 1 and "FAIL D1" in out, out)

        check("other id names it and the four accepted ids", "BCES01070" in out and all(i in out for i in ("BCUS98234", "BCES01007", "BCAS25008", "BCJS30066")), out)

        eu = make_dump(os.path.join(tmp, "eu2"), title_id="BCES01007")
        eu_u = make_update(os.path.join(tmp, "eu2"), title_id="BCES01007")
        rc, out = run(["--dump", eu, "--update", eu_u] + small)
        check("EU disc + EU update passes with the untested-region note",
              rc == 0 and "WARN D1: disc dump: EU disc (BCES01007): untested region; a player's report shows the same 1.14 EBOOT hash as the US version" in out
              and "WARN U1" in out and "FAIL" not in out, out)
        mix = "update from another region (%s) than the disc (%s): it gave the same game data in our tests (US and EU discs)"
        rc, out = run(["--dump", eu, "--update", u] + small)
        check("EU disc + US update warns U1", rc == 0 and "WARN U1" in out and "FAIL" not in out
              and mix % ("BCUS98234", "BCES01007") in out, out)
        rc, out = run(["--dump", d, "--update", eu_u] + small)
        check("US disc + EU update warns U1", rc == 0 and "WARN U1" in out and "FAIL" not in out
              and mix % ("BCES01007", "BCUS98234") in out, out)
        for tid, name in (("BCAS25008", "Asian disc"), ("BCJS30066", "Japanese disc")):
            rd = make_dump(os.path.join(tmp, "r" + tid), title_id=tid)
            ru = make_update(os.path.join(tmp, "r" + tid), title_id=tid)
            rc, out = run(["--dump", rd, "--update", ru] + small)
            check(name + " + matching update passes with the untested-region note",
                  rc == 0 and f"WARN D1: disc dump: {name} ({tid}): untested region; its 1.14 update carries the same EBOOT as the US one (checked on our copy)" in out
                  and "WARN U1" in out and "FAIL" not in out, out)
        jp = rd   # the JP dump from the loop
        rc, out = run(["--dump", jp, "--update", eu_u] + small)
        check("JP disc + EU update warns U1 naming both", rc == 0 and "FAIL" not in out
              and mix % ("BCES01007", "BCJS30066") in out, out)
        odd = make_update(os.path.join(tmp, "odd"), title_id="BCES01070")
        rc, out = run(["--dump", jp, "--update", odd] + small)
        check("update with an unknown id fails U1 naming the four ids", rc == 1 and "FAIL U1" in out and "BCES01070" in out
              and all(i in out for i in ("BCUS98234", "BCES01007", "BCAS25008", "BCJS30066")), out)

        # A1: the archive's SHA-256 against a fake known one (psarc_sha256.json format), injected with --psarc-hashes
        import hashlib
        def psarc_of(dump):
            return os.path.join(dump, "PS3_GAME", "USRDIR", "data_ps3.psarc")
        known = os.path.join(tmp, "known.json")
        with open(known, "w") as f:
            json.dump({"sha256": hashlib.sha256(open(psarc_of(d), "rb").read()).hexdigest(), "size": 4096,
                       "discs": ["BCUS98234", "BCES01007"]}, f)
        a1 = ["--min-psarc-bytes", "1024", "--psarc-hashes", known, "--update", u]
        rc, out = run(["--dump", d] + a1)
        check("A1 match is OK", rc == 0 and "OK   A1: data_ps3.psarc matches the known disc archive" in out, out)
        dmg = make_dump(os.path.join(tmp, "dmg"))
        with open(psarc_of(dmg), "r+b") as f:
            f.seek(100)
            f.write(b"x")
        rc, out = run(["--dump", dmg] + a1)
        check("A1 mismatch on a US disc is a FAIL", rc == 1 and "FAIL A1: data_ps3.psarc differs from the known disc archive (damaged or modified copy): copy or dump the disc again" in out, out)
        short = make_dump(os.path.join(tmp, "short"), psarc_bytes=8192)
        rc, out = run(["--dump", short] + a1)
        check("A1 different size is a FAIL too", rc == 1 and "FAIL A1" in out, out)
        eu_dmg = make_dump(os.path.join(tmp, "eudmg"), title_id="BCES01007")
        with open(psarc_of(eu_dmg), "r+b") as f:
            f.seek(100)
            f.write(b"x")
        rc, out = run(["--dump", eu_dmg, "--update", make_update(os.path.join(tmp, "eudmg"), title_id="BCES01007")] + a1[:-2])
        check("A1 mismatch on an EU disc is a FAIL", rc == 1 and "FAIL A1" in out, out)
        asia = make_dump(os.path.join(tmp, "asia"), title_id="BCAS25008")
        with open(psarc_of(asia), "r+b") as f:
            f.seek(100)
            f.write(b"x")
        rc, out = run(["--dump", asia, "--update", make_update(os.path.join(tmp, "asia"), title_id="BCAS25008")] + a1[:-2])
        check("A1 mismatch on an Asian disc is only a WARN (untested region)", rc == 0 and "WARN A1: data_ps3.psarc is not a known archive yet (untested region" in out and "FAIL" not in out, out)
        # the cache: a second check reads nothing; a changed file is read again
        calls = []
        real = fc.hash_file
        fc.hash_file = lambda p, progress=None: (calls.append(p), real(p, progress))[1]
        try:
            cdir = os.path.join(tmp, "cachedir")
            for n in (1, 2):
                rep = fc.Report()
                fc.check_psarc_hash(rep, psarc_of(d), "BCUS98234", known, cdir)
                check(f"A1 run {n} is OK", [c["level"] for c in rep.checks] == ["OK"], rep.checks)
            check("A1 cache hit: the file was read once", len(calls) == 1, calls)
            os.utime(psarc_of(d), ns=(1_000_000_000, 1_000_000_000))
            rep = fc.Report()
            fc.check_psarc_hash(rep, psarc_of(d), "BCUS98234", known, cdir)
            check("A1 changed mtime: read again", len(calls) == 2, calls)
            seen = []
            fc.check_psarc_hash(fc.Report(), psarc_of(dmg), "BCUS98234", known, None, lambda dn, t: seen.append((dn, t)))
            check("A1 progress reports done/total up to the end", seen and seen[-1] == (4096, 4096), seen)
        finally:
            fc.hash_file = real

        # FAT32 split parts (public issue 4): data_ps3.psarc.66600, .66601, ... instead of data_ps3.psarc
        def split_dump(name, sizes, skip=()):
            sd = make_dump(os.path.join(tmp, name))
            data = open(psarc_of(sd), "rb").read()
            os.remove(psarc_of(sd))
            off = 0
            for i, n in enumerate(sizes):
                if i not in skip:
                    with open(f"{psarc_of(sd)}.{66600 + i}", "wb") as f:
                        f.write(data[off:off + n])
                off += n
            return sd
        sp = split_dump("split4", [1024, 1024, 1024, 1024])
        rc, out = run(["--dump", sp] + a1)
        check("split parts: D3 OK with the total size, A1 matches the hash of the joined parts",
              rc == 0 and "OK   D3: data_ps3.psarc present" in out and "4 split parts" in out
              and "OK   A1: data_ps3.psarc matches the known disc archive" in out, out)
        sp2 = split_dump("split3", [2000, 2000, 96])        # uneven part sizes: nothing assumes a part size
        rc, out = run(["--dump", sp2] + a1)
        check("split parts of uneven sizes: A1 OK", rc == 0 and "OK   A1: data_ps3.psarc matches" in out, out)
        cdir2 = os.path.join(tmp, "splitcache")
        calls2 = []
        real2 = fc.hash_file
        fc.hash_file = lambda p, progress=None: (calls2.append(p), real2(p, progress))[1]
        try:
            for n in (1, 2):
                rep = fc.Report()
                fc.check_psarc_hash(rep, psarc_of(sp), "BCUS98234", known, cdir2)
                check(f"split parts: A1 run {n} OK", [c["level"] for c in rep.checks] == ["OK"], rep.checks)
            check("split parts: cache hit, the parts were read once", len(calls2) == 1 and isinstance(calls2[0], list), calls2)
            os.utime(f"{psarc_of(sp)}.66602", ns=(1_000_000_000, 1_000_000_000))      # any one part's mtime
            fc.check_psarc_hash(fc.Report(), psarc_of(sp), "BCUS98234", known, cdir2)
            check("split parts: one part's changed mtime reads again", len(calls2) == 2, calls2)
        finally:
            fc.hash_file = real2
        seen = []
        fc.check_psarc_hash(fc.Report(), psarc_of(sp), "BCUS98234", known, None, lambda dn, t: seen.append((dn, t)))
        check("split parts: progress runs over the total", seen and seen[-1] == (4096, 4096), seen[-1:])
        with open(f"{psarc_of(sp)}.66601", "r+b") as f:
            f.seek(5)
            f.write(b"x")
        rep = fc.Report()
        fc.check_psarc_hash(rep, psarc_of(sp), "BCUS98234", known, None)
        check("split parts: a damaged middle part fails A1", [c["level"] for c in rep.checks] == ["FAIL"], rep.checks)
        gap = split_dump("splitgap", [1024, 1024, 1024, 1024], skip=(2,))
        rc, out = run(["--dump", gap] + a1)
        check("split parts with a gap name the missing part", rc == 1 and "FAIL D3" in out and "data_ps3.psarc.66602 is missing" in out, out)
        late = split_dump("splitlate", [1024, 1024, 1024, 1024], skip=(0,))
        rc, out = run(["--dump", late] + a1)
        check("split parts without .66600 name it", rc == 1 and "data_ps3.psarc.66600 is missing" in out, out)
        short = split_dump("splitshort", [1024, 1024])
        rc, out = run(["--dump", short, "--min-psarc-bytes", "4096"] + a1[2:])
        check("split parts too small in total fail D3 on the total", rc == 1 and "FAIL D3" in out and "2,048 bytes" in out, out)
        nothing = make_dump(os.path.join(tmp, "nopsarc"))
        os.remove(psarc_of(nothing))
        rc, out = run(["--dump", nothing] + a1)
        check("neither file nor parts: D3 names data_ps3.psarc", rc == 1 and "no PS3_GAME/USRDIR/data_ps3.psarc" in out, out)
        check("game_files lists every part (online-only check)",
              [os.path.basename(x) for x in fc.game_files(sp, None, None) if "psarc" in x]
              == [f"data_ps3.psarc.{66600 + i}" for i in range(4)])

        bad = make_dump(os.path.join(tmp, "nosfo"))
        os.remove(os.path.join(bad, "PS3_GAME", "PARAM.SFO"))
        rc, out = run(["--dump", bad, "--update", u] + small)
        check("missing PARAM.SFO fails", rc == 1 and "FAIL D1" in out, out)

        dec = make_dump(os.path.join(tmp, "dec"), eboot_magic=fc.ELF_MAGIC)
        rc, out = run(["--dump", dec, "--update", u] + small)
        check("already-decrypted disc EBOOT passes", rc == 0 and "already a decrypted ELF" in out, out)
        rc, out = run(["--dump", dec] + small)
        check("decrypted EBOOT.BIN serves as the ELF when no EBOOT.elf exists", "FAIL E1" not in out
              and levels(out, "E1") == {"WARN"}, out)

        rc, out = run(["--dump", make_dump(os.path.join(tmp, "trunc"), psarc_bytes=512), "--update", u] + small)
        check("truncated psarc fails", rc == 1 and "FAIL D3" in out and "truncated" in out, out)

        u100 = make_update(os.path.join(tmp, "v100"), elf_size=20_745_952)
        rc, out = run(["--dump", d, "--update", u100] + small)
        check("v1.00 ELF on the 1.14 build fails E1 naming the update's USRDIR (issue #8)",
              rc == 1 and levels(out, "E1") == {"FAIL"} and "disc's 1.00 one" in out
              and os.path.join(u100, "USRDIR") in out and "not the disc's" in out, out)
        rc, out = run(["--dump", d, "--eboot", os.path.join(u100, "USRDIR", "EBOOT.elf")] + small)
        check("E1 without --update names the usual dev_hdd0 USRDIR", rc == 1 and
              os.path.join("dev_hdd0", "game", "BCUS98234", "USRDIR") in out, out)
        rc, out = run(["--dump", d, "--update", u100, "--target", "1.00"] + small)
        check("v1.00 ELF on a 1.00 tree (--target 1.00) is OK", rc == 0 and levels(out, "E1") == {"OK"}, out)
        rc, out = run(["--dump", d, "--update", u, "--target", "1.00"] + small)
        check("1.14 ELF on a 1.00 tree fails", rc == 1 and levels(out, "E1") == {"FAIL"} and "for the 1.00" in out, out)

        ux = make_update(os.path.join(tmp, "x86"), elf_size=21_228_256)
        write_elf(os.path.join(ux, "USRDIR", "EBOOT.elf"), 21_228_256, machine=0x3E, data=1)
        rc, out = run(["--dump", d, "--update", ux] + small)
        check("wrong-machine ELF fails", rc == 1 and "FAIL E1" in out, out)

        rc, out = run(["--dump", d, "--update", make_update(os.path.join(tmp, "v113"), app_ver="01.13")] + small)
        check("other APP_VER warns", rc == 0 and "WARN U1" in out, out)

        rc, out = run(["--dump", os.path.join(tmp, "missing"), "--update", u] + small)
        check("missing dump dir fails", rc == 1 and "FAIL D0" in out, out)

        # H1: EBOOT SHA-256 against tools/release/eboot_sha256.json (or --hashes)
        with open(fc.HASHES_FILE, encoding="utf-8") as f:
            shipped = json.load(f)
        check("shipped hashes file has 1.14 and 1.00 entries", {"1.14", "1.00"} <= set(shipped)
              and shipped["1.14"]["size"] == 21_228_256, shipped)
        elf = os.path.join(u, "USRDIR", "EBOOT.elf")
        got = fc.file_sha256(elf)
        hf = os.path.join(tmp, "hashes.json")

        def hashes(v):
            with open(hf, "w", encoding="utf-8") as f:
                json.dump({"1.14": {"size": 21_228_256, "sha256": v}}, f)
            return ["--hashes", hf]

        rc, out = run(["--dump", d, "--update", u] + small + hashes(""))
        check("H1 unknown yet warns only", rc == 0 and levels(out, "H1") == {"WARN"} and "unknown yet" in out
              and got in out, out)
        rc, out = run(["--dump", d, "--update", u] + small + hashes(got.upper()))
        check("H1 match is OK", rc == 0 and levels(out, "H1") == {"OK"}, out)
        rc, out = run(["--dump", d, "--update", u] + small + hashes("ab" * 32))
        check("H1 mismatch warns with size and both hashes", rc == 0 and levels(out, "H1") == {"WARN"}
              and got in out and "ab" * 32 in out and "21,228,256" in out, out)
        rc, out = run(["--dump", d, "--update", u, "--json"] + small + hashes(got))
        check("H1 hash in json facts", json.loads(out)["facts"]["eboot_elf"]["sha256"] == got, out)
        rc, out = run(["--dump", d, "--update", u] + small + ["--hashes", os.path.join(tmp, "nohashes.json")])
        check("H1 unreadable hashes file warns", rc == 0 and levels(out, "H1") == {"WARN"}, out)
        rc, out = run(["--dump", d, "--update", ux] + small + hashes(got))
        check("no H1 when E1 failed", "H1" not in out, out)

        # OneDrive (O1 install/build folder, O2 online-only files, O3 INFO): fake env vars and fake attributes
        env = {"OneDrive": "C:\\Users\\someone\\OneDrive", "OneDriveCommercial": "D:\\Work\\OneDrive - Corp"}
        check("O1 prefix match, case-insensitive",
              fc.onedrive_root_of("c:\\users\\SOMEONE\\onedrive\\Desktop\\Project Helghast", env) == env["OneDrive"]
              and fc.onedrive_root_of("D:/Work/OneDrive - Corp/x", env) == env["OneDriveCommercial"])
        check("O1 whole components only, empty env is nothing",
              fc.onedrive_root_of("C:\\Users\\someone\\OneDrive2\\x", env) == ""
              and fc.onedrive_root_of("C:\\Users\\someone\\Programs", {}) == "")
        r = fc.Report()
        fc.check_onedrive_tree(r, "C:\\Users\\someone\\OneDrive\\Desktop\\Project Helghast", "C:\\b\\build", env)
        check("O1 fails for a root inside OneDrive only", [(c["code"], c["level"]) for c in r.checks] == [("O1", "FAIL")]
              and "outside OneDrive" in r.checks[0]["message"], r.checks)
        r = fc.Report()
        fc.check_onedrive_tree(r, "C:\\ok\\Project Helghast", "D:\\Work\\OneDrive - Corp\\build", env)
        check("O1 fails for a build dir inside OneDrive", len(r.checks) == 1 and "build folder" in r.checks[0]["message"])
        old = os.environ.get("OneDrive")
        os.environ["OneDrive"] = os.path.join(tmp, "hdd0")      # the update folder sits inside the fake OneDrive
        real = fc.file_attributes
        try:
            rc, out = run(["--dump", d, "--update", u] + small + hashes(got))
            check("O3 INFO only when inside OneDrive but local", rc == 0 and levels(out, "O3") == {"OK"} and "INFO" in out
                  and "O2" not in out, out)
            fc.file_attributes = lambda p: 0x400000 if p.endswith("data_ps3.psarc") else 0
            rc, out = run(["--dump", d, "--update", u, "--json"] + small + hashes(got))
            j = json.loads(out)
            o2 = [c for c in j["checks"] if c["code"] == "O2"]
            check("O2 fails for an online-only psarc, with the fix, and reads nothing else",
                  rc == 1 and not j["ok"] and len(o2) == 1 and o2[0]["level"] == "FAIL"
                  and "Always keep on this device" in o2[0]["message"] and "data_ps3.psarc" in o2[0]["message"]
                  and not any(c["code"] in ("D1", "D3", "E1") for c in j["checks"]), out)
            for bit in (0x40000, 0x1000):
                fc.file_attributes = lambda p, b=bit: b if p.endswith("PARAM.SFO") else 0
                rc, out = run(["--dump", d, "--update", u] + small)
                check(f"O2 fails for attribute 0x{bit:x}", rc == 1 and levels(out, "O2") == {"FAIL"}, out)
        finally:
            fc.file_attributes = real
            if old is None:
                del os.environ["OneDrive"]
            else:
                os.environ["OneDrive"] = old
        # P1 (public issue 20): [ ] ; in the install or build folder
        for ch in "[];":
            r = fc.Report()
            fc.check_path_chars(r, "D:\\recomp" + ch + "\\Project Helghast", "D:\\ok\\build")
            check(f"P1 fails for {ch!r} in the install folder", [(c["code"], c["level"]) for c in r.checks] == [("P1", "FAIL")]
                  and "install folder" in r.checks[0]["message"] and "cannot" in r.checks[0]["message"])
        r = fc.Report()
        fc.check_path_chars(r, "D:\\ok\\Project Helghast", "D:\\b" + "[1]" + "\\build")
        check("P1 fails for a build folder with [1] and names it", len(r.checks) == 1 and "build folder" in r.checks[0]["message"])
        r = fc.Report()
        fc.check_path_chars(r, "D:\\Games (x86)\\Project Helghast", "")
        check("P1 silent for a fine path and an empty build", r.checks == [])
        rc, out = run(["--dump", d, "--update", u] + small + hashes(got) + ["--root", "/x[y/p", "--build", "/x/b"])
        check("--root with [ fails P1 end to end", rc == 1 and levels(out, "P1") == {"FAIL"}, out)
        rc, out = run(["--dump", d, "--update", u] + small + hashes(got) + ["--root", "/x/OneDrive/p", "--build", "/x/b"])
        check("--root without OneDrive env passes", rc == 0 and "O1" not in out, out)

        # decrypted ISO as --dump (iso_fixture.py builds tiny images): same D-codes from inside the image
        from iso_fixture import build_iso
        i = os.path.join(tmp, "iso")
        os.makedirs(i)
        sfo = os.path.join(i, "p.sfo")
        write_sfo(sfo, {"APP_VER": "01.00", "CATEGORY": "DG", "TITLE": "Synthetic", "TITLE_ID": "BCUS98234", "VERSION": "01.01"})
        sfo_bytes = open(sfo, "rb").read()
        files = {"PS3_DISC.SFB": b".SFB" + b"\0" * 8, "PS3_GAME/PARAM.SFO": sfo_bytes,
                 "PS3_GAME/USRDIR/EBOOT.BIN": fc.SELF_MAGIC + b"\0" * 60,
                 "PS3_GAME/USRDIR/data_ps3.psarc": fc.PSARC_MAGIC + b"\0" * 6000}
        iso = build_iso(os.path.join(i, "game.iso"), files, multi={"PS3_GAME/USRDIR/data_ps3.psarc": 2048})
        rc, out = run(["--dump", iso, "--update", u] + small + hashes(got))
        check("ISO: complete decrypted ISO passes", rc == 0 and "OK   D1:" in out and "OK   D3:" in out, out)
        check("ISO: INFO line names the image", "dump: decrypted ISO game.iso" in out, out)
        rc, out = run(["--dump", iso, "--update", u, "--json"] + small + hashes(got))
        check("ISO: facts read from inside", json.loads(out)["facts"]["disc"]["TITLE_ID"] == "BCUS98234", out)
        rc, out = run(["--dump", iso, "--update", u, "--min-psarc-bytes", "99999"] + hashes(got))
        check("ISO: small psarc fails D3", rc == 1 and "FAIL D3" in out, out)
        eu = dict(files)
        write_sfo(sfo, {"APP_VER": "01.00", "CATEGORY": "DG", "TITLE": "Synthetic", "TITLE_ID": "BCES01007"})
        eu["PS3_GAME/PARAM.SFO"] = open(sfo, "rb").read()
        isoeu = build_iso(os.path.join(i, "eu.iso"), eu)
        rc, out = run(["--dump", isoeu] + small + hashes(got))
        check("ISO: EU id warns D1", "WARN D1" in out and "EU disc" in out, out)
        write_sfo(sfo, {"APP_VER": "01.00", "CATEGORY": "DG", "TITLE": "Other", "TITLE_ID": "NPUB30001"})
        eu["PS3_GAME/PARAM.SFO"] = open(sfo, "rb").read()
        rc, out = run(["--dump", build_iso(os.path.join(i, "o.iso"), eu)] + small + hashes(got))
        check("ISO: another title id fails D1", rc == 1 and "FAIL D1" in out and "NPUB30001" in out, out)
        nofile = {k: v for k, v in files.items() if "psarc" not in k}
        rc, out = run(["--dump", build_iso(os.path.join(i, "n.iso"), nofile)] + small + hashes(got))
        check("ISO: no psarc fails D3", rc == 1 and "FAIL D3" in out and "no PS3_GAME/USRDIR/data_ps3.psarc" in out, out)
        rc, out = run(["--dump", build_iso(os.path.join(i, "e.iso"), {"PS3_DISC.SFB": b"x"})] + small + hashes(got))
        check("ISO: no PARAM.SFO fails D1", rc == 1 and "FAIL D1" in out, out)
        # encrypted: files whose magic is wrong
        scrambled = dict(files)
        scrambled["PS3_GAME/USRDIR/data_ps3.psarc"] = bytes(range(200)) * 30
        rc, out = run(["--dump", build_iso(os.path.join(i, "s.iso"), scrambled)] + small + hashes(got))
        check("ISO: unreadable files are 'encrypted' (I1)", rc == 1 and "FAIL I1: this ISO is encrypted; use a decrypted dump" in out, out)
        scrambled = dict(files)
        scrambled["PS3_GAME/PARAM.SFO"] = bytes(range(100))
        rc, out = run(["--dump", build_iso(os.path.join(i, "s2.iso"), scrambled)] + small + hashes(got))
        check("ISO: unreadable PARAM.SFO is 'encrypted' (I1)", rc == 1 and "FAIL I1" in out, out)
        # region table in sector 0, no volume descriptor
        reg = bytearray(2048 * 40)
        reg[0:4] = struct.pack(">I", 2)
        reg[8:24] = struct.pack(">IIII", 0, 15, 16, 30)
        open(os.path.join(i, "r.iso"), "wb").write(bytes(reg))
        rc, out = run(["--dump", os.path.join(i, "r.iso")] + small + hashes(got))
        check("ISO: region table image is I1", rc == 1 and "FAIL I1" in out, out)
        # not an ISO, truncated
        open(os.path.join(i, "x.iso"), "wb").write(b"hello" * 100000)
        rc, out = run(["--dump", os.path.join(i, "x.iso")] + small + hashes(got))
        check("ISO: not an ISO is I2, not 'encrypted'", rc == 1 and "FAIL I2" in out and "I1" not in out, out)
        data = open(iso, "rb").read()
        open(os.path.join(i, "t.iso"), "wb").write(data[:len(data) - 3000])
        rc, out = run(["--dump", os.path.join(i, "t.iso")] + small + hashes(got))
        check("ISO: truncated image fails D3", rc == 1 and "FAIL D3" in out and "truncated" in out, out)
        # a folder still works and has no ISO line
        rc, out = run(["--dump", d, "--update", u] + small + hashes(got))
        check("folder dump: no ISO line", "decrypted ISO" not in out, out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"firstrun_check: {'PASS' if not FAILS else 'FAIL ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
