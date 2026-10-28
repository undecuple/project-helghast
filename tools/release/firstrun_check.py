#!/usr/bin/env python3
"""First-run checks for the release installer and the portable zip: is the player's own game dump, 1.14 update
and decrypted EBOOT what the port can lift and build from? Standard library only, Python 3.8+, runs on the bundled
Windows Python and on Linux; reads nothing of the game beyond PARAM.SFO, the first bytes of each file (magic) and
file sizes.

    firstrun_check.py --dump DIR [--update DIR] [--eboot FILE] [--json] [--min-psarc-bytes N] [--hashes FILE]

  --dump    the decrypted disc dump folder (holds PS3_GAME/PARAM.SFO)
  --update  the installed 1.14 update folder, where a PS3 or RPCS3 keeps it: .../dev_hdd0/game/BCUS98234
            (holds PARAM.SFO and USRDIR/EBOOT.BIN)
  --eboot   the decrypted EBOOT (an ELF); default: <update>/USRDIR/EBOOT.elf, else <dump>/PS3_GAME/USRDIR/EBOOT.elf
            (next to EBOOT.BIN), else an EBOOT.BIN that is already an ELF
  --hashes  expected EBOOT SHA-256 per version; default eboot_sha256.json next to this script. H1 checks
            the ELF's SHA-256 against it: a mismatch is a WARN with the size and both hashes (never a FAIL: the build
            only needs a 1.14 EBOOT that lifts); an empty value means "unknown yet" and also only warns.

  --root / --build  the install and build folders; a FAIL O1 when either is inside OneDrive (env OneDrive,
            OneDriveConsumer, OneDriveCommercial). O2 FAIL: a game file is an online-only OneDrive placeholder
            (file attributes 0x400000, 0x40000 or 0x1000); checked before any file is read, so nothing downloads.
            O3 (OK, INFO): the dump or update is inside OneDrive but fully local.

One line per check: OK / WARN / FAIL <code>: <message>. Exit 0 = no FAIL, 1 = at least one FAIL, 2 = usage.
--json prints {"ok": bool, "checks": [{"code", "level", "message"}], "facts": {...}} instead. Numbers come from
the RE notes and a known-good dump (ASSUMPTION where marked); no game content is embedded.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

TITLE_ID = "BCUS98234"                 # US disc (the RE notes); other regions are untested
DISC_EBOOT_BIN_SIZE = 20_748_384       # PS3_GAME/USRDIR/EBOOT.BIN on the disc, encrypted SELF (the RE notes)
EBOOT_ELF_SIZES = {                    # decrypted EBOOTs the lift fixups were made for
    20_745_952: "1.00",
    21_228_256: "1.14",
}
UPDATE_APP_VER = "01.14"               # ASSUMPTION: the update's PARAM.SFO APP_VER; not read yet (needs disc_info.py on it)
UPDATE_EDAT = "added_data_ps3.psarc.edat"   # the update's data archive (the RE notes 8.4), 7,168,019 bytes
MIN_PSARC_BYTES = 8 << 30              # data_ps3.psarc is 9.6 GB on disc; smaller = truncated dump
PSARC_MAGIC = b"PSAR"
SELF_MAGIC = b"SCE\0"
ELF_MAGIC = b"\x7fELF"
EM_PPC64 = 0x15
ONEDRIVE_VARS = ("OneDrive", "OneDriveConsumer", "OneDriveCommercial")
# st_file_attributes bits of a OneDrive "files on demand" placeholder (online-only file)
ATTR_OFFLINE = 0x1000
ATTR_RECALL_ON_OPEN = 0x40000
ATTR_RECALL_ON_DATA_ACCESS = 0x400000
ONLINE_ONLY_MASK = ATTR_OFFLINE | ATTR_RECALL_ON_OPEN | ATTR_RECALL_ON_DATA_ACCESS
HASHES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "eboot_sha256.json")


def read_sfo(path):
    """PARAM.SFO -> dict (same decoder as tools/disc_info.py)."""
    d = open(path, "rb").read()
    if d[:4] != b"\0PSF":
        raise ValueError("not a PARAM.SFO (bad magic)")
    _, _, kt, dt, n = struct.unpack("<4sIIII", d[:20])
    out = {}
    for i in range(n):
        ko, fmt, ln, _, do = struct.unpack("<HHIII", d[20 + 16 * i:36 + 16 * i])
        k = d[kt + ko:d.index(b"\0", kt + ko)].decode()
        v = d[dt + do:dt + do + ln]
        out[k] = struct.unpack("<I", v)[0] if fmt == 0x404 else v.rstrip(b"\0").decode(errors="replace")
    return out


def _norm(p):
    return os.path.abspath(p).replace("/", "\\").rstrip("\\").lower()


def onedrive_root_of(path, env=None):
    """The OneDrive folder (from OneDrive, OneDriveConsumer, OneDriveCommercial) that holds `path`, else ''.
    Prefix match on whole path components, case-insensitive; the same rule as installer/kz3.iss."""
    env = os.environ if env is None else env
    p = _norm(path)
    for var in ONEDRIVE_VARS:
        base = (env.get(var) or "").strip().strip('"')
        if not base:
            continue
        b = _norm(base)
        if b and (p == b or p.startswith(b + "\\")):
            return base
    return ""


def file_attributes(path):
    """Windows st_file_attributes of `path` (0 elsewhere or when unreadable). Reads no file content."""
    try:
        return getattr(os.stat(path), "st_file_attributes", 0)
    except OSError:
        return 0


def check_onedrive_tree(r, root, build, env=None):
    """O1 (FAIL): the install root or the build folder is inside OneDrive: it would sync the build's thousands of
    files while the compiler writes them (locked files, a churning build). Silent when fine."""
    for what, path in (("install folder", root), ("build folder", build)):
        od = onedrive_root_of(path, env) if path else ""
        if od:
            r.fail("O1", f"the {what} {path} is inside OneDrive ({od}), which syncs the build's thousands of files "
                         "while it runs and locks them. Choose a folder outside OneDrive (for the installer the "
                         "default, %LOCALAPPDATA%\\Programs\\Project Helghast, is fine) and run the setup again")


def game_files(dump, update, eboot):
    """The files the build reads."""
    f = []
    if dump:
        u = os.path.join(dump, "PS3_GAME", "USRDIR")
        f += [os.path.join(dump, "PS3_GAME", "PARAM.SFO"), os.path.join(u, "EBOOT.BIN"),
              os.path.join(u, "data_ps3.psarc")]
    if update:
        f += [os.path.join(update, "PARAM.SFO"), os.path.join(update, "USRDIR", "EBOOT.BIN"),
              os.path.join(update, "USRDIR", UPDATE_EDAT), os.path.join(update, "USRDIR", "EBOOT.elf")]
    if eboot:
        f.append(eboot)
    return f


def check_online_only(r, dump, update, eboot, env=None, attrs=None):
    """O2 (FAIL): a game file is a OneDrive online-only placeholder (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN or
    OFFLINE): reading the 9 GB archive would download it or fail with a cloud-provider error. O3 (OK, an INFO
    line): a folder inside OneDrive whose files are all local. Runs before any content is read; returns True if O2
    failed."""
    attrs = attrs or file_attributes
    online = [p for p in game_files(dump, update, eboot) if os.path.exists(p) and attrs(p) & ONLINE_ONLY_MASK]
    if online:
        names = ", ".join(os.path.basename(p) for p in online[:4]) + (" ..." if len(online) > 4 else "")
        r.fail("O2", f"{len(online)} game file(s) exist only online in OneDrive ({names}; first: {online[0]}). "
                     "Right-click the game folder (the disc dump, the update and the EBOOT's folder), choose "
                     "\"Always keep on this device\", wait until OneDrive has downloaded it, then run the setup again")
        return True
    for what, path in (("disc dump", dump), ("1.14 update", update)):
        od = onedrive_root_of(path, env) if path else ""
        if od:
            r.ok("O3", f"INFO: the {what} folder is inside OneDrive ({od}) but its files are on this device")
    return False


def head(path, n):
    with open(path, "rb") as f:
        return f.read(n)


class Report:
    def __init__(self):
        self.checks = []
        self.facts = {}

    def add(self, level, code, msg):
        self.checks.append({"code": code, "level": level, "message": msg})

    def ok(self, code, msg):
        self.add("OK", code, msg)

    def warn(self, code, msg):
        self.add("WARN", code, msg)

    def fail(self, code, msg):
        self.add("FAIL", code, msg)

    @property
    def passed(self):
        return not any(c["level"] == "FAIL" for c in self.checks)


def check_sfo(r, path, code, what):
    """Shared PARAM.SFO check for the dump and the update. Returns the fields or None."""
    if not os.path.isfile(path):
        r.fail(code, f"{what}: no PARAM.SFO at {path}")
        return None
    try:
        sfo = read_sfo(path)
    except (ValueError, struct.error, IndexError) as e:
        r.fail(code, f"{what}: PARAM.SFO unreadable ({e})")
        return None
    tid = sfo.get("TITLE_ID")
    if tid != TITLE_ID:
        r.fail(code, f"{what}: TITLE_ID is {tid!r}, the port needs {TITLE_ID} (US disc); other releases are untested")
    else:
        r.ok(code, f"{what}: TITLE_ID {tid} ({sfo.get('TITLE', '?')})")
    return sfo


def check_dump(r, dump, min_psarc):
    sfo = check_sfo(r, os.path.join(dump, "PS3_GAME", "PARAM.SFO"), "D1", "disc dump")
    if sfo:
        r.facts["disc"] = {k: sfo.get(k) for k in ("TITLE_ID", "TITLE", "VERSION", "APP_VER", "CATEGORY")}
        if sfo.get("CATEGORY") != "DG":
            r.warn("D1", f"disc dump: CATEGORY {sfo.get('CATEGORY')!r}, a disc game is 'DG' (an installed copy?)")
    eboot = os.path.join(dump, "PS3_GAME", "USRDIR", "EBOOT.BIN")
    if not os.path.isfile(eboot):
        r.fail("D2", "disc dump: no PS3_GAME/USRDIR/EBOOT.BIN (dump incomplete)")
    else:
        magic, size = head(eboot, 4), os.path.getsize(eboot)
        r.facts["disc_eboot_bin"] = {"size": size, "magic": magic.hex()}
        if magic == SELF_MAGIC:
            r.ok("D2", f"disc EBOOT.BIN is an encrypted SELF ({size:,} bytes; expected: the disc one is never used directly)")
        elif magic == ELF_MAGIC:
            r.ok("D2", f"disc EBOOT.BIN is already a decrypted ELF ({size:,} bytes)")
        else:
            r.fail("D2", f"disc EBOOT.BIN has magic {magic!r}: neither SELF nor ELF (bad dump)")
        if magic == SELF_MAGIC and size != DISC_EBOOT_BIN_SIZE:
            r.warn("D2", f"disc EBOOT.BIN is {size:,} bytes, the US disc's is {DISC_EBOOT_BIN_SIZE:,} (another version?)")
    psarc = os.path.join(dump, "PS3_GAME", "USRDIR", "data_ps3.psarc")
    if not os.path.isfile(psarc):
        r.fail("D3", "disc dump: no PS3_GAME/USRDIR/data_ps3.psarc (the game's data archive)")
    else:
        magic, size = head(psarc, 4), os.path.getsize(psarc)
        r.facts["psarc"] = {"size": size}
        if magic != PSARC_MAGIC:
            r.fail("D3", f"data_ps3.psarc does not start with PSAR (magic {magic!r}): not a PSARC, or still encrypted")
        elif size < min_psarc:
            r.fail("D3", f"data_ps3.psarc is {size:,} bytes, less than {min_psarc:,}: the dump looks truncated")
        else:
            r.ok("D3", f"data_ps3.psarc present ({size / 2**30:.2f} GiB)")


def check_update(r, upd):
    sfo = check_sfo(r, os.path.join(upd, "PARAM.SFO"), "U1", "1.14 update")
    if sfo:
        ver = sfo.get("APP_VER")
        r.facts["update"] = {k: sfo.get(k) for k in ("TITLE_ID", "APP_VER", "VERSION", "CATEGORY")}
        if ver != UPDATE_APP_VER:
            r.warn("U1", f"update APP_VER is {ver!r}, the port is built for {UPDATE_APP_VER} (unverified expectation)")
        else:
            r.ok("U1", f"update APP_VER {ver}")
    eb = os.path.join(upd, "USRDIR", "EBOOT.BIN")
    if not os.path.isfile(eb):
        r.fail("U2", "1.14 update: no USRDIR/EBOOT.BIN (update not installed here)")
    else:
        r.ok("U2", f"update EBOOT.BIN present ({os.path.getsize(eb):,} bytes, {'SELF' if head(eb, 4) == SELF_MAGIC else 'ELF'})")
    edat = os.path.join(upd, "USRDIR", UPDATE_EDAT)
    if os.path.isfile(edat):
        r.ok("U3", f"update data archive present ({os.path.getsize(edat):,} bytes)")
    else:
        r.warn("U3", f"1.14 update: no USRDIR/{UPDATE_EDAT}; the update's data layer will be missing (disc data only)")


def check_eboot_elf(r, elf, target="1.14"):
    if not os.path.isfile(elf):
        r.fail("E1", f"no decrypted EBOOT at {elf}: the port needs EBOOT.elf, the decrypted EBOOT.BIN of your own copy "
                     "(this project includes no decryption tools)")
        return
    h, size = head(elf, 64), os.path.getsize(elf)
    r.facts["eboot_elf"] = {"path": elf, "size": size}
    if len(h) < 64 or h[:4] != ELF_MAGIC:
        r.fail("E1", f"{elf} is not an ELF (magic {h[:4]!r}); an encrypted SELF starts with 'SCE'")
        return
    cls, data = h[4], h[5]
    machine = struct.unpack(">H", h[18:20])[0] if data == 2 else struct.unpack("<H", h[18:20])[0]
    if cls != 2 or data != 2 or machine != EM_PPC64:
        r.fail("E1", f"{elf}: ELF class {cls}, data {data}, machine 0x{machine:x}; a PS3 EBOOT is 64-bit big-endian PPC64")
        return
    ver = EBOOT_ELF_SIZES.get(size)
    r.facts["eboot_elf"]["version"] = ver
    if ver == target:
        r.ok("E1", f"decrypted EBOOT is the {ver} one ({size:,} bytes)")
    elif ver == "1.00":
        r.warn("E1", f"decrypted EBOOT is the disc v1.00 one ({size:,} bytes); the port is built on {target}: "
                     "use the decrypted EBOOT of the 1.14 update instead")
    elif ver:
        r.warn("E1", f"decrypted EBOOT is the {ver} one ({size:,} bytes); this build is for the {target} EBOOT")
    else:
        r.warn("E1", f"decrypted EBOOT has an unknown size ({size:,} bytes): not 1.00 or 1.14; the lift fixups may not apply")


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_eboot_hash(r, elf, hashes_file):
    """H1: the decrypted EBOOT's SHA-256 against the expected one for its version (1.14 when the size
    is unknown). Warn-only: mismatch, unknown yet (empty value) and an unreadable hashes file all give WARN."""
    info = r.facts.get("eboot_elf")
    if not info or "version" not in info:      # E1 failed (missing, not an ELF, wrong machine): nothing to hash
        return
    try:
        got = file_sha256(elf)
    except OSError as e:
        r.warn("H1", f"could not read {elf} to hash it: {e}")
        return
    info["sha256"] = got
    ver = info.get("version") or "1.14"
    try:
        with open(hashes_file, encoding="utf-8") as f:
            want = (json.load(f).get(ver) or {}).get("sha256", "").strip().lower()
    except (OSError, ValueError, AttributeError) as e:
        r.warn("H1", f"expected EBOOT hashes not readable ({hashes_file}: {e}); SHA-256 {got} not checked")
        return
    if not want:
        r.warn("H1", f"expected SHA-256 of the {ver} EBOOT is unknown yet (empty in {os.path.basename(hashes_file)}); "
                     f"this one is {got} ({info['size']:,} bytes), not checked")
    elif want == got:
        r.ok("H1", f"EBOOT SHA-256 matches the known {ver} EBOOT")
    else:
        r.warn("H1", f"EBOOT SHA-256 differs from the known {ver} one: {info['size']:,} bytes, SHA-256 {got}, "
                     f"expected {want}. The build may still work if it is a 1.14 EBOOT that lifts; if it fails, "
                     "make the decrypted EBOOT from the update's EBOOT.BIN again")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", required=True)
    ap.add_argument("--update")
    ap.add_argument("--eboot")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-psarc-bytes", type=int, default=MIN_PSARC_BYTES)
    ap.add_argument("--hashes", default=HASHES_FILE)
    ap.add_argument("--root", help="the install folder; refused (O1) when inside OneDrive")
    ap.add_argument("--build", help="the build folder; refused (O1) when inside OneDrive")
    ap.add_argument("--target", choices=("1.00", "1.14"), default="1.14",
                    help="the EBOOT version the build is for (firstrun.py passes its tree's; pass 12)")
    a = ap.parse_args(argv)
    r = Report()
    check_onedrive_tree(r, a.root, a.build)
    # before anything is read: opening an online-only placeholder would start a download of the 9 GB archive
    if check_online_only(r, a.dump if os.path.isdir(a.dump) else "", a.update if a.update and os.path.isdir(a.update) else "",
                         a.eboot or ""):
        return finish(r, a)
    if not os.path.isdir(a.dump):
        r.fail("D0", f"dump folder not found: {a.dump}")
    else:
        check_dump(r, a.dump, a.min_psarc_bytes)
    if a.update:
        if not os.path.isdir(a.update):
            r.fail("U0", f"update folder not found: {a.update}")
        else:
            check_update(r, a.update)
    else:
        r.warn("U0", "no --update given: the 1.14 update is required (docs/requirements.md)")
    elf = a.eboot
    if not elf:
        # RPCS3 writes EBOOT.elf next to EBOOT.BIN; a dump decrypted some other way may hold an ELF as EBOOT.BIN.
        cand = [os.path.join(a.update, "USRDIR", n) for n in ("EBOOT.elf", "EBOOT.BIN")] if a.update else []
        cand += [os.path.join(a.dump, "PS3_GAME", "USRDIR", n) for n in ("EBOOT.elf", "EBOOT.BIN")]
        elf = next((c for c in cand if os.path.isfile(c) and head(c, 4) == ELF_MAGIC), cand[0])
    check_eboot_elf(r, elf, a.target)
    check_eboot_hash(r, elf, a.hashes)
    return finish(r, a)


def finish(r, a):
    if a.json:
        print(json.dumps({"ok": r.passed, "checks": r.checks, "facts": r.facts}, indent=2))
    else:
        for c in r.checks:
            print(f"{c['level']:4} {c['code']}: {c['message']}")
        print("first-run checks:", "pass" if r.passed else "FAIL")
    return 0 if r.passed else 1


if __name__ == "__main__":
    sys.exit(main())
