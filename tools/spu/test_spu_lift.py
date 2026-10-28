#!/usr/bin/env python3
"""End-to-end check of ps3recomp's SPU decode + lift + helpers against a big-endian model of the SPU ISA.

    test_spu_lift.py [PATCHED_PS3RECOMP_DIR] [--cases N]     (default work/spu/ps3recomp-patched, 200)

For every op: encode a real SPU instruction word (opcode values from the SPU ISA, not from ps3recomp's tables),
decode it with the patched spu_disasm, translate it with spu_lifter's _translate (or _cond for branch
conditions), compile all statements into one host program with runtime/spu/spu_helpers.h, run random and
edge-case vectors through it, and compare with a Python model written on big-endian bytes. Registers enter and
leave the C side the way spu_ls_read128/write128 move them (word i of the quadword = _u32[i]), so a lane or
byte-order mistake anywhere in decoder, lifter or helper shows up as a mismatch.

Covers the instructions KZ3's SPU code uses (tools/spu/spu_audit.py histogram) except control flow, channels,
hints and the estimate pair frest/frsqest/fi. Float ops are checked against IEEE round-to-nearest on normal
values (what the helpers implement; real SPU singles truncate and have no NaN/Inf/denormals, see the RE notes).
Exit status 0 = every case matches.
"""
import math, os, random, struct, subprocess, sys, tempfile
from fractions import Fraction

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
PS3 = os.path.abspath(args[0]) if args else os.path.join(ROOT, "work", "spu", "ps3recomp-patched")
CASES = int(sys.argv[sys.argv.index("--cases") + 1]) if "--cases" in sys.argv else 200
sys.path.insert(0, os.path.join(PS3, "tools"))
import spu_lifter                       # noqa: E402
from spu_disasm import spu_decode       # noqa: E402

RT, RA, RB, RC = 3, 4, 5, 6
M32 = 0xFFFFFFFF

# ---- instruction encodings (SPU ISA v1.2 opcode tables) ----
RR = {  # op11
    "a": 0x0C0, "sf": 0x040, "ah": 0x0C8, "sfh": 0x048, "and": 0x0C1, "or": 0x041, "xor": 0x241, "nand": 0x0C9,
    "nor": 0x049, "andc": 0x2C1, "orc": 0x2C9, "eqv": 0x249, "ceq": 0x3C0, "ceqh": 0x3C8, "ceqb": 0x3D0,
    "cgt": 0x240, "cgth": 0x248, "cgtb": 0x250, "clgt": 0x2C0, "clgth": 0x2C8, "clgtb": 0x2D0,
    "mpy": 0x3C4, "mpyu": 0x3CC, "mpyh": 0x3C5, "mpys": 0x3C7, "mpyhh": 0x3C6, "mpyhhu": 0x3CE,
    "mpyhha": 0x346, "mpyhhau": 0x34E, "cg": 0x0C2, "bg": 0x042, "addx": 0x340, "sfx": 0x341, "cgx": 0x342,
    "bgx": 0x343, "sumb": 0x253, "absdb": 0x053, "avgb": 0x0D3,
    "shl": 0x05B, "shlh": 0x05F, "rot": 0x058, "roth": 0x05C, "rotm": 0x059, "rotma": 0x05A, "rothm": 0x05D,
    "rotmah": 0x05E, "shlqbi": 0x1DB, "rotqbi": 0x1D8, "rotqmbi": 0x1D9, "shlqby": 0x1DF, "rotqby": 0x1DC,
    "rotqmby": 0x1DD, "shlqbybi": 0x1CF, "rotqbybi": 0x1CC, "rotqmbybi": 0x1CD,
    "cbx": 0x1D4, "chx": 0x1D5, "cwx": 0x1D6, "cdx": 0x1D7,
    "fa": 0x2C4, "fs": 0x2C5, "fm": 0x2C6, "fceq": 0x3C2, "fcgt": 0x2C2, "fcmeq": 0x3CA, "fcmgt": 0x2CA,
    "dfa": 0x2CC, "dfs": 0x2CD, "dfm": 0x2CE,
}
RR1 = {"clz": 0x2A5, "cntb": 0x2B4, "xsbh": 0x2B6, "xshw": 0x2AE, "xswd": 0x2A6, "orx": 0x1F0, "gb": 0x1B0,
       "gbh": 0x1B1, "gbb": 0x1B2, "fsm": 0x1B4, "fsmh": 0x1B5, "fsmb": 0x1B6, "fesd": 0x3B8, "frds": 0x3B9}
