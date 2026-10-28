#!/usr/bin/env python3
"""Leads for "which PPU code submits which SPU program": find every reference to a KZ3 SPU image in the EBOOT.

    job_refs.py [--out work/spu/job_refs.tsv]

Targets: the 75 job modules (work/analysis/jobmods.tsv, true start = offset - 0xC) and the 22 SPU ELFs
(work/bin/spu/*.elf), each at VA = file offset + 0x10000 (first LOAD segment).
Searches:
  data  every aligned 32-bit word in both LOAD segments whose value lies in [image start, image end]
        (words inside the SPU images themselves are skipped);
  code  `lis rD,hi` followed within 16 instructions by `addi/ori rX,rD,lo` (or a load/store through rD) that
        builds an address inside an image, in the PPU code range (< 0xdf5710);
  toc   `lwz/ld rX,d(r2)` / `addi rX,r2,d` (TOC 0x132c960) whose TOC slot holds such an address;
  ref2  code (as above, lis/addi or TOC) that addresses a data hit's word or the 0x40 bytes before it
        (a descriptor/table holding the image pointer).
Code addresses are mapped to work/analysis/functions.json. Output: one row per hit.
KZ3_EBOOT_VER=114 reads the 1.14 set instead (tools/spu/kz3ver.py: EBOOT-114.elf, work/analysis-114, TOC 0x139FE40).
"""
import argparse, bisect, json, os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kz3ver  # noqa: E402  KZ3_EBOOT_VER=100|114
EBOOT = kz3ver.EBOOT
TOC = kz3ver.TOC                # v1.00 0x132C960, 1.14 0x139FE40
CODE_END = kz3ver.PPU_CODE_END  # v1.00 0xDF5710, 1.14 0xE753F4


