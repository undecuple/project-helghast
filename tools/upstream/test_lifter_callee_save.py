#!/usr/bin/env python3
"""Host test for KZ3 patch 3009 (ppu_lifter callee-save pass: a register's first redefinition counts any assignment in
the line, found by Project Pyrrhus as their 6018). No game data.

Lifts a synthetic function through PPULifter.lift_function:
    addic r22,r27,1 ; std r22,0xA8(r1) ; mr r3,r4 ; ld r22,0xA8(r1) ; blr
(addic is emitted as `{ ...; ctx->gpr[22] = _r; ... }`). The spill follows the only definition of r22, so the reload must
stay a real load. Upstream rewrites it to `ctx->gpr[22] = _cs_22;` (the value at entry): the bug. Control: the same
function with `addi r22,r27,1` (a line that starts with the assignment) is not rewritten by either tree.
NEW_TREE=dir picks the tree under test (default work/ps3recomp-patched; SKIP when absent)."""
import importlib.util, os, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
TREES = {"upstream": os.path.join(ROOT, "third_party/ps3recomp"),
         "tree under test": os.environ.get("NEW_TREE", os.path.join(ROOT, "work/ps3recomp-patched"))}

def load(tree):
    d = os.path.join(tree, "tools")
    if not os.path.exists(os.path.join(d, "ppu_lifter.py")):
        return None
    sys.path.insert(0, d)
    for m in ("ppu_disasm", "ppu_lifter"): sys.modules.pop(m, None)
    spec = importlib.util.spec_from_file_location("ppu_lifter", os.path.join(d, "ppu_lifter.py"))
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    sys.path.pop(0)
    return mod

def words(first):
    return [first,
            (62 << 26) | (22 << 21) | (1 << 16) | 0xA8,                        # std r22,0xA8(r1)
            (31 << 26) | (4 << 21) | (3 << 16) | (4 << 11) | (444 << 1),       # mr r3,r4
            (58 << 26) | (22 << 21) | (1 << 16) | 0xA8,                        # ld r22,0xA8(r1)
            0x4E800020]                                                         # blr

def rewritten(mod, first):
    data = b"".join(struct.pack(">I", w) for w in words(first))
    insns = mod.disassemble_bytes(data, 0x10000)
    f = mod.PPULifter().lift_function(insns, 0x10000, 0x10000 + len(data))
    return any("ctx->gpr[22] = _cs_22;" in l for l in f.body_lines)

ADDIC = (12 << 26) | (22 << 21) | (27 << 16) | 1
ADDI = (14 << 26) | (22 << 21) | (27 << 16) | 1
fail = 0
for name, tree in TREES.items():
    mod = load(tree)
    if mod is None:
        print(f"SKIP {name}: no tree at {tree}"); continue
    a, b = rewritten(mod, ADDIC), rewritten(mod, ADDI)
    want = (name == "upstream")          # upstream shows the bug, the patched tree must not
    ok = (a == want) and not b
    print(f"{'PASS' if ok else 'FAIL'} {name}: addic reload rewritten={a} (expect {want}), addi reload rewritten={b} (expect False)")
    if name == "upstream" and not ok: print("  (upstream behaviour changed: the bug may be fixed upstream)")
    fail += (not ok) and name != "upstream"
sys.exit(1 if fail else 0)
