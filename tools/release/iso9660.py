#!/usr/bin/env python3
"""Minimal read-only ISO 9660 reader for the release tools (standard library only, Python 3.8+): a decrypted PS3 disc
ISO is a plain ISO 9660 image (2048-byte sectors, primary volume descriptor at sector 16, optionally Joliet) holding
PS3_DISC.SFB and PS3_GAME/..., so firstrun_check.py and firstrun.py can read the player's ISO without any extra tool.

    iso9660.py list IMAGE [DIR]                 list a folder inside the image
    iso9660.py extract IMAGE PATH DEST          copy one file out (chunked, prints "COPY done=X total=Y")

    with Iso(path) as iso:
        e = iso.lookup("PS3_GAME/PARAM.SFO")    # case-insensitive, ";1" version suffixes stripped
        data = iso.read("PS3_GAME/PARAM.SFO", 4)
        iso.copy("PS3_GAME/USRDIR/data_ps3.psarc", dest, progress=lambda done, total: ...)

Files with several extents (ISO 9660 level 3: the multi-extent flag 0x80 chains the records of one name) are read as one
file. Every length and sector number is bounds-checked against the image; a malformed or non-ISO file raises IsoError
with a plain message. IsoEncrypted (a subclass) means the image is a raw, encrypted disc image: the PS3 raw layout
starts with a table of encrypted regions in sector 0 (ASSUMPTION: big-endian count at 0, then start/end sector pairs
from offset 8; no sample was at hand, so the table test is conservative), and files whose magic is wrong
(check_magic) are taken as unreadable for the same reason. Nothing is decrypted and nothing is hinted at.
"""
import os
import struct
import sys

SECTOR = 2048
MAX_DIR_BYTES = 64 << 20          # a directory larger than this is not real
MAX_DEPTH = 32
FLAG_DIR = 0x02
FLAG_MULTI = 0x80
ENCRYPTED_MSG = "this ISO is encrypted; use a decrypted dump"


class IsoError(Exception):
    """The file is not a readable ISO 9660 image (message is for the player)."""


class IsoEncrypted(IsoError):
    def __init__(self, msg=ENCRYPTED_MSG):
        super().__init__(msg)


class Entry:
    def __init__(self, name, is_dir, extents):
        self.name = name
        self.is_dir = is_dir
        self.extents = extents                      # [(byte offset in the image, length)], in file order

    @property
    def size(self):
        return sum(n for _, n in self.extents)


def _both32(b, o):
    le, be = struct.unpack_from("<I", b, o)[0], struct.unpack_from(">I", b, o + 4)[0]
    if le != be:
        raise ValueError("both-endian field disagrees")
    return le


def _clean(name):
    """ISO 9660 file name -> plain: no ';version', no trailing '.'."""
    i = name.rfind(";")
    if i > 0 and name[i + 1:].isdigit():
        name = name[:i]
    return name[:-1] if name.endswith(".") and len(name) > 1 else name