RRR = {"selb": 0x8, "shufb": 0xB, "mpya": 0xC, "fnms": 0xD, "fma": 0xE, "fms": 0xF}
RI7 = {"shli": 0x07B, "shlhi": 0x07F, "roti": 0x078, "rothi": 0x07C, "rotmi": 0x079, "rotmai": 0x07A,
       "rothmi": 0x07D, "rotmahi": 0x07E, "shlqbii": 0x1FB, "rotqbii": 0x1F8, "rotqmbii": 0x1F9,
       "shlqbyi": 0x1FF, "rotqbyi": 0x1FC, "rotqmbyi": 0x1FD, "cbd": 0x1F4, "chd": 0x1F5, "cwd": 0x1F6,
       "cdd": 0x1F7}
RI10 = {"ai": 0x1C, "ahi": 0x1D, "sfi": 0x0C, "sfhi": 0x0D, "andi": 0x14, "andhi": 0x15, "andbi": 0x16,
        "ori": 0x04, "orhi": 0x05, "orbi": 0x06, "xori": 0x44, "xorhi": 0x45, "xorbi": 0x46, "ceqi": 0x7C,
        "ceqhi": 0x7D, "ceqbi": 0x7E, "cgti": 0x4C, "cgthi": 0x4D, "cgtbi": 0x4E, "clgti": 0x5C,
        "clgthi": 0x5D, "clgtbi": 0x5E, "mpyi": 0x74, "mpyui": 0x75}
RI8 = {"cflts": 0x1D8, "cfltu": 0x1D9, "csflt": 0x1DA, "cuflt": 0x1DB}
RI16 = {"il": 0x081, "ilhu": 0x082, "ilh": 0x083, "iohl": 0x0C1, "fsmbi": 0x065}
MEM = {"lqd": ("ri10", 0x34), "stqd": ("ri10", 0x24), "lqx": ("rr", 0x1C4), "stqx": ("rr", 0x144),
       "lqa": ("ri16", 0x061), "stqa": ("ri16", 0x041), "lqr": ("ri16", 0x067), "stqr": ("ri16", 0x047)}
COND = {"brz": 0x040, "brnz": 0x042, "brhz": 0x044, "brhnz": 0x046}
FLOAT = {"fa", "fs", "fm", "fceq", "fcgt", "fcmeq", "fcmgt", "fma", "fms", "fnms", "cflts", "cfltu", "fesd"}
DOUBLE = {"dfa", "dfs", "dfm", "frds"}
PC = 0x8000  # address the r-form (lqr/stqr) instructions sit at


def enc(mn, imm=0):
    if mn in RR:
        return RR[mn] << 21 | RB << 14 | RA << 7 | RT
    if mn in RR1:
        return RR1[mn] << 21 | RA << 7 | RT
    if mn in RRR:
        return RRR[mn] << 28 | RT << 21 | RB << 14 | RA << 7 | RC
    if mn in RI7:
        return RI7[mn] << 21 | (imm & 0x7F) << 14 | RA << 7 | RT
    if mn in RI10:
        return RI10[mn] << 24 | (imm & 0x3FF) << 14 | RA << 7 | RT
    if mn in RI8:
        return RI8[mn] << 22 | (imm & 0xFF) << 14 | RA << 7 | RT
    if mn in RI16:
        return RI16[mn] << 23 | (imm & 0xFFFF) << 7 | RT
    if mn == "ila":
        return 0x21 << 25 | (imm & 0x3FFFF) << 7 | RT
    if mn in MEM:
        f, op = MEM[mn]
        if f == "ri10":
            return op << 24 | (imm & 0x3FF) << 14 | RA << 7 | RT
        if f == "rr":
            return op << 21 | RB << 14 | RA << 7 | RT
        return op << 23 | (imm & 0xFFFF) << 7 | RT
    if mn in COND:
        return COND[mn] << 23 | (4 & 0xFFFF) << 7 | RA     # brz $ra, .+16 -- condition register in rt field
    raise KeyError(mn)


