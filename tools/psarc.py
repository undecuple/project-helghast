#!/usr/bin/env python3
"""PSARC reader (Sony PlayStation archive, v1.4, zlib).

    psarc.py info    ARCHIVE
    psarc.py list    ARCHIVE [--out FILE]
    psarc.py stats   ARCHIVE                  # extension / top-dir histogram
    psarc.py extract ARCHIVE OUTDIR [--match SUBSTR] [--limit N] [--ext .core] [--max-size BYTES] [--dedup-case]

Format (big-endian):
  header 32 B: 'PSAR', u16 major, u16 minor, char[4] compression ('zlib'/'lzma'),
               u32 toc_length, u32 toc_entry_size (30), u32 toc_entries,
               u32 block_size, u32 flags (1 = relative paths, 2 = absolute, 4 = encrypted toc)
  toc entry 30 B: md5[16], u32 first_block, u40 uncompressed_size, u40 offset
  then a table of compressed block sizes, each ceil(log256(block_size)) bytes wide;
  a stored size of 0 means a full, uncompressed block.
  Entry 0 is the manifest: newline-separated paths for entries 1..n.
"""
import argparse, collections, os, struct, sys, zlib, lzma


class Psarc:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(32)
        (magic, self.major, self.minor, self.comp, self.toc_len, self.entry_size,
         self.count, self.block_size, self.flags) = struct.unpack(">4sHH4sIIIII", h)
        if magic != b"PSAR":
            raise SystemExit(f"{path}: not a PSARC")
        if self.flags & 4:
            raise SystemExit("encrypted TOC is not supported")
        toc = self.f.read(self.toc_len - 32)
        self.entries = []
        for i in range(self.count):
            e = toc[i * self.entry_size:(i + 1) * self.entry_size]
            md5 = e[:16]
            block = struct.unpack(">I", e[16:20])[0]
            size = int.from_bytes(e[20:25], "big")
            off = int.from_bytes(e[25:30], "big")
            self.entries.append((md5, block, size, off))
        w = 1
        while 256 ** w < self.block_size:
            w += 1
        tbl = toc[self.count * self.entry_size:]
        self.blocks = [int.from_bytes(tbl[j:j + w], "big") for j in range(0, len(tbl) - w + 1, w)]
        self.names = [""] + self.read(0).decode("utf-8").split("\n")

    def read(self, idx):
        _, block, size, off = self.entries[idx]
        out = bytearray()
        self.f.seek(off)
        while len(out) < size:
            csize = self.blocks[block] or self.block_size
            data = self.f.read(csize)
            want = min(self.block_size, size - len(out))
            if csize == want:
                out += data  # stored
            else:
                try:
                    out += zlib.decompress(data) if self.comp == b"zlib" else lzma.decompress(data)
                except (zlib.error, lzma.LZMAError):
                    out += data  # some writers store raw blocks shorter than block_size
            block += 1
        return bytes(out[:size])


def safe_join(root, name):
    """Reject absolute paths, '..' components and anything escaping root."""
    rel = name.lstrip("/")
    parts = rel.replace("\\", "/").split("/")
    if not rel or any(p in ("", ".", "..") for p in parts):
        raise ValueError(f"unsafe path in archive: {name!r}")
    dest = os.path.realpath(os.path.join(root, *parts))
    if not dest.startswith(os.path.realpath(root) + os.sep):
        raise ValueError(f"path escapes output dir: {name!r}")
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "list", "stats", "extract"])
    ap.add_argument("archive")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--out")
    ap.add_argument("--match")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--ext", help="extract only names ending in this suffix (case-insensitive), e.g. .core")
    ap.add_argument("--max-size", type=int, help="extract only entries up to this many bytes")
    ap.add_argument("--dedup-case", action="store_true", help="skip names that differ only in case from an earlier one")
    a = ap.parse_args()
    p = Psarc(a.archive)

    if a.cmd == "info":
        total = sum(e[2] for e in p.entries[1:])
        print(f"version {p.major}.{p.minor}  compression {p.comp.decode()}  flags {p.flags}")
        print(f"entries {p.count - 1} (+manifest)  block {p.block_size}  blocks {len(p.blocks)}")
        print(f"uncompressed total {total / 2**30:.2f} GiB")
    elif a.cmd == "list":
        out = open(a.out, "w") if a.out else sys.stdout
        for i, n in enumerate(p.names[1:], 1):
            print(f"{p.entries[i][2]}\t{n}", file=out)
    elif a.cmd == "stats":
        ext, top = collections.Counter(), collections.Counter()
        size_by_ext = collections.Counter()
        for i, n in enumerate(p.names[1:], 1):
            e = os.path.splitext(n)[1].lower() or "(none)"
            ext[e] += 1
            size_by_ext[e] += p.entries[i][2]
            top["/".join(n.lstrip("/").split("/")[:2])] += 1
        print("extension\tfiles\tMiB")
        for e, c in ext.most_common():
            print(f"{e}\t{c}\t{size_by_ext[e] / 2**20:.0f}")
        print("\ntop dirs\tfiles")
        for d, c in top.most_common(40):
            print(f"{d}\t{c}")
    else:
        if not a.outdir:
            raise SystemExit("extract needs OUTDIR")
        os.makedirs(a.outdir, exist_ok=True)
        done = 0
        seen = set()
        for i, n in enumerate(p.names[1:], 1):
            if a.match and a.match not in n:
                continue
            if a.ext and not n.lower().endswith(a.ext.lower()):
                continue
            if a.max_size is not None and p.entries[i][2] > a.max_size:
                continue
            if a.dedup_case:
                if n.lower() in seen:
                    continue
                seen.add(n.lower())
            dest = safe_join(a.outdir, n)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as o:
                o.write(p.read(i))
            done += 1
            if a.limit and done >= a.limit:
                break
        print(f"extracted {done} files to {a.outdir}")


if __name__ == "__main__":
    main()
