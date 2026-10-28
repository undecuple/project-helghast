#!/usr/bin/env python3
"""Patch 1116 host test, part 1 (SPU pass 101): lift a small synthetic guest program with the toolkit's own
ppu_lifter.py and write the C for tools/spu/test_r101_setjmp.cpp. No game data.

The guest program is Lua's protected-call shape (luaD_rawrunprotected / luaD_throw, KZ3 0xA91FC / 0x9EE30):
  SJ     setjmp(buf): buf = (r3 + 15) & ~15; saves r1, LR, r2, r30, r31; returns 0
  LJ     longjmp(buf, v): restores them; r3 = v ? v : 1; blr (returns to the saved LR on the saved r1)
  PCALL_k  frame 0x100; saves the global "current jmp_buf" (0x7000), sets it to its own (r1 + 0x74, unaligned like
         Lua's lj.b), bl SJ; on 0: bl BODY_k, r3 = 0; on a landing: r3 = 100 + value; restores 0x7000; returns
  THROW(v) r3 = *0x7000; bl LJ; if LJ returns (the bug): *0x7008 += 1, returns
Bodies: OK (returns), THROW7, NESTED (PCALL_THROW7, result to 0x7010, then THROW9), DEEP (D1 -> D2 -> host
function 0x10F00, which bumps the dispatch depth like ps3_indirect_call, -> THROW5), PCALL_OK_DEEP (an extra
frame around PCALL_OK, leaving a record below the next call's r1).

Usage: test_r101_setjmp_gen.py TOOLKIT OUT_DIR  ->  OUT_DIR/lifted.inc, OUT_DIR/syms.h
Without the patch (no --setjmp support in the lifter) it lifts the plain program and says so in syms.h
(SJ_PATCHED 0): the test must then fail."""
import os
import sys

T, OUT = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(T, "tools"))
import ppu_disasm                      # noqa: E402
from ppu_lifter import PPULifter       # noqa: E402

BASE = 0x10000


