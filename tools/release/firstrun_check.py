#!/usr/bin/env python3
"""First-run checks for the release installer and the portable zip: is the player's own game dump, 1.14 update
and decrypted EBOOT what the port can lift and build from? Standard library only, Python 3.8+, runs on the bundled
Windows Python and on Linux; reads nothing of the game beyond PARAM.SFO, the first bytes of each file (magic) and
file sizes.

    firstrun_check.py --dump DIR [--update DIR] [--eboot FILE] [--json] [--min-psarc-bytes N] [--hashes FILE]

  --dump    the decrypted disc dump folder (holds PS3_GAME/PARAM.SFO), or a decrypted disc ISO file (a file, not a
            folder): then PARAM.SFO, EBOOT.BIN and data_ps3.psarc are read from inside the image (iso9660.py) with the
            same D1-D3 checks plus an INFO line "dump: decrypted ISO <name>". I1 FAIL: the ISO is encrypted ("this ISO is
            encrypted; use a decrypted dump"); I2 FAIL: the ISO cannot be read (not an ISO, damaged, truncated).
  --update  the installed 1.14 update folder, where a PS3 or RPCS3 keeps it: .../dev_hdd0/game/<title id>
            (BCUS98234 or, untested, BCES01007 / BCAS25008 / BCJS30066; its TITLE_ID may be another accepted region's (WARN); holds PARAM.SFO and USRDIR/EBOOT.BIN)
  --eboot   the decrypted EBOOT (an ELF); default: <update>/USRDIR/EBOOT.elf, else <dump>/PS3_GAME/USRDIR/EBOOT.elf
            (next to EBOOT.BIN), else an EBOOT.BIN that is already an ELF
            E1 FAILs on a known EBOOT of another version than --target (the disc's 1.00 one on the 1.14 build: the
            message names the update's USRDIR folder); an unknown size only warns
  --hashes  expected EBOOT SHA-256 per version; default eboot_sha256.json next to this script. H1 checks
            the ELF's SHA-256 against it: a mismatch is a WARN with the size and both hashes (never a FAIL: the build
            only needs a 1.14 EBOOT that lifts); an empty value means "unknown yet" and also only warns.

  --psarc-hashes  the known data_ps3.psarc (default psarc_sha256.json next to this script). A1: SHA-256 of the whole
            archive (chunked; about 23 s on an SSD) against it. OK = matches; FAIL on a listed disc (US, EU) = differs
            (damaged or modified copy); WARN on any other disc = not a known archive yet (untested region). The hash is
            cached in <build>/psarc-sha256-cache.json by path, size and mtime (no cache without --build). A folder dump is
            hashed here; an ISO is hashed by firstrun.py after its files are extracted (no second read of the image).
            --progress prints "ARCHIVE-HASH done=X total=Y" lines (firstrun.py turns them into PROGRESS lines);
            --skip-archive-hash leaves A1 out.

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

# the bundled embeddable Python (a python3XX._pth file) puts only its own folders on sys.path, never the script's:
# firstrun.py starts this file by full path, and `import iso9660` (an ISO dump) failed there (issue #10)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TITLE_ID = "BCUS98234"                 # US disc (the RE notes)
TITLE_IDS = {"BCUS98234": "US", "BCES01007": "EU", "BCAS25008": "Asia", "BCJS30066": "JP"}   # all but US: accepted as untested regions (H1 checks each copy)
REGION_NAMES = {"EU": "EU disc", "Asia": "Asian disc", "JP": "Japanese disc"}
REGION_NOTES = {                       # EU: a player's report; Asia, JP: the updates were decrypted and compared locally
    "EU": "a player's report shows the same 1.14 EBOOT hash as the US version",
    "Asia": "its 1.14 update carries the same EBOOT as the US one (checked on our copy)",
    "JP": "its 1.14 update carries the same EBOOT as the US one (checked on our copy)",
}
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
PSARC_HASHES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "psarc_sha256.json")
PSARC_CACHE_NAME = "psarc-sha256-cache.json"
HASH_CHUNK = 8 << 20
SPLIT_FIRST = 66600                    # a FAT32 dump cuts files over 4 GiB into <name>.66600, .66601, ... (public issue 4)


def read_sfo(path):
    """PARAM.SFO -> dict (same decoder as tools/disc_info.py)."""
    return parse_sfo(open(path, "rb").read())


def parse_sfo(d):
    """The bytes of a PARAM.SFO -> dict."""
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


def split_parts(path):
    """The pieces of a file as a FAT32 dump stores it. -> (parts, missing): parts = [path] when the file exists, else
    the consecutive <path>.66600, .66601, ... that exist (stops at the first gap); missing = the file name to ask the
    player for ("" when nothing is missing): <name> when neither the file nor any part exists, <name>.666NN for the
    first gap in the numbering (also when later parts exist but .66600 does not)."""
    if os.path.isfile(path):
        return [path], ""
    d, name = os.path.split(path)
    pre = name.lower() + "."
    nums = set()
    try:
        for n in os.listdir(d or "."):
            if n.lower().startswith(pre) and n[len(pre):].isdigit() and int(n[len(pre):]) >= SPLIT_FIRST \
                    and os.path.isfile(os.path.join(d, n)):
                nums.add(int(n[len(pre):]))
    except OSError:
        pass
    parts, i = [], SPLIT_FIRST
    while i in nums:
        parts.append(f"{path}.{i}")
        i += 1
    if not parts and not nums:
        return [], name
    if nums and max(nums) >= i:                    # a part past the gap exists: the numbering has a hole
        return parts, f"{name}.{i}"
    return parts, ""


def psarc_info(path):
    """-> (parts, missing, total size, first four bytes) of data_ps3.psarc as a file or as FAT32 split parts."""
    parts, missing = split_parts(path)
    if not parts or missing:
        return parts, missing, 0, b""
    return parts, "", sum(os.path.getsize(p) for p in parts), head(parts[0], 4)


def game_files(dump, update, eboot):
    """The files the build reads."""
    f = []
    if dump and os.path.isfile(dump):
        f.append(dump)
    elif dump:
        u = os.path.join(dump, "PS3_GAME", "USRDIR")
        f += [os.path.join(dump, "PS3_GAME", "PARAM.SFO"), os.path.join(u, "EBOOT.BIN"),
              *(split_parts(os.path.join(u, "data_ps3.psarc"))[0] or [os.path.join(u, "data_ps3.psarc")])]
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


def check_sfo(r, path, code, what, expect=None):
    """Shared PARAM.SFO check for the dump and the update. Returns the fields or None."""
    if not os.path.isfile(path):
        r.fail(code, f"{what}: no PARAM.SFO at {path}")
        return None
    try:
        sfo = read_sfo(path)
    except (ValueError, struct.error, IndexError) as e:
        r.fail(code, f"{what}: PARAM.SFO unreadable ({e})")
        return None
    return sfo_report(r, sfo, code, what, expect)


def sfo_report(r, sfo, code, what, expect=None):
    tid = sfo.get("TITLE_ID")
    if tid not in TITLE_IDS:
        r.fail(code, f"{what}: TITLE_ID is {tid!r}, not Killzone 3 (US {TITLE_ID}, or, untested, EU BCES01007, Asian BCAS25008, Japanese BCJS30066)")
    elif expect and tid != expect:
        r.warn(code, f"{what}: update from another region ({tid}) than the disc ({expect}): it gave the same game data in "
                     f"our tests (US and EU discs)")
    elif tid != TITLE_ID:
        reg = TITLE_IDS[tid]
        r.warn(code, f"{what}: {REGION_NAMES[reg]} ({tid}): untested region; {REGION_NOTES[reg]} "
                     f"({sfo.get('TITLE', '?')})")
    else:
        r.ok(code, f"{what}: TITLE_ID {tid} ({sfo.get('TITLE', '?')})")
    return sfo


def check_dump(r, dump, min_psarc):
    sfo = check_sfo(r, os.path.join(dump, "PS3_GAME", "PARAM.SFO"), "D1", "disc dump")
    eboot = os.path.join(dump, "PS3_GAME", "USRDIR", "EBOOT.BIN")
    psarc = os.path.join(dump, "PS3_GAME", "USRDIR", "data_ps3.psarc")
    parts, missing, size, magic = psarc_info(psarc)
    dump_files(r, sfo,
               (os.path.getsize(eboot), head(eboot, 4)) if os.path.isfile(eboot) else None,
               (size, magic) if parts and not missing else None, min_psarc, psarc_parts=len(parts), psarc_missing=missing)


def dump_files(r, sfo, eboot, psarc, min_psarc, psarc_cut=False, psarc_parts=1, psarc_missing=""):
    """D1 facts and the D2 / D3 checks from what was read: eboot / psarc = (size, first four bytes) or None (absent).
    psarc_cut: the file's bytes reach past the end of its (ISO) image. psarc_parts: more than 1 = the archive is
    FAT32 split parts (size = their total). psarc_missing: the part name absent from a split set (a gap)."""
    if sfo:
        r.facts["disc"] = {k: sfo.get(k) for k in ("TITLE_ID", "TITLE", "VERSION", "APP_VER", "CATEGORY")}
        if sfo.get("CATEGORY") != "DG":
            r.warn("D1", f"disc dump: CATEGORY {sfo.get('CATEGORY')!r}, a disc game is 'DG' (an installed copy?)")
    if eboot is None:
        r.fail("D2", "disc dump: no PS3_GAME/USRDIR/EBOOT.BIN (dump incomplete)")
    else:
        size, magic = eboot
        r.facts["disc_eboot_bin"] = {"size": size, "magic": magic.hex()}
        if magic == SELF_MAGIC:
            r.ok("D2", f"disc EBOOT.BIN is an encrypted SELF ({size:,} bytes; expected: the disc one is never used directly)")
        elif magic == ELF_MAGIC:
            r.ok("D2", f"disc EBOOT.BIN is already a decrypted ELF ({size:,} bytes)")
        else:
            r.fail("D2", f"disc EBOOT.BIN has magic {magic!r}: neither SELF nor ELF (bad dump)")
        if magic == SELF_MAGIC and size != DISC_EBOOT_BIN_SIZE:
            r.warn("D2", f"disc EBOOT.BIN is {size:,} bytes, the US disc's is {DISC_EBOOT_BIN_SIZE:,} (another version?)")
    if psarc is None and psarc_missing.lower() != "data_ps3.psarc" and psarc_missing:
        r.fail("D3", f"disc dump: PS3_GAME/USRDIR/{psarc_missing} is missing: data_ps3.psarc is split into parts "
                     "(data_ps3.psarc.66600, .66601, ...) and the numbering has a gap (dump incomplete)")
    elif psarc is None:
        r.fail("D3", "disc dump: no PS3_GAME/USRDIR/data_ps3.psarc (the game's data archive)")
    else:
        size, magic = psarc
        r.facts["psarc"] = {"size": size}
        if psarc_parts > 1:
            r.facts["psarc"]["parts"] = psarc_parts
        if psarc_cut:
            r.fail("D3", f"data_ps3.psarc ({size:,} bytes) reaches past the end of the ISO file: the ISO looks truncated")
        elif magic != PSARC_MAGIC:
            r.fail("D3", f"data_ps3.psarc does not start with PSAR (magic {magic!r}): not a PSARC, or still encrypted")
        elif size < min_psarc:
            r.fail("D3", f"data_ps3.psarc is {size:,} bytes, less than {min_psarc:,}: the dump looks truncated")
        else:
            r.ok("D3", f"data_ps3.psarc present ({size / 2**30:.2f} GiB"
                       + (f", {psarc_parts} split parts .66600 ...)" if psarc_parts > 1 else ")"))


def hash_file(path, progress=None):
    """-> the file's SHA-256 (hex), read in HASH_CHUNK pieces; progress(done, total) after each piece. path may be a
    list of split parts: the hash of their concatenation in list order, progress over the total."""
    h = hashlib.sha256()
    paths = [path] if isinstance(path, str) else list(path)
    total = sum(os.path.getsize(p) for p in paths)
    done = 0
    for p in paths:
        with open(p, "rb") as f:
            while True:
                b = f.read(HASH_CHUNK)
                if not b:
                    break
                h.update(b)
                done += len(b)
                if progress:
                    progress(done, total)
    return h.hexdigest()


def check_psarc_hash(r, psarc, title_id, hashes_file=PSARC_HASHES_FILE, cache_dir=None, progress=None):
    """A1: data_ps3.psarc against the known disc archive (psarc_sha256.json). The verdict follows the disc: a mismatch
    on a listed disc (US, EU) is a FAIL, on another disc a WARN. A different size is a mismatch without reading the
    file. The hash is cached in <cache_dir>/psarc-sha256-cache.json keyed by path, size and mtime (for split parts,
    .66600 ..., the hash is that of their concatenation in part order, the size their total, and the cache entry
    holds path, size and mtime of every part)."""
    try:
        with open(hashes_file, encoding="utf-8") as f:
            known = json.load(f)
        want, want_size, discs = known["sha256"].lower(), int(known["size"]), list(known.get("discs", []))
    except (OSError, ValueError, KeyError) as e:
        r.warn("A1", f"known archive hash not readable ({hashes_file}: {e}); data_ps3.psarc not checked")
        return
    parts, missing = split_parts(psarc)
    if not parts or missing:
        r.warn("A1", f"data_ps3.psarc not complete ({missing or 'not found'} is missing); archive not hashed")
        return
    size = sum(os.path.getsize(p) for p in parts)
    sig = [[os.path.abspath(p), os.path.getsize(p), os.stat(p).st_mtime_ns] for p in parts]
    key = os.path.abspath(psarc)
    cache_path = os.path.join(cache_dir, PSARC_CACHE_NAME) if cache_dir else None
    cache = {}
    if cache_path:
        try:
            with open(cache_path, encoding="utf-8") as f:
                cache = json.load(f)
            if not isinstance(cache, dict):
                cache = {}
        except (OSError, ValueError):
            cache = {}
    got, cached = None, False
    ent = cache.get(key)
    if isinstance(ent, dict) and ent.get("size") == size and ent.get("parts") == sig and ent.get("sha256"):
        got, cached = ent["sha256"], True
    elif size == want_size:
        got = hash_file(parts[0] if len(parts) == 1 else parts, progress)
        if cache_path:
            cache[key] = {"size": size, "parts": sig, "sha256": got}
            try:
                os.makedirs(cache_dir, exist_ok=True)
                with open(cache_path, "w", encoding="utf-8") as f:
                    json.dump(cache, f, indent=1)
            except OSError:
                pass
    r.facts.setdefault("psarc", {})["sha256"] = got or ""
    if got == want:
        r.ok("A1", "data_ps3.psarc matches the known disc archive" + (" (hash cached from an earlier run)" if cached else ""))
    elif title_id in discs:
        r.fail("A1", f"data_ps3.psarc differs from the known disc archive (damaged or modified copy): copy or dump the "
                     f"disc again ({size:,} bytes" + (f", SHA-256 {got}" if got else f"; the archive is {want_size:,} bytes") + ")")
    else:
        r.warn("A1", f"data_ps3.psarc is not a known archive yet (untested region; {title_id or 'no title id'})")


ISO_ENCRYPTED = "this ISO is encrypted; use a decrypted dump"


def check_dump_iso(r, path, min_psarc):
    """The same D1-D3 from inside a decrypted ISO (iso9660.py). I1 FAIL = encrypted (the file table or the files are
    unreadable: no volume descriptor behind a region table, or a file whose first bytes are not its format's magic),
    I2 FAIL = the image cannot be read (not an ISO, damaged, truncated)."""
    import iso9660
    r.ok("D0", f"INFO: dump: decrypted ISO {os.path.basename(path)}")
    try:
        with iso9660.Iso(path) as im:
            sfo, info, cut = None, {}, False
            se = im.lookup("PS3_GAME/PARAM.SFO")
            if se is None or se.is_dir or not im.in_image(se):
                r.fail("D1", "disc dump: no PS3_GAME/PARAM.SFO inside the ISO")
            else:
                im.check_magic("PS3_GAME/PARAM.SFO", b"\0PSF")
                try:
                    sfo = sfo_report(r, parse_sfo(im.read("PS3_GAME/PARAM.SFO")), "D1", "disc dump")
                except (ValueError, struct.error, IndexError) as e:
                    r.fail("D1", f"disc dump: PARAM.SFO unreadable ({e})")
            for key, rel in (("eboot", "PS3_GAME/USRDIR/EBOOT.BIN"), ("psarc", "PS3_GAME/USRDIR/data_ps3.psarc")):
                e = im.lookup(rel)
                if e is None or e.is_dir:
                    info[key] = None
                    continue
                inside = im.in_image(e)
                if key == "psarc":
                    cut = not inside
                info[key] = (e.size, im.read(rel, 4) if inside else b"")
                if inside and info[key][1] not in ((SELF_MAGIC, ELF_MAGIC) if key == "eboot" else (PSARC_MAGIC,)):
                    raise iso9660.IsoEncrypted(ISO_ENCRYPTED)
            dump_files(r, sfo, info.get("eboot"), info.get("psarc"), min_psarc, cut and bool(info.get("psarc")))
    except iso9660.IsoEncrypted:
        r.fail("I1", ISO_ENCRYPTED)
    except iso9660.IsoError as e:
        r.fail("I2", f"disc ISO unreadable: {e}")


def check_update(r, upd, disc_id=None):
    sfo = check_sfo(r, os.path.join(upd, "PARAM.SFO"), "U1", "1.14 update", disc_id)
    if sfo and sfo.get("CATEGORY") == "DG":
        r.fail("U1", "1.14 update: this PARAM.SFO is the disc's (category DG), not an update's: the disc's PS3_GAME "
                     "folder was given as the update")
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


def check_eboot_elf(r, elf, target="1.14", update=None):
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
        # a FAIL since pass 63 (issue #8): as a WARN it passed the console's check and the build then stopped at
        # firstrun's version test, so a restart reused the same EBOOT
        r.fail("E1", f"this EBOOT is the disc's 1.00 one ({size:,} bytes); this release needs the EBOOT of the "
                     f"{target} update: EBOOT.elf, the decrypted form of EBOOT.BIN in the update's USRDIR folder "
                     f"({update_usrdir(update)}), not the disc's")
    elif ver:
        r.fail("E1", f"this EBOOT is the {ver} one ({size:,} bytes); this build is for the {target} EBOOT")
    else:
        r.warn("E1", f"decrypted EBOOT has an unknown size ({size:,} bytes): not 1.00 or 1.14; the lift fixups may not apply")


def update_usrdir(update):
    """Where the update's EBOOT lies, for the E1 message: <update>/USRDIR, else the usual console/RPCS3 place."""
    usual = os.path.join("dev_hdd0", "game", TITLE_ID, "USRDIR")
    return os.path.join(update, "USRDIR") if update else usual


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


GETTING_FILES = "https://github.com/undecuple/project-helghast/blob/main/docs/GETTING-FILES.md"
INPUT_CODES = {"disc": ("D", "I", "A"), "update": ("U",), "eboot": ("E", "H")}   # check code letters per input
INPUT_NAMES = {"disc": "the disc dump", "update": "the 1.14 update", "eboot": "the decrypted EBOOT"}
INPUT_LABELS = {"disc": "Disc dump", "update": "1.14 update", "eboot": "EBOOT"}   # each failure's label (issue #12)
MAX_LIST = 400            # folder entries looked at when describing what a wrong folder holds (a drive root can be big)


def _is(d, *rel):
    return os.path.isfile(os.path.join(d, *rel))


def _entries(d):
    """-> (folders, files) names in d, sorted, at most MAX_LIST together; ([], []) when unreadable."""
    try:
        names = sorted(os.listdir(d))[:MAX_LIST]
    except OSError:
        return [], []
    dirs = [n for n in names if os.path.isdir(os.path.join(d, n))]
    return dirs, [n for n in names if n not in dirs]


def _find(names, want):
    """The entry of names equal to want ignoring case (Windows), else ""."""
    return next((n for n in names if n.lower() == want.lower()), "")


def resolve_dump(path):
    """A pick of PS3_GAME or PS3_GAME/USRDIR (one or two levels too deep) -> the folder that holds PS3_GAME, as the
    installer's ResolveDump does; anything else unchanged (the checks then say what is wrong)."""
    if not path or not os.path.isdir(path) or os.path.isdir(os.path.join(path, "PS3_GAME")):
        return path
    p = os.path.normpath(os.path.abspath(path))
    if os.path.basename(p).upper() == "USRDIR" and os.path.basename(os.path.dirname(p)).upper() == "PS3_GAME":
        p = os.path.dirname(p)
    if os.path.basename(p).upper() == "PS3_GAME" and _is(p, "PARAM.SFO"):
        return os.path.dirname(p)
    return path


def resolve_update(path):
    """A pick of the update's USRDIR -> the update folder (the one holding PARAM.SFO); anything else unchanged."""
    if not path or not os.path.isdir(path) or _is(path, "PARAM.SFO"):
        return path
    p = os.path.normpath(os.path.abspath(path))
    if os.path.basename(p).upper() == "USRDIR" and _is(os.path.dirname(p), "PARAM.SFO"):
        return os.path.dirname(p)
    return path


def _sfo_kind(path):
    try:
        sfo = read_sfo(path)
    except (OSError, ValueError, struct.error, IndexError):
        return None
    return sfo.get("TITLE_ID"), sfo.get("CATEGORY")


def is_drive_root(path):
    r"""E:\ or / (the folder is its own parent): issue #13 gave drive roots for both inputs."""
    p = os.path.abspath(path)
    return os.path.dirname(p) == p


def looks_like_disc(d):
    """A folder holding PS3_GAME/PARAM.SFO (a disc dump)."""
    return _is(d, "PS3_GAME", "PARAM.SFO")


def looks_like_update(d):
    """A folder that is an installed update: PARAM.SFO of a non-disc category plus USRDIR/EBOOT.BIN, or the update's
    own data archive (USRDIR/added_data_ps3.psarc.edat, which a disc never has; issue #12)."""
    if _is(d, "USRDIR", UPDATE_EDAT):
        return True
    kind = _sfo_kind(os.path.join(d, "PARAM.SFO")) if _is(d, "PARAM.SFO") else None
    return bool(kind and kind[1] != "DG" and _is(d, "USRDIR", "EBOOT.BIN"))


def looks_like_ps3_game(d):
    """The disc's PS3_GAME folder itself (perhaps renamed): a disc-category PARAM.SFO directly in it."""
    kind = _sfo_kind(os.path.join(d, "PARAM.SFO")) if _is(d, "PARAM.SFO") else None
    return bool(kind and kind[1] == "DG")


def candidates(top, depth):
    """-> (disc folders, update folders) among the subfolders of top, `depth` levels down (1 or 2; never deeper:
    a drive root can be large). At most MAX_LIST entries per folder, three of each kind."""
    discs, updates, level = [], [], [top]
    for _ in range(depth):
        nxt = []
        for d in level:
            for n in _entries(d)[0]:
                sub = os.path.join(d, n)
                if looks_like_disc(sub):
                    discs.append(sub)
                elif looks_like_update(sub):
                    updates.append(sub)
                else:
                    nxt.append(sub)
        level = nxt[:MAX_LIST]
    return discs[:3], updates[:3]


def _hint(path, want):
    """Where the wanted input (disc or update) and the other one seem to be, below path (two levels on a drive root,
    one elsewhere): '; on this drive the disc dump looks like X and the 1.14 update like Y' or ''."""
    root = is_drive_root(path)
    discs, updates = candidates(path, 2 if root else 1)
    if not discs and not updates:
        return ""
    where = "on this drive" if root else "one folder down"
    parts = []
    first, second = (discs, updates) if want == "disc" else (updates, discs)
    names = {"disc": "the disc dump", "update": "the 1.14 update"}
    other = "update" if want == "disc" else "disc"
    if first:
        parts.append(f"{names[want]} looks like {' or '.join(first)}: pick that folder")
    if second:
        parts.append(f"{names[other]} looks like {' or '.join(second)} (give it as the {other})")
    return f"; {path} is a drive root" * root + f"; {where}, " + "; ".join(parts)


def disc_found(path):
    """-> (what the disc input path holds, structural): structural = the input is the wrong thing altogether, so
    the D1-D3 messages add nothing."""
    if not os.path.exists(path):
        return f"nothing is there (no such file or folder: {path})", True
    if os.path.isfile(path):
        return f"the file {os.path.basename(path)}, read as a disc ISO", False
    dirs, files = _entries(path)
    game = _find(dirs, "PS3_GAME")
    if game:
        need = [os.path.join("PS3_GAME", "PARAM.SFO"), os.path.join("PS3_GAME", "USRDIR", "EBOOT.BIN"),
                os.path.join("PS3_GAME", "USRDIR", "data_ps3.psarc")]
        miss = [n for n in need[:2] if not _is(path, n)]
        if not psarc_info(os.path.join(path, need[2]))[0]:
            miss.append(need[2])
        if miss:
            return f"{path} holds PS3_GAME, but not " + " or ".join(miss), False
        return f"{path} holds PS3_GAME with PARAM.SFO, EBOOT.BIN and data_ps3.psarc", False
    if looks_like_update(path):       # issue #12: the update folder given as the disc
        return (f"{path} looks like the 1.14 update (PARAM.SFO, USRDIR{os.sep}EBOOT.BIN, USRDIR{os.sep}{UPDATE_EDAT}), "
                "not the disc dump: give it as the 1.14 update, and the folder that holds PS3_GAME as the disc dump"), True
    what = f"{path} holds no PS3_GAME folder"
    if looks_like_ps3_game(path):
        return what + "; it holds a disc PARAM.SFO directly, so it looks like the PS3_GAME folder under another name", True
    hint = _hint(path, "disc")
    isos = [n for n in files if n.lower().endswith(".iso")]
    if hint:
        what += hint
    elif isos:
        what += f"; it holds the ISO file {isos[0]}: pick that file"
    elif not dirs and not files:
        what += " (the folder is empty)"
    else:
        what += f" ({len(dirs)} folders, {len(files)} files)"
    return what, True


def update_found(path):
    if not os.path.exists(path):
        return f"nothing is there (no such folder: {path})", True
    if os.path.isfile(path):
        return f"{path} is a file, not a folder" + (
            " (an update package, not an installed update)" if path.lower().endswith(".pkg") else ""), True
    dirs, files = _entries(path)
    if looks_like_disc(path) or _find(dirs, "PS3_GAME"):     # issue #12 the other way round
        return (f"{path} holds PS3_GAME, so it looks like the disc dump, not the update: give it as the disc dump, "
                "and the installed update folder as the 1.14 update"), True
    if looks_like_ps3_game(path):
        return (f"{path} holds the disc's PARAM.SFO (category DG), so it looks like the disc's PS3_GAME folder, not "
                "the update: give the folder above it as the disc dump, and the installed update folder as the 1.14 "
                "update"), True
    have = [n for n in ("PARAM.SFO", os.path.join("USRDIR", "EBOOT.BIN"), os.path.join("USRDIR", UPDATE_EDAT))
            if _is(path, n)]
    if not have:
        what = f"{path} holds none of these files"
        hint = _hint(path, "update")
        pkgs = [n for n in files if n.lower().endswith(".pkg")]
        if hint:
            what += hint
        elif pkgs:
            what += f"; it holds the package {pkgs[0]}, which is not an installed update"
        elif not dirs and not files:
            what += " (the folder is empty)"
        else:
            what += f" ({len(dirs)} folders, {len(files)} files)"
        return what, True
    return f"{path} holds " + ", ".join(have), False


def eboot_found(path):
    if not path or not os.path.exists(path):
        return f"no file at {path}: EBOOT.elf is missing", True
    if os.path.isdir(path):
        return f"{path} is a folder, not the EBOOT.elf file", True
    return f"the file {path} ({os.path.getsize(path):,} bytes)", False


DISC_NEED = ("It must be the folder that holds PS3_GAME (with PS3_GAME{s}PARAM.SFO, PS3_GAME{s}USRDIR{s}EBOOT.BIN and "
             "PS3_GAME{s}USRDIR{s}data_ps3.psarc), the PS3_GAME folder itself, or a decrypted .iso file picked as a file")
UPDATE_NEED = ("It must be the installed 1.14 update folder (dev_hdd0{s}game{s}<title id>), holding PARAM.SFO, "
               "USRDIR{s}EBOOT.BIN and USRDIR{s}" + UPDATE_EDAT)
EBOOT_NEED = "It must be EBOOT.elf, the decrypted 1.14 EBOOT of your own copy (see " + GETTING_FILES + ")"


def input_reasons(r, dump, update, eboot):
    """Issues #8 and #10: one line per failing input (which input, the path, what is there, what it must hold, its
    FAIL messages unless the input is the wrong thing altogether), the other checks' FAIL messages, and which inputs
    passed. r.facts["reasons"]; firstrun.py shows them in the setup's failure box (REASON lines) and the console."""
    fails = [c for c in r.checks if c["level"] == "FAIL"]
    if not fails:
        return []
    of = {k: [c for c in r.checks if c["code"][:1] in v] for k, v in INPUT_CODES.items()}
    found = {"disc": lambda: disc_found(dump), "update": lambda: update_found(update), "eboot": lambda: eboot_found(eboot)}
    need = {"disc": DISC_NEED, "update": UPDATE_NEED, "eboot": EBOOT_NEED}
    lines, passed = [], []
    for k in ("disc", "update", "eboot"):
        bad = [c["message"] for c in of[k] if c["level"] == "FAIL"]
        if not bad:
            if of[k] and (k != "update" or update):
                passed.append(INPUT_NAMES[k])
            continue
        what, structural = found[k]()
        line = f"{INPUT_LABELS[k]} failed: {what}. " + need[k].format(s=os.sep) + "."
        if not structural:
            line += " Found: " + "; ".join(bad) + "."
        lines.append(line)
    codes = tuple(x for v in INPUT_CODES.values() for x in v)
    lines += [c["message"] for c in fails if not c["code"].startswith(codes)]
    if passed:
        lines.append("Passed: " + (", ".join(passed[:-1]) + " and " if len(passed) > 1 else "") + passed[-1] + ".")
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", required=True)
    ap.add_argument("--update")
    ap.add_argument("--eboot")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--min-psarc-bytes", type=int, default=MIN_PSARC_BYTES)
    ap.add_argument("--hashes", default=HASHES_FILE)
    ap.add_argument("--psarc-hashes", default=PSARC_HASHES_FILE)
    ap.add_argument("--progress", action="store_true", help="print ARCHIVE-HASH progress lines while A1 hashes")
    ap.add_argument("--skip-archive-hash", action="store_true", help="leave A1 (the archive's SHA-256) out")
    ap.add_argument("--root", help="the install folder; refused (O1) when inside OneDrive")
    ap.add_argument("--build", help="the build folder; refused (O1) when inside OneDrive")
    ap.add_argument("--target", choices=("1.00", "1.14"), default="1.14",
                    help="the EBOOT version the build is for (firstrun.py passes its tree's; pass 12)")
    a = ap.parse_args(argv)
    r = Report()
    dump, upd = resolve_dump(a.dump), resolve_update(a.update)
    if dump != a.dump:
        r.ok("D0", f"INFO: {a.dump} is inside the dump; using the folder that holds PS3_GAME: {dump}")
    if upd != a.update:
        r.ok("U0", f"INFO: {a.update} is the update's USRDIR; using the update folder: {upd}")
    a.dump, a.update = dump, upd
    check_onedrive_tree(r, a.root, a.build)
    # before anything is read: opening an online-only placeholder would start a download of the 9 GB archive
    iso = os.path.isfile(a.dump)     # a decrypted disc ISO instead of a folder
    if check_online_only(r, a.dump if os.path.exists(a.dump) else "", a.update if a.update and os.path.isdir(a.update) else "",
                         a.eboot or ""):
        return finish(r, a)
    if iso:
        check_dump_iso(r, a.dump, a.min_psarc_bytes)
    elif not os.path.isdir(a.dump):
        r.fail("D0", f"dump folder not found: {a.dump}")
    else:
        check_dump(r, a.dump, a.min_psarc_bytes)
        psarc = os.path.join(a.dump, "PS3_GAME", "USRDIR", "data_ps3.psarc")
        if (not a.skip_archive_hash and psarc_info(psarc)[0] and not psarc_info(psarc)[1]
                and not any(c["code"] == "D3" and c["level"] == "FAIL" for c in r.checks)):
            def prog(done, total):
                print(f"ARCHIVE-HASH done={done} total={total}", flush=True)
            check_psarc_hash(r, psarc, (r.facts.get("disc") or {}).get("TITLE_ID"), a.psarc_hashes, a.build,
                             prog if a.progress else None)
    if a.update:
        if not os.path.isdir(a.update):
            r.fail("U0", f"update folder not found: {a.update}")
        else:
            check_update(r, a.update, (r.facts.get("disc") or {}).get("TITLE_ID"))
    else:
        r.warn("U0", "no --update given: the 1.14 update is required (docs/requirements.md)")
    elf = a.eboot
    if not elf:
        # RPCS3 writes EBOOT.elf next to EBOOT.BIN; a dump decrypted some other way may hold an ELF as EBOOT.BIN.
        cand = [os.path.join(a.update, "USRDIR", n) for n in ("EBOOT.elf", "EBOOT.BIN")] if a.update else []
        if not iso:
            cand += [os.path.join(a.dump, "PS3_GAME", "USRDIR", n) for n in ("EBOOT.elf", "EBOOT.BIN")]
        cand = cand or [os.path.join(os.path.dirname(a.dump), "EBOOT.elf")]
        elf = next((c for c in cand if os.path.isfile(c) and head(c, 4) == ELF_MAGIC), cand[0])
    check_eboot_elf(r, elf, a.target, a.update)
    check_eboot_hash(r, elf, a.hashes)
    r.facts["reasons"] = input_reasons(r, a.dump, a.update, elf)
    return finish(r, a)


def finish(r, a):
    if a.json:
        print(json.dumps({"ok": r.passed, "checks": r.checks, "facts": r.facts}, indent=2))
    else:
        for c in r.checks:
            print(f"{c['level']:4} {c['code']}: {c['message']}")
        print("first-run checks:", "pass" if r.passed else "FAIL")
        for ln in r.facts.get("reasons", []):
            print(f"  {ln}")
    return 0 if r.passed else 1


if __name__ == "__main__":
    sys.exit(main())
