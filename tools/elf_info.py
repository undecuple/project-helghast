#!/usr/bin/env python3
"""Inventory of a decrypted PS3 PPU ELF (EBOOT.elf).

    elf_info.py ELF [--imports FILE.tsv] [--spu-dir DIR]

Prints: header, program headers, sections, OPD (function descriptor) count, imported
libraries with function counts (from the PRX stub table), and embedded SPU ELF images.
--imports writes library<TAB>nid rows; --spu-dir writes each embedded SPU ELF out.

PS3 specifics used:
  - ELF64 big-endian, e_machine 21 (PPC64). Pointers inside data are 32-bit.
  - PT 0x60000001 = process param, PT 0x60000002 = PRX param:
      u32 size, u32 magic 0x1b434cec, u32 version, u32 unk, u32 libent_start, u32 libent_end,
      u32 libstub_start, u32 libstub_end, ...
  - Import stub entry (0x2c bytes): u8 size, u8 pad, u16 version, u16 attr, u16 num_func, u16 num_var,
      u16 num_tlsvar, u8 hash, u8 tlshash, u8[2] pad, u32 name, u32 func_nids, u32 func_stubs, ...
  - .opd entries are 8 bytes (u32 code address, u32 TOC).
  - Embedded SPU programs are ELF32 big-endian with e_machine 23 (SPU).
"""
import argparse, os, struct

PT_NAMES = {1: "LOAD", 7: "TLS", 0x60000001: "PROC_PARAM", 0x60000002: "PRX_PARAM", 0x700000a4: "SCE_PPURELA"}


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
            raise SystemExit("not an ELF64 big-endian file (decrypt the SELF first)")
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, _, _, self.phentsize,
         self.phnum, self.shentsize, self.shnum, self.shstrndx) = struct.unpack(">HHIQQQIHHHHHH", d[16:64])
        self.ph = []
        for i in range(self.phnum):
            o = self.phoff + i * self.phentsize
            t, fl, off, va, pa, fsz, msz, al = struct.unpack(">IIQQQQQQ", d[o:o + 56])
            self.ph.append(dict(type=t, flags=fl, off=off, va=va, filesz=fsz, memsz=msz))
        self.sh = []
        if self.shnum and self.shoff:
            for i in range(self.shnum):
                o = self.shoff + i * self.shentsize
                nm, t, fl, addr, off, sz = struct.unpack(">IIQQQQ", d[o:o + 40])
                self.sh.append(dict(name_off=nm, type=t, addr=addr, off=off, size=sz))
            if self.shstrndx < len(self.sh):
                so = self.sh[self.shstrndx]["off"]
                for s in self.sh:
                    e = d.index(b"\0", so + s["name_off"])
                    s["name"] = d[so + s["name_off"]:e].decode(errors="replace")

    def va2off(self, va):
        for p in self.ph:
            if p["type"] == 1 and p["va"] <= va < p["va"] + p["filesz"]:
                return p["off"] + va - p["va"]
        return None

    def u32(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack(">I", self.d[o:o + 4])[0]

    def cstr(self, va):
        o = self.va2off(va)
        if o is None:
            return None
        return self.d[o:self.d.index(b"\0", o)].decode(errors="replace")

    def imports(self):
        prx = next((p for p in self.ph if p["type"] == 0x60000002), None)
        if not prx:
            return []
        o = prx["off"]
        size, magic, _, _, ent_s, ent_e, stub_s, stub_e = struct.unpack(">8I", self.d[o:o + 32])
        if magic != 0x1B434CEC:
            raise SystemExit(f"unexpected PRX param magic {magic:#x}")
        out, va = [], stub_s
        while va < stub_e:
            so = self.va2off(va)
            esz = self.d[so]
            num_func, num_var = struct.unpack(">HH", self.d[so + 6:so + 10])
            name_va, fnid_va = struct.unpack(">II", self.d[so + 16:so + 24])
            nids = [self.u32(fnid_va + 4 * i) for i in range(num_func)]
            out.append((self.cstr(name_va), num_func, num_var, nids))
            va += esz or 0x2C
        return out

    def spu_images(self):
        res, i = [], 0
        while True:
            i = self.d.find(b"\x7fELF\x01\x02\x01", i)
            if i < 0:
                return res
            if struct.unpack(">H", self.d[i + 18:i + 20])[0] == 23:
                phoff, = struct.unpack(">I", self.d[i + 28:i + 32])
                phnum, = struct.unpack(">H", self.d[i + 44:i + 46])
                end = i
                for k in range(phnum):
                    po = i + phoff + 32 * k
                    _, off, _, _, fsz = struct.unpack(">IIIII", self.d[po:po + 20])
                    end = max(end, i + off + fsz)
                shoff, = struct.unpack(">I", self.d[i + 32:i + 36])
                shnum, = struct.unpack(">H", self.d[i + 48:i + 50])
                if shoff and shnum and shoff < 0x100000:
                    end = max(end, i + shoff + 40 * shnum)
                res.append((i, end - i))
            i += 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--imports")
    ap.add_argument("--spu-dir")
    a = ap.parse_args()
    e = Elf(a.elf)
    print(f"type {e.type:#x}  machine {e.machine}  entry {e.entry:#x}  size {len(e.d)}")
    print("\nprogram headers:")
    for p in e.ph:
        print(f"  {PT_NAMES.get(p['type'], hex(p['type'])):12} va {p['va']:#010x} filesz {p['filesz']:#x} memsz {p['memsz']:#x} flags {p['flags']:#x}")
    print(f"\nsections: {len(e.sh)}")
    for s in e.sh:
        if s.get("name"):
            print(f"  {s['name']:24} addr {s['addr']:#010x} size {s['size']:#x}")
    opd = next((s for s in e.sh if s.get("name") == ".opd"), None)
    if opd:
        print(f"\n.opd descriptors: {opd['size'] // 8}")
    imps = e.imports()
    print(f"\nimported libraries: {len(imps)}  functions: {sum(x[1] for x in imps)}  variables: {sum(x[2] for x in imps)}")
    for name, nf, nv, _ in sorted(imps):
        print(f"  {name:28} {nf:4} funcs {nv:3} vars")
    if a.imports:
        with open(a.imports, "w") as f:
            for name, _, _, nids in imps:
                for n in nids:
                    f.write(f"{name}\t{n:#010x}\n")
    spus = e.spu_images()
    print(f"\nembedded SPU ELF images: {len(spus)}  total {sum(s for _, s in spus)} bytes")
    if a.spu_dir:
        os.makedirs(a.spu_dir, exist_ok=True)
        for off, sz in spus:
            open(os.path.join(a.spu_dir, f"spu_{off:08x}.elf"), "wb").write(e.d[off:off + sz])


if __name__ == "__main__":
    main()