class Iso:
    def __init__(self, path):
        self.path = path
        try:
            self.f = open(path, "rb")
            self.f.seek(0, os.SEEK_END)
            self.image_size = self.f.tell()
        except OSError as e:
            raise IsoError(f"cannot read {path}: {e.strerror or e}")
        try:
            self._open()
        except BaseException:
            self.f.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        self.f.close()

    # ------------------------------------------------------------------------------------------------- open
    def _raw(self, off, n):
        if off < 0 or n < 0 or off + n > self.image_size:
            raise IsoError("the image is truncated (a read past its end)")
        self.f.seek(off)
        b = self.f.read(n)
        if len(b) != n:
            raise IsoError("the image is truncated (short read)")
        return b

    def _encrypted_table(self):
        """True if sector 0 holds a PS3 raw-image region table (see the module docstring)."""
        if self.image_size < SECTOR:
            return False
        h = self._raw(0, SECTOR)
        n = struct.unpack_from(">I", h, 0)[0]
        if not 1 <= n <= 200 or 8 + 8 * n > SECTOR:
            return False
        vals = [struct.unpack_from(">I", h, 8 + 4 * i)[0] for i in range(2 * n)]
        return vals[0] == 0 and all(a <= b for a, b in zip(vals, vals[1:])) and vals[-1] > 16 and \
            vals[-1] <= self.image_size // SECTOR + 1

    def _open(self):
        self.suspect = self._encrypted_table()
        if self.image_size < 17 * SECTOR + 7:
            raise IsoEncrypted() if self.suspect else IsoError("this file is too small to be an ISO image")
        pvd = joliet = None
        for sec in range(16, 64):
            if (sec + 1) * SECTOR > self.image_size:
                break
            d = self._raw(sec * SECTOR, SECTOR)
            if d[1:6] != b"CD001":
                break
            if d[0] == 255:
                break
            if d[0] == 1 and pvd is None:
                pvd = d
            elif d[0] == 2 and d[88:91] in (b"%/@", b"%/C", b"%/E") and joliet is None:
                joliet = d
        if pvd is None:
            raise IsoEncrypted() if self.suspect else IsoError("not an ISO image (no ISO 9660 volume descriptor at sector 16)")
        self.volumes = [("iso", pvd)] + ([("joliet", joliet)] if joliet else [])
        self._cache = {}
        self._use(0)
        bs = struct.unpack_from("<H", pvd, 128)[0]
        if bs != SECTOR:
            raise IsoError(f"unsupported ISO block size {bs} (2048 expected)")
        self.truncated_image = _both32(pvd, 80) * SECTOR > self.image_size   # volume space size vs the file
        self.volume_bytes = _both32(pvd, 80) * SECTOR

    def _use(self, k):
        self.mode, vd = self.volumes[k]
        self.vol_index = k
        try:
            self.root = self._record(vd[156:190], 0, vd[156] if vd[156] else 34)
        except (ValueError, struct.error):
            raise self._bad("the root directory record is damaged")
        if not self.root or not self.root.is_dir:
            raise self._bad("the root directory record is damaged")
        self._cache = {}

    def _bad(self, msg):
        return IsoEncrypted() if self.suspect else IsoError(msg)

    # --------------------------------------------------------------------------------------------- records
    def _record(self, b, o, ln):
        if ln < 34 or o + ln > len(b):
            return None
        lba, size = _both32(b, o + 2), _both32(b, o + 10)
        flags, nl = b[o + 25], b[o + 32]
        if 33 + nl > ln:
            return None
        raw = bytes(b[o + 33:o + 33 + nl])
        if nl == 1 and raw in (b"\0", b"\1"):
            name = raw
        elif self.mode == "joliet":
            name = _clean(raw.decode("utf-16-be", "replace"))
        else:
            name = _clean(raw.decode("latin-1"))
        return Entry(name, bool(flags & FLAG_DIR), [(lba * SECTOR, size)]) if not flags & FLAG_MULTI \
            else _Partial(name, lba * SECTOR, size)

    def _entries(self, d):
        """Entries of directory d (an Entry), multi-extent records merged. Cached."""
        key = d.extents[0][0]
        if key in self._cache:
            return self._cache[key]
        off, size = d.extents[0]
        if size > MAX_DIR_BYTES or off + size > self.image_size:
            raise self._bad("a directory points past the end of the image (truncated or damaged)")
        data = self._raw(off, size)
        out, pend, p = [], None, 0
        while p < size:
            ln = data[p]
            if ln == 0:                                   # padding to the next sector
                p = (p // SECTOR + 1) * SECTOR
                continue
            if (p % SECTOR) + ln > SECTOR:
                raise self._bad("a directory record crosses a sector boundary (damaged)")
            try:
                e = self._record(data, p, ln)
            except (ValueError, struct.error):
                e = None
            if e is None:
                raise self._bad("a directory record is damaged")
            p += ln
            if isinstance(e.name, bytes):                 # "." and ".."
                continue
            if isinstance(e, _Partial):
                if pend and pend.name == e.name:
                    pend.extents.append(e.extents[0])
                else:
                    pend = Entry(e.name, False, [e.extents[0]])
                continue
            if pend and pend.name == e.name:
                pend.extents.append(e.extents[0])
                e, pend = pend, None
            elif pend:
                raise self._bad("a multi-extent file ends without its last record")
            out.append(e)
        if pend:
            raise self._bad("a multi-extent file ends without its last record")
        self._cache[key] = out
        return out

    # ---------------------------------------------------------------------------------------------- queries
    def lookup(self, path):
        """-> Entry for 'A/B/C' (either slash, any case); None when absent. '' is the root. A name missing in the
        primary volume is looked up in the Joliet one too (long names the primary tree may have cut)."""
        e = self._lookup(path)
        if e is None and len(self.volumes) > 1 and self.vol_index == 0:
            try:
                self._use(1)
                e = self._lookup(path)
            finally:
                self._use(0)
        return e

    def _lookup(self, path):
        e = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            if not e.is_dir:
                return None
            for c in self._entries(e):
                if c.name.lower() == part.lower():
                    e = c
                    break
            else:
                return None
        return e

    def listdir(self, path=""):
        e = self.lookup(path)
        if e is None or not e.is_dir:
            raise IsoError(f"no folder {path!r} in the image")
        return [c.name for c in self._entries(e)]

    def walk(self, path=""):
        """Yield (relative path with '/', Entry) for every file below `path`."""
        start = self.lookup(path)
        if start is None or not start.is_dir:
            return

        def rec(d, prefix, depth):
            if depth > MAX_DEPTH:
                raise self._bad("folders nested too deep (damaged)")
            for c in self._entries(d):
                rel = prefix + c.name
                if c.is_dir:
                    yield from rec(c, rel + "/", depth + 1)
                else:
                    yield rel, c
        yield from rec(start, "", 0)

    def in_image(self, e):
        """True if every extent of the file lies inside the image file."""
        return all(o + n <= self.image_size for o, n in e.extents)

    def _need(self, path):
        e = self.lookup(path)
        if e is None or e.is_dir:
            raise IsoError(f"{path} is not in the image")
        return e

    def read(self, path, n=None, offset=0):
        """The first n bytes (all if None; only for small files) of a file, from `offset`."""
        e = self._need(path)
        want = e.size - offset if n is None else min(n, e.size - offset)
        out, skip = [], offset
        for off, ln in e.extents:
            if want <= 0:
                break
            if skip >= ln:
                skip -= ln
                continue
            take = min(ln - skip, want)
            out.append(self._raw(off + skip, take))
            want -= take
            skip = 0
        return b"".join(out)

    def copy(self, path, dest, progress=None, chunk=4 << 20):
        """Stream a file to `dest` (written as dest + '.part', then renamed). progress(done, total) per chunk.
        -> bytes written."""
        e = self._need(path)
        total = e.size
        if not self.in_image(e):
            raise IsoError(f"{path} reaches past the end of the image (truncated)")
        part = dest + ".part"
        done = 0
        with open(part, "wb") as out:
            for off, ln in e.extents:
                pos = 0
                while pos < ln:
                    b = self._raw(off + pos, min(chunk, ln - pos))
                    out.write(b)
                    pos += len(b)
                    done += len(b)
                    if progress:
                        progress(done, total)
        os.replace(part, dest)
        return done

    def check_magic(self, path, magic):
        """Read the first bytes of a file: IsoEncrypted when they are not `magic` (the file is unreadable as plain
        data), IsoError when the file is missing."""
        if self.read(path, len(magic)) != magic:
            raise IsoEncrypted()


class _Partial(Entry):
    """A record with the multi-extent flag (not yet the last of its name)."""
    def __init__(self, name, off, size):
        super().__init__(name, False, [(off, size)])


def _main(argv):
    if len(argv) >= 3 and argv[1] == "list":
        with Iso(argv[2]) as iso:
            for n in iso.listdir(argv[3] if len(argv) > 3 else ""):
                print(n)
        return 0
    if len(argv) == 5 and argv[1] == "extract":
        with Iso(argv[2]) as iso:
            iso.copy(argv[3], argv[4], lambda d, t: print(f"COPY done={d} total={t}", flush=True))
        return 0
    print(__doc__.split("\n\n")[1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(_main(sys.argv))
    except IsoError as e:
        print(f"FAIL: {e}")
        sys.exit(1)
