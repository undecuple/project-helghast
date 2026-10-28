#!/usr/bin/env python3
"""Test helper (stdlib only): write a tiny ISO 9660 image from a dict of files, for the release host tests. Not shipped.

    build_iso(path, {"PS3_GAME/PARAM.SFO": b"...", ...}, multi={"PS3_GAME/USRDIR/BIG.BIN": 4096}, joliet=False)

`multi` maps a file to an extent size: the file is stored as several extents chained with the 0x80 flag (ISO 9660
level 3). Names are upper-cased with a ";1" suffix, like a mastering tool does. No game data is involved.
"""
import struct

SECTOR = 2048


def _both(v, n=4):
    f = "<I" if n == 4 else "<H"
    g = ">I" if n == 4 else ">H"
    return struct.pack(f, v) + struct.pack(g, v)


def _rec(lba, size, flags, name):
    nl = len(name)
    ln = 33 + nl + ((33 + nl) % 2)                           # pad byte so the record length is even
    b = bytearray(ln)
    b[0] = ln
    b[2:10] = _both(lba)
    b[10:18] = _both(size)
    b[18:25] = bytes([126, 1, 1, 0, 0, 0, 0])
    b[25] = flags
    b[28:32] = _both(1, 2)
    b[32] = nl
    b[33:33 + nl] = name
    return bytes(b)


def _tree(files):
    root = {}
    for p, data in files.items():
        parts = p.split("/")
        d = root
        for x in parts[:-1]:
            d = d.setdefault(x, {})
        d[parts[-1]] = data
    return root


def build_iso(path, files, multi=None, joliet=False, volume_sectors=None, system_area=b""):
    multi = multi or {}
    root = _tree(files)
    dirs = []                       # (node, path)

    def collect(node, p):
        dirs.append((node, p))
        for n, v in sorted(node.items()):
            if isinstance(v, dict):
                collect(v, p + n + "/")
    collect(root, "")

    def nm(n, is_dir, joli):
        if joli:
            return n.encode("utf-16-be")
        return n.upper().encode() + (b"" if is_dir else b";1")

    def build_dir(node, p, lba_of, file_lba, parent_p, joli, szs=None):
        szs = szs or {}
        recs = [_rec(lba_of[p], szs.get(p, 0) * SECTOR, 2, b"\0"), _rec(lba_of[parent_p], szs.get(parent_p, 0) * SECTOR, 2, b"\1")]
        for n, v in sorted(node.items()):
            if isinstance(v, dict):
                recs.append(_rec(lba_of[p + n + "/"], szs.get(p + n + "/", 0) * SECTOR, 2, nm(n, True, joli)))
            else:
                ext = file_lba[p + n]
                for i, (lba, size) in enumerate(ext):
                    recs.append(_rec(lba, size, 0x80 if i < len(ext) - 1 else 0, nm(n, False, joli)))
        out, cur = [], b""
        for r in recs:
            if len(cur) + len(r) > SECTOR:
                out.append(cur.ljust(SECTOR, b"\0"))
                cur = b""
            cur += r
        out.append(cur.ljust(SECTOR, b"\0"))
        return b"".join(out)

    def sizes(joli):
        z = {p: 0 for _, p in dirs}
        zf = {}
        for node, p in dirs:
            for n, v in node.items():
                if not isinstance(v, dict):
                    zf[p + n] = [(0, 0)] * max(1, -(-len(v) // multi[p + n]) if (p + n) in multi else 1)
        return {p: len(build_dir(node, p, z, zf, "", joli)) // SECTOR for node, p in dirs}

    lba = 19 if joliet else 18
    layout = []
    for joli in ([False, True] if joliet else [False]):
        szs = sizes(joli)
        lba_of = {}
        for _, p in dirs:
            lba_of[p] = lba
            lba += szs[p]
        layout.append((joli, szs, lba_of))
    # file data after all directories (shared by the Joliet tree)
    file_lba, blobs = {}, []
    for node, p in dirs:
        for n, v in sorted(node.items()):
            if isinstance(v, dict):
                continue
            step = multi.get(p + n, max(len(v), 1))
            ext = []
            for off in range(0, max(len(v), 1), step):
                chunk = v[off:off + step]
                ext.append((lba, len(chunk)))
                blobs.append(chunk.ljust(-(-max(len(chunk), 1) // SECTOR) * SECTOR, b"\0"))
                lba += -(-max(len(chunk), 1) // SECTOR)
            file_lba[p + n] = ext
    total = lba if volume_sectors is None else volume_sectors

    def vd(kind, joli, lba_of, szs):
        b = bytearray(SECTOR)
        b[0] = kind
        b[1:6] = b"CD001"
        b[6] = 1
        if joli:
            b[88:91] = b"%/E"
        b[80:88] = _both(total)
        b[120:124] = _both(1, 2)
        b[124:128] = _both(1, 2)
        b[128:132] = _both(SECTOR, 2)
        b[156:190] = _rec(lba_of[""], szs[""] * SECTOR, 2, b"\0")[:34]
        return bytes(b)

    out = bytearray(system_area.ljust(16 * SECTOR, b"\0"))
    joli0, szs0, lba0 = layout[0]
    out += vd(1, False, lba0, szs0)
    if joliet:
        out += vd(2, True, layout[1][2], layout[1][1])
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    out += term
    for joli, szs, lba_of in layout:
        out = out.ljust(lba_of[""] * SECTOR, b"\0")
        for node, p in dirs:
            parent = p[:p[:-1].rfind("/") + 1] if p else ""
            d = build_dir(node, p, lba_of, file_lba, parent, joli, szs)
            out = out.ljust(lba_of[p] * SECTOR, b"\0")
            out += d
    out = out.ljust(layout[-1][2][""] * SECTOR, b"\0")
    first_file = min(l for ext in file_lba.values() for l, _ in ext) if file_lba else len(out) // SECTOR
    out = out.ljust(first_file * SECTOR, b"\0")
    for b in blobs:
        out += b
    with open(path, "wb") as f:
        f.write(out)
    return path