# ---- big-endian model ----
def W(v): return list(struct.unpack(">4I", v))
def Ws(v): return list(struct.unpack(">4i", v))
def H(v): return list(struct.unpack(">8H", v))
def Hs(v): return list(struct.unpack(">8h", v))
def B(v): return list(v)
def Bs(v): return list(struct.unpack(">16b", v))
def F(v): return list(struct.unpack(">4f", v))
def D(v): return list(struct.unpack(">2d", v))
def pW(x): return struct.pack(">4I", *[e & M32 for e in x])
def pH(x): return struct.pack(">8H", *[e & 0xFFFF for e in x])
def pB(x): return bytes(e & 0xFF for e in x)
def pD(x): return struct.pack(">2d", *x)
def sx(v, n): v &= (1 << n) - 1; return v - (1 << n) if v >> (n - 1) else v
def ones(c, n): return (1 << n) - 1 if c else 0
def q(v): return int.from_bytes(v, "big")
def pq(x): return (x & ((1 << 128) - 1)).to_bytes(16, "big")


def f32(x):
    """Round an exact rational (or float) to the nearest single, ties to even."""
    if isinstance(x, float):
        return struct.unpack(">f", struct.pack(">f", x))[0]
    d = float(x)
    s = struct.unpack(">f", struct.pack(">f", d))[0]
    bits = struct.unpack(">I", struct.pack(">f", s))[0]
    cands = [s]
    for nb in (bits - 1, bits + 1):
        if 0 <= nb <= M32:
            c = struct.unpack(">f", struct.pack(">I", nb))[0]
            if math.isfinite(c):
                cands.append(c)
    best = min(cands, key=lambda c: (abs(Fraction(c) - x),
                                     struct.unpack(">I", struct.pack(">f", c))[0] & 1))
    return best


def pF(x): return struct.pack(">4f", *[f32(e) for e in x])


def shufb(a, b, c):
    cat = a + b
    out = []
    for s in c:
        if s & 0xE0 == 0xE0: out.append(0x80)
        elif s & 0xC0 == 0xC0: out.append(0xFF)
        elif s & 0xC0 == 0x80: out.append(0x00)
        else: out.append(cat[s & 0x1F])
    return bytes(out)


def gen_ctl(kind, t):
    m = list(range(0x10, 0x20))
    if kind == "b": m[t & 0xF] = 0x03
    if kind == "h": p = t & 0xE; m[p:p + 2] = [2, 3]
    if kind == "w": p = t & 0xC; m[p:p + 4] = [0, 1, 2, 3]
    if kind == "d": p = t & 0x8; m[p:p + 8] = list(range(8))
    return bytes(m)


def qshift_bytes(a, n):   # left, zero fill, n may exceed 16
    return pq(q(a) << (8 * n)) if n < 16 else bytes(16)


def qshr_bytes(a, n):
    return pq(q(a) >> (8 * n)) if n < 16 else bytes(16)


def qrot_bytes(a, n): n &= 15; return a[n:] + a[:n]


def cflt(v, i8, signed):
    x = Fraction(v) * Fraction(2) ** (173 - i8)
    t = math.trunc(x)
    lo, hi = (-2 ** 31, 2 ** 31 - 1) if signed else (0, M32)
    return max(lo, min(hi, t))


