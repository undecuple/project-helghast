#!/usr/bin/env python3
"""Find and dump WWS SPU job modules embedded in a PPU ELF (or any blob).

    jobmods.py FILE [--out DIR] [--tsv FILE]

A job module starts with a 24-byte header whose last word is the magic 0xC0DEC0DE:
    +0x00 ila instruction (top byte 0x42/0x43)   +0x04 entry offset (0x30 seen)
    +0x08 image size (code+rodata)              +0x0C, +0x10 two more sizes (ASSUMPTION: bss / stack or
    +0x14 0xC0DEC0DE                                   job buffer sizes; not yet confirmed)
Format per ps3recomp tools/extract_jobmods.py (which reads LBP's SDATA archives; KZ3 embeds them in the EBOOT).
"""
import argparse, hashlib, os, struct

MAGIC = b"\xC0\xDE\xC0\xDE"


def scan(d):
    out, i = [], 0
    while True:
        i = d.find(MAGIC, i)
        if i < 0:
            return out
        start = i - 0x14
        if start >= 0 and d[start] in (0x42, 0x43):
            ila, entry, size, w3, w4 = struct.unpack(">5I", d[start:start + 0x14])
            if 0 < size < 0x40000 and entry < size:
                out.append((start, entry, size, w3, w4))
        i += 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--out")
    ap.add_argument("--tsv")
    a = ap.parse_args()
    d = open(a.file, "rb").read()
    mods = scan(d)
    total = sum(m[2] for m in mods)
    print(f"job modules: {len(mods)}  image bytes: {total}")
    rows = []
    for start, entry, size, w3, w4 in mods:
        img = d[start:start + size]
        sha = hashlib.sha1(img).hexdigest()[:12]
        rows.append(f"{start:#010x}\t{entry:#x}\t{size}\t{w3:#x}\t{w4:#x}\t{sha}")
        if a.out:
            os.makedirs(a.out, exist_ok=True)
            open(os.path.join(a.out, f"jobmod_{start:08x}_{sha}.bin"), "wb").write(img)
    if a.tsv:
        open(a.tsv, "w").write("file_offset\tentry\timage_size\tw3\tw4\tsha1_12\n" + "\n".join(rows) + "\n")
    sizes = sorted(m[2] for m in mods)
    if sizes:
        print(f"size min {sizes[0]}  median {sizes[len(sizes) // 2]}  max {sizes[-1]}")
        print(f"file offsets {mods[0][0]:#x} .. {mods[-1][0]:#x}")


if __name__ == "__main__":
    main()