def segments(d):
    phoff, = struct.unpack(">Q", d[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
    segs = []
    for i in range(phnum):
        p = d[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        typ, flags, off, va, pa, fsz, msz, al = struct.unpack(">IIQQQQQQ", p)
        if typ == 1 and fsz:
            segs.append((va, off, fsz, flags))
    return segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(kz3ver.SPU_OUT, "job_refs.tsv"))
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=VA:LEN",
                    help="also look for references to this address range (e.g. a workload name string)")
    a = ap.parse_args()
    d = open(EBOOT, "rb").read()
    segs = segments(d)

    def va2off(va):
        for sva, off, fsz, _ in segs:
            if sva <= va < sva + fsz:
                return off + va - sva
        return None

    def rd32(va):
        o = va2off(va)
        return struct.unpack(">I", d[o:o + 4])[0] if o is not None else None

    # targets: (start_va, end_va, name)
    T = []
    rows = [l.split("\t") for l in open(kz3ver.JOBTSV).read().splitlines()[1:] if l]
    starts = [int(r[0], 16) - 0xC for r in rows]
    for i, s in enumerate(starts):
        foot = struct.unpack(">I", d[s + 0x14:s + 0x18])[0]
        n = min(foot, (starts[i + 1] if i + 1 < len(starts) else s + foot) - s)
        T.append((s + 0x10000, s + 0x10000 + n, f"job_{s:08x}"))
    for fn in sorted(os.listdir(kz3ver.SPU_DIR)):
        if fn.endswith(".elf"):
            off = int(fn[4:12], 16)
            T.append((off + 0x10000, off + 0x10000 + os.path.getsize(os.path.join(kz3ver.SPU_DIR, fn)),
                      f"elf_{off:08x}"))
    for e in a.extra:
        nm, rng = e.split("=")
        va, ln = rng.split(":")
        T.append((int(va, 16), int(va, 16) + int(ln, 0), nm))
    T.sort()
    tstarts = [t[0] for t in T]

    def target(v):
        i = bisect.bisect_right(tstarts, v) - 1
        if i >= 0 and T[i][0] <= v <= T[i][1]:
            # an address exactly at an image end that is also the next image's start counts as that start
            if v == T[i][1] and i + 1 < len(T) and T[i + 1][0] == v:
                return T[i + 1], 0
            return T[i], v - T[i][0]
        return None, None

    funcs = json.load(open(kz3ver.FUNCTIONS))
    fstart = sorted(int(f["start"], 16) for f in funcs)

    def func_of(va):
        i = bisect.bisect_right(fstart, va) - 1
        return f"{fstart[i]:#010x}" if i >= 0 else "?"

    hits = []
    # data words
    data_hits = {}
    for sva, off, fsz, _ in segs:
        for o in range(0, fsz - 3, 4):
            v = struct.unpack(">I", d[off + o:off + o + 4])[0]
            if not (tstarts[0] <= v <= T[-1][1]):
                continue
            t, rel = target(v)
            if t is None:
                continue
            loc = sva + o
            inside, _ = target(loc)
            if inside is not None:
                continue
            data_hits[loc] = (t, rel)
            hits.append(("data", loc, "-", t[2], rel, f"word at {loc:#x} = {v:#x}"))
    # code: a per-function register-value tracker (lis/li/addi/ori/mr, loads through r2 or a known base). PS3
    # GCC's -mminimal-toc code loads a per-object constant pool pointer from the TOC into a register (often
    # r30) and then loads constants through it, so following one level of loaded pointers matters.
    text_va, text_off = segs[0][0], segs[0][1]
    fset = set(fstart)
    regs = {2: TOC}
    code_hits_addr = []   # (insn_va, value or address, how)
    for va in range(text_va, CODE_END, 4):
        if va in fset:
            regs = {2: TOC}
        w = struct.unpack(">I", d[text_off + va - text_va:text_off + va - text_va + 4])[0]
        op = w >> 26
        rD, rA, rB = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        simm = w & 0xFFFF
        simm -= 0x10000 if simm & 0x8000 else 0
        if op == 15:                                              # addis / lis
            base = 0 if rA == 0 else regs.get(rA)
            if base is None:
                regs.pop(rD, None)
            else:
                regs[rD] = (base + (simm << 16)) & 0xFFFFFFFF
        elif op == 14:                                            # addi / li
            base = 0 if rA == 0 else regs.get(rA)
            if base is None:
                regs.pop(rD, None)
            else:
                regs[rD] = (base + simm) & 0xFFFFFFFF
                if rA:
                    code_hits_addr.append((va, regs[rD], "addi" if rA != 2 else "addi r2"))
        elif op == 24:                                            # ori rA, rS(=rD field), uimm
            if rD in regs:
                regs[rA] = regs[rD] | (w & 0xFFFF)
                code_hits_addr.append((va, regs[rA], "ori"))
            else:
                regs.pop(rA, None)
        elif op == 31 and (w >> 1) & 0x3FF == 444 and rD == rB:   # mr rA, rS
            if rD in regs:
                regs[rA] = regs[rD]
            else:
                regs.pop(rA, None)
        elif op in (32, 58, 34, 40, 42, 48, 50, 36, 38, 44, 52, 54, 33, 35, 41, 37, 39, 45):   # D/DS loads, stores
            disp = (simm & ~3) if op == 58 else simm
            if rA in regs:
                addr = (regs[rA] + disp) & 0xFFFFFFFF
                code_hits_addr.append((va, addr, f"access {addr:#x}"))
                if op in (32, 58) and not (op == 58 and w & 3):  # lwz / ld: follow the loaded pointer
                    v = rd32(addr + 4) if op == 58 else rd32(addr)
                    if v is not None:
                        code_hits_addr.append((va, v, f"load [{addr:#x}]"))
                        regs[rD] = v
                    else:
                        regs.pop(rD, None)
                elif op in (34, 40, 42, 33, 35, 41) or (op == 58 and w & 3):
                    regs.pop(rD, None)
            elif op in (32, 58, 34, 40, 42, 33, 35, 41):          # GPR loads with unknown base
                regs.pop(rD, None)
            if op in (33, 35, 41, 37, 39, 45):                    # update forms also write rA
                regs.pop(rA, None)
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in (28, 60, 124, 284, 316, 412, 476, 24, 536, 792, 824, 26, 922, 954, 986, 27, 539, 794, 413,
                      58, 444):
                regs.pop(rA, None)                                # logical/shift: destination is rA
            elif xo not in (151, 215, 407, 150, 183, 662, 918, 149, 181, 0, 32):   # stores, cmp: no GPR write
                regs.pop(rD, None)
        elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):      # rotates, logical immediates: write rA
            if op == 25 and rD in regs:                           # oris
                regs[rA] = regs[rD] | ((w & 0xFFFF) << 16)
                code_hits_addr.append((va, regs[rA], "oris"))
            else:
                regs.pop(rA, None)
        elif op in (7, 8, 12, 13):                                # mulli, subfic, addic(.)
            regs.pop(rD, None)
        elif op == 18 and w & 1:                                  # bl: volatile GPRs are clobbered
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                regs.pop(r, None)
        elif op == 19 and w & 1:                                  # bctrl / bclrl
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                regs.pop(r, None)
        regs[2] = TOC
    for va, val, how in code_hits_addr:
        t, rel = target(val)
        if t is not None:
            hits.append(("code", va, func_of(va), t[2], rel, f"{how} -> {val:#x}"))
    # second level: code addressing a data hit (or up to 0x40 bytes before it: struct base)
    dh = sorted(data_hits)
    for va, val, how in code_hits_addr:
        i = bisect.bisect_left(dh, val)
        if i < len(dh) and 0 <= dh[i] - val <= 0x40:
            t, rel = data_hits[dh[i]]
            hits.append(("ref2", va, func_of(va), t[2], rel, f"{how} -> {val:#x} (+{dh[i] - val:#x} holds image ptr)"))
    hits.sort(key=lambda h: (h[3], h[0], h[1]))
    with open(a.out, "w") as f:
        f.write("kind\tva\tfunction\timage\toffset_in_image\tdetail\n")
        for k, va, fn, img, rel, det in hits:
            f.write(f"{k}\t{va:#010x}\t{fn}\t{img}\t{rel:#x}\t{det}\n")
    # Summary: strong evidence = code that loads or builds the image's exact start address.
    names = [t[2] for t in T]
    strong = {n: sorted({h[2] for h in hits if h[3] == n and h[0] == "code" and h[4] == 0}) for n in names}
    print(f"images {len(names)}; with code loading/building the exact start address: "
          f"{sum(1 for n in names if strong[n])}")
    for n in names:
        if strong[n]:
            print(f"  {n}: functions {' '.join(strong[n])}")
    print("no static reference to the start address: " + " ".join(n for n in names if not strong[n]))


if __name__ == "__main__":
    main()
