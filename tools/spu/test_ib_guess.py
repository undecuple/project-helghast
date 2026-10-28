#!/usr/bin/env python3
"""Patch 1098 host test: the lifter's computed-hop guesses (compute_ib_guesses in tools/spu_lifter.py).

    python3 tools/spu/test_ib_guess.py            (KZ3_PATCHED_DIR = patched ps3recomp, default work/ps3recomp-patched)

Self-contained: hand-encoded SPU words, decoded by the patched tree's own disassembler. Checks the loop idiom of the
audio mixer jobs (ila head; selb rX, head, r0, cond; bi rX), a copy (ori rY, rX, 0), two candidates through selb,
no guess for a loaded target (lqd), for a constant that starts no function, and for the runtime's special addresses,
and the emitted SPU_IB_HOP_G line. SKIP when the patched tree has no compute_ib_guesses (1098 not applied)."""
import os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PT = os.environ.get("KZ3_PATCHED_DIR", os.path.join(ROOT, "work", "ps3recomp-patched"))
sys.path.insert(0, os.path.join(PT, "tools"))
try:
    import spu_lifter
    from spu_disasm import disassemble_spu
except ImportError as e:
    print(f"SKIP: no lifter in {PT} ({e})")
    sys.exit(0)
if not hasattr(spu_lifter, "compute_ib_guesses"):
    print(f"SKIP: {PT} has no compute_ib_guesses (patch 1098 not applied)")
    sys.exit(0)


def ila(rt, imm): return (0x21 << 25) | ((imm & 0x3FFFF) << 7) | rt
def selb(rt, ra, rb, rc): return (0x8 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc
def bi(ra): return (0x1A8 << 21) | (ra << 7)
def ori0(rt, ra): return (0x04 << 24) | (ra << 7) | rt
def lqd(rt, ra): return (0x34 << 24) | (ra << 7) | rt
NOP = 0x40200000

prog = {
    0x100: ila(38, 0x200),
    0x104: ila(39, 0x300),
    0x108: ila(9, 0xA00),
    0x10C: ila(10, 0x1234),
    0x200: selb(42, 38, 0, 35),   # loop closing: rX = selb(head, exit = r0, cond)
    0x204: bi(42),
    0x208: ori0(7, 38),           # copy of the head
    0x20C: bi(7),
    0x210: selb(43, 38, 39, 35),  # two candidates
    0x214: bi(43),
    0x218: lqd(5, 1),             # loaded target: no guess
    0x21C: bi(5),
    0x220: bi(9),                 # 0xA00: a runtime special address, never guessed
    0x224: bi(10),                # 0x1234 starts no function
}
base = 0x100
words = [prog.get(a, NOP) for a in range(base, 0x228, 4)]
data = b"".join(w.to_bytes(4, "big") for w in words)
insns = disassemble_spu(data, base)
starts = {0x100, 0x200, 0x300, 0xA00}
sites = {i.addr for i in insns if spu_lifter._bi_target_reg(i) is not None}
g = spu_lifter.compute_ib_guesses(insns, starts, sites)

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", msg)

check(g.get(0x204) == [0x200], f"selb loop idiom: {g.get(0x204)}")
check(g.get(0x20C) == [0x200], f"copy: {g.get(0x20C)}")
check(sorted(g.get(0x214, [])) == [0x200, 0x300], f"two candidates: {g.get(0x214)}")
check(0x21C not in g, f"loaded target guessed: {g.get(0x21C)}")
check(0x220 not in g, f"special address guessed: {g.get(0x220)}")
check(0x224 not in g, f"non-function constant guessed: {g.get(0x224)}")

lf = spu_lifter.SPULifter(prefix="t_")
lf.ib_guesses = g
line = lf._ib_hop(0x204, "ctx->gpr[42]._u32[0]")
check(line == "SPU_IB_HOP_G(ctx, ctx->gpr[42]._u32[0], SPU_IB_GUESS(ctx, 0x200u, t_spu_func_00000200));",
      f"emitted: {line}")
check(lf._ib_hop(0x21C, "x") == "SPU_IB_HOP(ctx, x);", "no-guess site not the plain SPU_IB_HOP")
check(lf.ib_guess_targets == {0x200}, f"guess targets {lf.ib_guess_targets}")

print("FAIL (%d)" % fails if fails else f"PASS ({len(g)} guess sites)")
sys.exit(1 if fails else 0)