def model(mn, a, b, c, t, imm):
    if mn == "a": return pW([x + y for x, y in zip(W(a), W(b))])
    if mn == "sf": return pW([y - x for x, y in zip(W(a), W(b))])
    if mn == "ah": return pH([x + y for x, y in zip(H(a), H(b))])
    if mn == "sfh": return pH([y - x for x, y in zip(H(a), H(b))])
    if mn == "and": return pq(q(a) & q(b))
    if mn == "or": return pq(q(a) | q(b))
    if mn == "xor": return pq(q(a) ^ q(b))
    if mn == "nand": return pq(~(q(a) & q(b)))
    if mn == "nor": return pq(~(q(a) | q(b)))
    if mn == "andc": return pq(q(a) & ~q(b))
    if mn == "orc": return pq(q(a) | ~q(b))
    if mn == "eqv": return pq(~(q(a) ^ q(b)))
    if mn == "ceq": return pW([ones(x == y, 32) for x, y in zip(W(a), W(b))])
    if mn == "ceqh": return pH([ones(x == y, 16) for x, y in zip(H(a), H(b))])
    if mn == "ceqb": return pB([ones(x == y, 8) for x, y in zip(B(a), B(b))])
    if mn == "cgt": return pW([ones(x > y, 32) for x, y in zip(Ws(a), Ws(b))])
    if mn == "cgth": return pH([ones(x > y, 16) for x, y in zip(Hs(a), Hs(b))])
    if mn == "cgtb": return pB([ones(x > y, 8) for x, y in zip(Bs(a), Bs(b))])
    if mn == "clgt": return pW([ones(x > y, 32) for x, y in zip(W(a), W(b))])
    if mn == "clgth": return pH([ones(x > y, 16) for x, y in zip(H(a), H(b))])
    if mn == "clgtb": return pB([ones(x > y, 8) for x, y in zip(B(a), B(b))])
    # multiplies: SPU halfword 2i is the high half of word i, 2i+1 the low half
    if mn == "mpy": return pW([Hs(a)[2*i+1] * Hs(b)[2*i+1] for i in range(4)])
    if mn == "mpyu": return pW([H(a)[2*i+1] * H(b)[2*i+1] for i in range(4)])
    if mn == "mpyh": return pW([(Hs(a)[2*i] * Hs(b)[2*i+1]) << 16 for i in range(4)])
    if mn == "mpys": return pW([(Hs(a)[2*i+1] * Hs(b)[2*i+1]) >> 16 for i in range(4)])
    if mn == "mpyhh": return pW([Hs(a)[2*i] * Hs(b)[2*i] for i in range(4)])
    if mn == "mpyhhu": return pW([H(a)[2*i] * H(b)[2*i] for i in range(4)])
    if mn == "mpyhha": return pW([Ws(t)[i] + Hs(a)[2*i] * Hs(b)[2*i] for i in range(4)])
    if mn == "mpyhhau": return pW([W(t)[i] + H(a)[2*i] * H(b)[2*i] for i in range(4)])
    if mn == "mpya": return pW([Hs(a)[2*i+1] * Hs(b)[2*i+1] + Ws(c)[i] for i in range(4)])
    if mn == "mpyi": return pW([Hs(a)[2*i+1] * sx(imm, 10) for i in range(4)])
    if mn == "mpyui": return pW([H(a)[2*i+1] * (sx(imm, 10) & 0xFFFF) for i in range(4)])
    if mn == "cg": return pW([(x + y) >> 32 for x, y in zip(W(a), W(b))])
    if mn == "bg": return pW([int(y >= x) for x, y in zip(W(a), W(b))])
    if mn == "addx": return pW([x + y + (z & 1) for x, y, z in zip(W(a), W(b), W(t))])
    if mn == "sfx": return pW([y + (~x & M32) + (z & 1) for x, y, z in zip(W(a), W(b), W(t))])
    if mn == "cgx": return pW([(x + y + (z & 1)) >> 32 for x, y, z in zip(W(a), W(b), W(t))])
    if mn == "bgx": return pW([(y + (~x & M32) + (z & 1)) >> 32 for x, y, z in zip(W(a), W(b), W(t))])
    if mn == "sumb":
        return pH([s for i in range(4) for s in (sum(b[4*i:4*i+4]), sum(a[4*i:4*i+4]))])
    if mn == "absdb": return pB([abs(x - y) for x, y in zip(B(a), B(b))])
    if mn == "avgb": return pB([(x + y + 1) >> 1 for x, y in zip(B(a), B(b))])
    if mn == "clz": return pW([32 - x.bit_length() for x in W(a)])
    if mn == "cntb": return pB([bin(x).count("1") for x in B(a)])
    if mn == "xsbh": return pH([sx(a[2*i+1], 8) for i in range(8)])
    if mn == "xshw": return pW([Hs(a)[2*i+1] for i in range(4)])
    if mn == "xswd": return struct.pack(">2q", Ws(a)[1], Ws(a)[3])
    if mn == "orx": return pW([W(a)[0] | W(a)[1] | W(a)[2] | W(a)[3], 0, 0, 0])
    if mn == "gb": return pW([sum((W(a)[i] & 1) << (3 - i) for i in range(4)), 0, 0, 0])
    if mn == "gbh": return pW([sum((H(a)[i] & 1) << (7 - i) for i in range(8)), 0, 0, 0])
    if mn == "gbb": return pW([sum((a[i] & 1) << (15 - i) for i in range(16)), 0, 0, 0])
    if mn == "fsm": return pW([ones(W(a)[0] >> (3 - i) & 1, 32) for i in range(4)])
    if mn == "fsmh": return pH([ones(W(a)[0] >> (7 - i) & 1, 16) for i in range(8)])
    if mn == "fsmb": return pB([ones(W(a)[0] >> (15 - i) & 1, 8) for i in range(16)])
    if mn == "fsmbi": return pB([ones(imm >> (15 - i) & 1, 8) for i in range(16)])
    # shifts / rotates
    if mn == "shl": return pW([x << (y & 0x3F) if y & 0x3F < 32 else 0 for x, y in zip(W(a), W(b))])
    if mn == "shlh": return pH([x << (y & 0x1F) if y & 0x1F < 16 else 0 for x, y in zip(H(a), H(b))])
    if mn == "rot": return pW([(x << (y & 31)) | (x >> (32 - (y & 31))) for x, y in zip(W(a), W(b))])
    if mn == "roth": return pH([(x << (y & 15)) | (x >> (16 - (y & 15))) for x, y in zip(H(a), H(b))])
    if mn == "rotm": return pW([x >> (-y & 0x3F) if -y & 0x3F < 32 else 0 for x, y in zip(W(a), W(b))])
    if mn == "rotma": return pW([x >> min(-y & 0x3F, 31) for x, y in zip(Ws(a), W(b))])
    if mn == "rothm": return pH([x >> (-y & 0x1F) if -y & 0x1F < 16 else 0 for x, y in zip(H(a), H(b))])
    if mn == "rotmah": return pH([x >> min(-y & 0x1F, 15) for x, y in zip(Hs(a), H(b))])
    s0 = W(b)[0]
    if mn == "shlqbi": return pq(q(a) << (s0 & 7))
    if mn == "rotqbi": n = s0 & 7; return pq((q(a) << n) | (q(a) >> (128 - n)))
    if mn == "rotqmbi": return pq(q(a) >> (-s0 & 7))
    if mn == "shlqby": return qshift_bytes(a, s0 & 0x1F)
    if mn == "rotqby": return qrot_bytes(a, s0 & 0xF)
    if mn == "rotqmby": return qshr_bytes(a, -s0 & 0x1F)
    if mn == "shlqbybi": return qshift_bytes(a, (s0 >> 3) & 0x1F)
    if mn == "rotqbybi": return qrot_bytes(a, (s0 >> 3) & 0xF)
    if mn == "rotqmbybi": return qshr_bytes(a, -((s0 >> 3) & 0x1F) & 0x1F)
    if mn in ("cbx", "chx", "cwx", "cdx"): return gen_ctl(mn[1], (W(a)[0] + s0) & 0xF)
    if mn in ("cbd", "chd", "cwd", "cdd"): return gen_ctl(mn[1], (W(a)[0] + sx(imm, 7)) & 0xF)
    i7 = sx(imm, 7)
    if mn == "shli": n = i7 & 0x3F; return pW([x << n if n < 32 else 0 for x in W(a)])
    if mn == "shlhi": n = i7 & 0x1F; return pH([x << n if n < 16 else 0 for x in H(a)])
    if mn == "roti": n = i7 & 31; return pW([(x << n) | (x >> (32 - n)) for x in W(a)])
    if mn == "rothi": n = i7 & 15; return pH([(x << n) | (x >> (16 - n)) for x in H(a)])
    if mn == "rotmi": n = -i7 & 0x3F; return pW([x >> n if n < 32 else 0 for x in W(a)])
    if mn == "rotmai": n = -i7 & 0x3F; return pW([x >> min(n, 31) for x in Ws(a)])
    if mn == "rothmi": n = -i7 & 0x1F; return pH([x >> n if n < 16 else 0 for x in H(a)])
    if mn == "rotmahi": n = -i7 & 0x1F; return pH([x >> min(n, 15) for x in Hs(a)])
    if mn == "shlqbii": return pq(q(a) << (i7 & 7))
    if mn == "rotqbii": n = i7 & 7; return pq((q(a) << n) | (q(a) >> (128 - n)))
    if mn == "rotqmbii": return pq(q(a) >> (-i7 & 7))
    if mn == "shlqbyi": return qshift_bytes(a, i7 & 0x1F)
    if mn == "rotqbyi": return qrot_bytes(a, i7 & 0xF)
    if mn == "rotqmbyi": return qshr_bytes(a, -i7 & 0x1F)
    # immediates (RI10: I10 sign-extended to the lane width; byte forms use its low 8 bits)
    i10 = sx(imm, 10)
    if mn == "ai": return pW([x + i10 for x in W(a)])
    if mn == "ahi": return pH([x + i10 for x in H(a)])
    if mn == "sfi": return pW([i10 - x for x in W(a)])
    if mn == "sfhi": return pH([i10 - x for x in H(a)])
    if mn == "andi": return pW([x & i10 for x in W(a)])
    if mn == "andhi": return pH([x & i10 for x in H(a)])
    if mn == "andbi": return pB([x & imm for x in B(a)])
    if mn == "ori": return pW([x | (i10 & M32) for x in W(a)])
    if mn == "orhi": return pH([x | (i10 & 0xFFFF) for x in H(a)])
    if mn == "orbi": return pB([x | (imm & 0xFF) for x in B(a)])
    if mn == "xori": return pW([x ^ (i10 & M32) for x in W(a)])
    if mn == "xorhi": return pH([x ^ (i10 & 0xFFFF) for x in H(a)])
    if mn == "xorbi": return pB([x ^ (imm & 0xFF) for x in B(a)])
    if mn == "ceqi": return pW([ones(x == i10, 32) for x in Ws(a)])
    if mn == "ceqhi": return pH([ones(x == i10, 16) for x in Hs(a)])
    if mn == "ceqbi": return pB([ones(x == imm & 0xFF, 8) for x in B(a)])
    if mn == "cgti": return pW([ones(x > i10, 32) for x in Ws(a)])
    if mn == "cgthi": return pH([ones(x > i10, 16) for x in Hs(a)])
    if mn == "cgtbi": return pB([ones(x > sx(imm, 8), 8) for x in Bs(a)])
    if mn == "clgti": return pW([ones(x > (i10 & M32), 32) for x in W(a)])
    if mn == "clgthi": return pH([ones(x > (i10 & 0xFFFF), 16) for x in H(a)])
    if mn == "clgtbi": return pB([ones(x > (imm & 0xFF), 8) for x in B(a)])
    if mn == "il": return pW([sx(imm, 16)] * 4)
    if mn == "ilhu": return pW([imm << 16] * 4)
    if mn == "ilh": return pH([imm] * 8)
    if mn == "iohl": return pW([x | imm for x in W(t)])
    if mn == "ila": return pW([imm] * 4)
    # select / shuffle
    if mn == "selb": return pq((q(a) & ~q(c)) | (q(b) & q(c)))
    if mn == "shufb": return shufb(a, b, c)
    # single precision
    if mn == "fa": return pF([Fraction(x) + Fraction(y) for x, y in zip(F(a), F(b))])
    if mn == "fs": return pF([Fraction(x) - Fraction(y) for x, y in zip(F(a), F(b))])
    if mn == "fm": return pF([Fraction(x) * Fraction(y) for x, y in zip(F(a), F(b))])
    if mn == "fma": return pF([Fraction(x) * Fraction(y) + Fraction(z) for x, y, z in zip(F(a), F(b), F(c))])
    if mn == "fms": return pF([Fraction(x) * Fraction(y) - Fraction(z) for x, y, z in zip(F(a), F(b), F(c))])
    if mn == "fnms": return pF([Fraction(z) - Fraction(x) * Fraction(y) for x, y, z in zip(F(a), F(b), F(c))])
    if mn == "fceq": return pW([ones(x == y, 32) for x, y in zip(F(a), F(b))])
    if mn == "fcgt": return pW([ones(x > y, 32) for x, y in zip(F(a), F(b))])
    if mn == "fcmeq": return pW([ones(abs(x) == abs(y), 32) for x, y in zip(F(a), F(b))])
    if mn == "fcmgt": return pW([ones(abs(x) > abs(y), 32) for x, y in zip(F(a), F(b))])
    if mn == "cflts": return pW([cflt(x, imm, True) for x in F(a)])
    if mn == "cfltu": return pW([cflt(x, imm, False) for x in F(a)])
    if mn == "csflt": return pF([Fraction(x) * Fraction(2) ** (imm - 155) for x in Ws(a)])
    if mn == "cuflt": return pF([Fraction(x) * Fraction(2) ** (imm - 155) for x in W(a)])
    # double precision: doubleword i = SPU words 2i (high), 2i+1 (low)
    if mn == "dfa": return pD([x + y for x, y in zip(D(a), D(b))])
    if mn == "dfs": return pD([x - y for x, y in zip(D(a), D(b))])
    if mn == "dfm": return pD([x * y for x, y in zip(D(a), D(b))])
    if mn == "fesd": return pD([float(F(a)[0]), float(F(a)[2])])
    if mn == "frds": return struct.pack(">f", f32(D(a)[0])) + bytes(4) + struct.pack(">f", f32(D(a)[1])) + bytes(4)
    raise KeyError(mn)


