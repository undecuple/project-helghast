#!/usr/bin/env python3
"""Focused test for the staged PR docs/upstream/submission/3009-lifter-callee-save-carry-forms.diff (ppu_lifter
callee-save pass: a register's first redefinition counts any assignment in the line; found by the Project Pyrrhus
Killzone 2 port). OLD_TREE = upstream master (default work/upstream-snapshot/master), the diff is applied to a copy.

Lifts a synthetic function through PPULifter.lift_function:
    addic r22,r27,1 ; std r22,0xA8(r1) ; mr r3,r4 ; ld r22,0xA8(r1) ; blr
(addic is emitted as `{ ...; ctx->gpr[22] = _r; ... }`). The spill follows the only definition of r22, so the reload must
stay a real load. Upstream rewrites it to `ctx->gpr[22] = _cs_22;` (the value at entry): the bug. Control: the same
function with `addi r22,r27,1` (a line that starts with the assignment) is not rewritten by either tree.
NEW_TREE=dir picks the tree under test instead of the copy; SKIP when the upstream tree is absent."""
import importlib.util, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _staged, _extract as X
_old, _new = _staged.trees("3009-lifter-callee-save-carry-forms.diff")
TREES = {"upstream": _old, "tree under test": _new}

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

def control_tree():
    """The tree under test with the definition regex cut back to a line that STARTS with the assignment (the bug)."""
    import shutil, tempfile
    d = tempfile.mkdtemp(prefix="ctl-"); shutil.copytree(os.path.join(_new, "tools"), os.path.join(d, "tools"))
    p = os.path.join(d, "tools", "ppu_lifter.py"); src = open(p).read()
    src2 = src.replace("_CS_ANY_DEF_RE = re.compile(r'ctx->gpr", "_CS_ANY_DEF_RE = re.compile(r'^\\s*ctx->gpr")
    if src2 == src: sys.exit("FAIL: negative control did not apply")
    open(p, "w").write(src2)
    return d

up, new = rewritten(load(_old), ADDIC), rewritten(load(_new), ADDIC)
ctl = rewritten(load(control_tree()), ADDIC)
ctl_addi = rewritten(load(_new), ADDI)
print(f"addic reload rewritten to the entry value: upstream={up} (bug), with the diff={new}, control (regex cut back)={ctl}; addi unchanged={not ctl_addi}")
X.verdict("3009 callee-save pass sees carry-form definitions", up, (not new) and not ctl_addi, ctl)