class Asm:
    def __init__(self, addr):
        self.addr, self.w, self.labels, self.fix = addr, [], {}, []

    def here(self):
        return self.addr + 4 * len(self.w)

    def L(self, name):
        self.labels[name] = self.here()

    def emit(self, word):
        self.w.append(word & 0xFFFFFFFF)

    def addi(self, rt, ra, si): self.emit((14 << 26) | (rt << 21) | (ra << 16) | (si & 0xFFFF))
    def li(self, rt, si): self.addi(rt, 0, si)
    def and_(self, ra, rs, rb): self.emit((31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (28 << 1))
    def mr(self, ra, rs): self.emit((31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1))
    def std(self, rs, d, ra): self.emit((62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC))
    def stdu(self, rs, d, ra): self.emit((62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC) | 1)
    def ld(self, rt, d, ra): self.emit((58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC))
    def mflr(self, rt): self.emit((31 << 26) | (rt << 21) | (8 << 16) | (339 << 1))
    def mtlr(self, rs): self.emit((31 << 26) | (rs << 21) | (8 << 16) | (467 << 1))
    def blr(self): self.emit(0x4E800020)
    def cmpwi(self, ra, si): self.emit((11 << 26) | (ra << 16) | (si & 0xFFFF))

    def bl(self, tgt): self.emit((18 << 26) | ((tgt - self.here()) & 0x3FFFFFC) | 1)

    def b(self, lab): self.fix.append((len(self.w), lab, "b")); self.emit(18 << 26)
    def bne(self, lab): self.fix.append((len(self.w), lab, "bne")); self.emit((16 << 26) | (4 << 21) | (2 << 16))
    def beq(self, lab): self.fix.append((len(self.w), lab, "beq")); self.emit((16 << 26) | (12 << 21) | (2 << 16))

    def done(self):
        for i, lab, kind in self.fix:
            rel = self.labels[lab] - (self.addr + 4 * i)
            self.w[i] |= (rel & 0x3FFFFFC) if kind == "b" else (rel & 0xFFFC)
        return self.w


A = {"SJ": BASE, "LJ": BASE + 0x100, "THROW": BASE + 0x200,
     "PCALL_OK": BASE + 0x300, "PCALL_THROW7": BASE + 0x400, "PCALL_NESTED": BASE + 0x500,
     "PCALL_DEEP": BASE + 0x600, "PCALL_OK_DEEP": BASE + 0x700,
     "BODY_OK": BASE + 0x800, "BODY_THROW7": BASE + 0x900, "BODY_NESTED": BASE + 0xA00, "BODY_DEEP": BASE + 0xB00,
     "D1": BASE + 0xC00, "D2": BASE + 0xD00, "BODY_THROW5": BASE + 0xE00, "HOSTFN": BASE + 0xF00}
GJB, GBUG, GNEST = 0x7000, 0x7008, 0x7010
funcs = {}


def align16(a):
    a.addi(3, 3, 15); a.li(0, -16); a.and_(3, 3, 0)


a = Asm(A["SJ"]); align16(a)
a.std(1, 0, 3); a.mflr(0); a.std(0, 8, 3); a.std(31, 16, 3); a.std(30, 24, 3); a.std(2, 32, 3)
a.li(3, 0); a.blr(); funcs["SJ"] = a.done()

a = Asm(A["LJ"]); align16(a)
a.ld(1, 0, 3); a.ld(0, 8, 3); a.mtlr(0); a.ld(31, 16, 3); a.ld(30, 24, 3); a.ld(2, 32, 3)
a.cmpwi(4, 0); a.bne("nz"); a.li(4, 1); a.L("nz"); a.mr(3, 4); a.blr(); funcs["LJ"] = a.done()

a = Asm(A["THROW"])   # r3 = value
a.mflr(0); a.std(0, 16, 1); a.stdu(1, -0x70, 1)
a.mr(4, 3); a.li(9, GJB); a.ld(3, 0, 9); a.bl(A["LJ"])
a.li(9, GBUG); a.ld(8, 0, 9); a.addi(8, 8, 1); a.std(8, 0, 9)          # only reached when longjmp "returned"
a.addi(1, 1, 0x70); a.ld(0, 16, 1); a.mtlr(0); a.blr(); funcs["THROW"] = a.done()


def pcall(name, body):
    a = Asm(A[name])
    a.mflr(0); a.std(0, 16, 1); a.stdu(1, -0x100, 1); a.std(31, 0xF8, 1)
    a.li(31, 0x31)                                                       # a callee-saved reg the landing must see
    a.li(9, GJB); a.ld(8, 0, 9); a.std(8, 0xE0, 1); a.addi(8, 1, 0x74); a.std(8, 0, 9)
    a.addi(3, 1, 0x74); a.bl(A["SJ"])
    a.cmpwi(3, 0); a.bne("land")
    a.bl(A[body]); a.li(3, 0); a.b("out")
    a.L("land"); a.cmpwi(31, 0x31); a.bne("bad"); a.addi(3, 3, 100); a.b("out")
    a.L("bad"); a.li(3, -1)
    a.L("out"); a.li(9, GJB); a.ld(8, 0xE0, 1); a.std(8, 0, 9)
    a.ld(31, 0xF8, 1); a.addi(1, 1, 0x100); a.ld(0, 16, 1); a.mtlr(0); a.blr()
    funcs[name] = a.done()


pcall("PCALL_OK", "BODY_OK"); pcall("PCALL_THROW7", "BODY_THROW7")
pcall("PCALL_NESTED", "BODY_NESTED"); pcall("PCALL_DEEP", "BODY_DEEP")


def framed(name, calls, tail=None):
    a = Asm(A[name])
    a.mflr(0); a.std(0, 16, 1); a.stdu(1, -0x80, 1); a.std(31, 0x78, 1)
    a.li(31, 0x77)                                                       # clobber r31: a landing restores it
    for c in calls:
        if isinstance(c, int):
            a.li(3, c)
        elif c[0] == "store":
            a.li(9, c[1]); a.std(3, 0, 9)
        else:
            a.bl(A[c])
    a.ld(31, 0x78, 1); a.addi(1, 1, 0x80); a.ld(0, 16, 1); a.mtlr(0); a.blr()
    funcs[name] = a.done()


framed("PCALL_OK_DEEP", ["PCALL_OK"])
a = Asm(A["BODY_OK"]); a.blr(); funcs["BODY_OK"] = a.done()
framed("BODY_THROW7", [7, "THROW"])
framed("BODY_THROW5", [5, "THROW"])
framed("BODY_NESTED", ["PCALL_THROW7", ("store", GNEST), 9, "THROW"])
framed("BODY_DEEP", ["D1"])
framed("D1", ["D2"])
framed("D2", ["HOSTFN"])           # HOSTFN (the harness) bumps the dispatch depth and calls BODY_THROW5

lifter = PPULifter(prefix="")
patched = hasattr(lifter, "setjmp_marks")
if patched:
    lifter.setjmp_marks[A["SJ"]] = -1
    lifter.longjmp_marks.add(A["LJ"])
out = []
for name, words in funcs.items():
    insns = [ppu_disasm.decode(w, A[name] + 4 * i) for i, w in enumerate(words)]
    f = lifter.lift_function(insns, A[name], A[name] + 4 * len(words))
    by = {f.start_addr: f}
    out += lifter._function_def_lines(f, by, [f.start_addr], {f.start_addr: 0})
with open(os.path.join(OUT, "lifted.inc"), "w") as fh:
    fh.write("\n".join(out) + "\n")
with open(os.path.join(OUT, "syms.h"), "w") as fh:
    fh.write(f"#define SJ_PATCHED {1 if patched else 0}\n")
    for k, v in A.items():
        fh.write(f"#define A_{k} 0x{v:08X}u\nextern \"C\" void func_{v:08X}(ppu_context*);\n#define F_{k} func_{v:08X}\n")
    fh.write(f"#define G_JB 0x{GJB:X}u\n#define G_BUG 0x{GBUG:X}u\n#define G_NEST 0x{GNEST:X}u\n")
print(f"lifted {len(funcs)} guest functions ({'patched' if patched else 'UNPATCHED'} lifter)")