def ls_init(i):
    return (i * 131 + (i >> 8) * 7 + 0x5A) & 0xFF


def mem_model(mn, a, b, t, imm):
    """Returns (rt_after, probe_address)."""
    if mn in ("lqd", "stqd"): ea = (W(a)[0] + sx(imm, 10) * 16) & 0x3FFF0
    elif mn in ("lqx", "stqx"): ea = (W(a)[0] + W(b)[0]) & 0x3FFF0
    elif mn in ("lqa", "stqa"): ea = (imm << 2) & 0x3FFF0
    else: ea = (PC + sx(imm, 16) * 4) & 0x3FFF0
    if mn.startswith("lq"):
        return bytes(ls_init(ea + k) for k in range(16)), ea, None
    return t, ea, t


def cond_model(mn, a):
    if mn == "brz": return int(W(a)[0] == 0)
    if mn == "brnz": return int(W(a)[0] != 0)
    if mn == "brhz": return int(H(a)[1] == 0)      # preferred halfword = SPU bytes 2-3
    if mn == "brhnz": return int(H(a)[1] != 0)


def rand_vec(rng, mn):
    if mn in FLOAT or mn in ("fesd",):
        return struct.pack(">4f", *[rng.choice([1, -1]) * rng.uniform(0.5, 2.0) * 2.0 ** rng.randrange(-20, 20)
                                    for _ in range(4)])
    if mn in DOUBLE:
        return struct.pack(">2d", *[rng.choice([1, -1]) * rng.uniform(0.5, 2.0) * 2.0 ** rng.randrange(-30, 30)
                                    for _ in range(2)])
    r = rng.random()
    if r < 0.08: return bytes(16)
    if r < 0.16: return b"\xff" * 16
    if r < 0.22: return b"\x80" * 16
    if r < 0.28: return bytes(range(16))
    if r < 0.40:  # small values in every word (shift counts, indices, equal lanes)
        return struct.pack(">4I", *[rng.randrange(0, 70) for _ in range(4)])
    return bytes(rng.randrange(256) for _ in range(16))


def rand_imm(rng, mn):
    if mn in RI7: return rng.randrange(128)
    if mn in RI10: return rng.choice([0, 1, 0x3FF, 0x200, 0x1FF, 0x7F, 0x80]) if rng.random() < .3 else rng.randrange(1024)
    if mn in RI8: return rng.choice([173, 155, 160, 150, 180]) if mn in ("cflts", "cfltu") else rng.choice([155, 140, 160, 130])
    if mn == "ila": return rng.randrange(1 << 18)
    if mn in ("lqd", "stqd"): return rng.randrange(1024)
    if mn in ("lqa", "stqa", "lqr", "stqr"): return rng.randrange(1 << 16)
    if mn in RI16: return rng.randrange(1 << 16)
    return 0


def main():
    ops = list(RR) + list(RR1) + list(RRR) + list(RI7) + list(RI10) + list(RI8) + list(RI16) + ["ila"]
    mems, conds = list(MEM), list(COND)
    rng = random.Random(20261007)
    lifter = spu_lifter.SPULifter()
    func = spu_lifter.LiftedFunction(name="t", start_addr=0, end_addr=0x40000)
    src = ['#include "spu_helpers.h"',
           "int g_spu_ls_watch_n = 0;",
           # runtime globals the helpers read (patch 1071 extended-range lanes on, as in the port)
           "int g_kz3_spu_xr = 1; int g_kz3_spu_slowcg = 0; int g_kz3_fma_stats = 0;",
           "void kz3_spu_fma_note(const double* s, int ftz){(void)s;(void)ftz;}",
           # patch 1088 probe off and patch 1089 inline lanes on (the runtime defaults); stubs keep the link self-contained
           "int g_kz3_xr_stats = 0; int g_kz3_spu_xrq = 1;",
           "void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c,"
           " const uint32_t* r, const void* ra){(void)op;(void)m;(void)a;(void)b;(void)c;(void)r;(void)ra;}",
           "void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr)"
           "{(void)a;(void)w;(void)p;(void)pc;(void)lr;}",
           "static spu_context C;",
           'static void rd(int r){ for(int i=0;i<4;i++){ unsigned x=0; if(scanf("%8x",&x)!=1) return; C.gpr[r]._u32[i]=x; } }',
           "static unsigned lsv(unsigned i){ return (i*131+(i>>8)*7+0x5A)&0xFF; }",
           "int main(void){ spu_context* ctx=&C; int id; unsigned probe; C.ls=C.ls_store;",
           " for(unsigned i=0;i<SPU_LS_SIZE;i++) C.ls[i]=(uint8_t)lsv(i);",
           ' while(scanf("%d %x",&id,&probe)==2){ int cond=-1; probe&=0x3FFF0;',
           f"  rd({RA}); rd({RB}); rd({RC}); rd({RT});",
           "  switch(id){"]
    bad = {}
    plan = []   # (mn, imm list)
    ids = {}
    for mn in ops + mems + conds:
        imms = [rand_imm(rng, mn) for _ in range(CASES)]
        plan.append((mn, imms))
        # The immediate is part of the instruction, so each (op, imm) gets its own statement.
        for imm in sorted(set(imms)):
            word = enc(mn, imm)
            ins = spu_decode(word, PC if mn in ("lqr", "stqr") else 0x4000)
            if ins.mnemonic != mn:
                bad[mn] = f"0x{word:08x} decoded as {ins.mnemonic} {ins.operands}"
                break
            if mn in conds:
                stmt = f"cond = ({lifter._cond(mn, str(RA))}) ? 1 : 0;"
            else:
                stmt = lifter._translate(ins, func)
                if "unsupported" in stmt or "TODO" in stmt:
                    bad[mn] = f"lifter emits: {stmt[:90]}"
                    break
            ids[(mn, imm)] = len(ids)
            src.append(f"  case {ids[(mn, imm)]}: {{ {stmt} }} break;")
    src += ["  }",
            f'  for(int i=0;i<4;i++) printf("%08x", C.gpr[{RT}]._u32[i]);',
            '  printf(" "); for(int i=0;i<16;i++) printf("%02x", C.ls[probe+i]); printf(" %d\\n", cond);',
            "  for(int i=0;i<16;i++) C.ls[probe+i]=(uint8_t)lsv(probe+i);",
            " } return 0; }"]
    inp, expect = [], []
    for mn, imms in plan:
        if mn in bad:
            continue
        for imm in imms:
            a, b, c, t = (rand_vec(rng, mn) for _ in range(4))
            if mn in ("cflts", "cfltu") and rng.random() < 0.3:   # also exercise saturation
                a = struct.pack(">4f", *[rng.choice([1, -1]) * 2.0 ** rng.randrange(20, 40) for _ in range(4)])
            probe, ls_exp, cond = 0, None, -1
            if mn in mems:
                rt, probe, ls_exp = mem_model(mn, a, b, t, imm)
            elif mn in conds:
                rt, cond = t, cond_model(mn, a)
            else:
                rt = model(mn, a, b, c, t, imm)
            if ls_exp is None:
                ls_exp = bytes(ls_init((probe & 0x3FFF0) + k) for k in range(16))
            inp.append(f"{ids[(mn, imm)]} {probe:x} " + " ".join(v.hex() for v in (a, b, c, t)))
            expect.append((mn, (a, b, c, t, imm), rt.hex(), ls_exp.hex(), cond))
    with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "work", "spu")) as td:
        cfile, exe = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cfile, "w").write("\n".join(src))
        env = {k: v for k, v in os.environ.items() if not k.startswith("NIX_LDFLAGS")}
        inc = [f"-I{os.path.join(PS3, 'runtime', 'spu')}"]
        subprocess.run(["cc", "-std=gnu11", "-O1", "-w", *inc, "-o", exe, cfile, "-lm"], check=True, env=env)
        out = subprocess.run([exe], input="\n".join(inp) + "\n", capture_output=True, text=True,
                             check=True).stdout.splitlines()
    assert len(out) == len(expect), (len(out), len(expect))
    counts = {}
    for line, (mn, vin, rt, ls, cond) in zip(out, expect):
        got_rt, got_ls, got_c = line.split()
        counts[mn] = counts.get(mn, 0) + 1
        ok = got_rt == rt and got_ls == ls and int(got_c) == cond
        if not ok and mn not in bad:
            a, b, c, t, imm = vin
            bad[mn] = (f"imm={imm:#x} a={a.hex()} b={b.hex()} c={c.hex()} t={t.hex()} "
                       f"got rt={got_rt} ls={got_ls} cond={got_c} want rt={rt} ls={ls} cond={cond}")
    names = ops + mems + conds
    for mn in names:
        print(f"{'FAIL' if mn in bad else 'ok  '} {mn:10} {counts.get(mn, 0)} cases"
              + (f"  {bad[mn]}" if mn in bad else ""))
    print(f"{len(names) - len(bad)}/{len(names)} ops match the big-endian model "
          f"({sum(counts.values())} cases)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
