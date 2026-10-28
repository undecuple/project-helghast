#!/usr/bin/env python3
"""Check the VMX handlers of port/patches/0001-vmx-big-endian-lanes.patch against a reference model.

    test_vmx_patch.py [PATCHED_PS3RECOMP_DIR]   (default work/ps3recomp-patched)

For each op: ask the patched lifter for its C statement, compile it into a host test program (cc), run
random vectors through it and compare with a Python model of the PowerPC (big-endian lane) semantics.
Exit status 0 = all cases match.
"""
import os, random, struct, subprocess, sys, tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
PS3 = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "work", "ps3recomp-patched")
sys.path.insert(0, os.path.join(PS3, "tools"))
import ppu_lifter                      # noqa: E402
from ppu_disasm import Instruction     # noqa: E402

FOUR = {"vmsumshm", "vmsumuhm"}


def H(v): return list(struct.unpack(">8H", v))
def Hs(v): return list(struct.unpack(">8h", v))
def W(v): return list(struct.unpack(">4I", v))
def Ws(v): return list(struct.unpack(">4i", v))
def pH(x): return struct.pack(">8H", *[e & 0xFFFF for e in x])
def pW(x): return struct.pack(">4I", *[e & 0xFFFFFFFF for e in x])
def sat(v, lo, hi): return max(lo, min(hi, v))


def model(mn, a, b, c):
    if mn == "vmulouh":
        return pW([H(a)[2 * i + 1] * H(b)[2 * i + 1] for i in range(4)])
    if mn == "vmsumshm":
        return pW([Ws(c)[i] + Hs(a)[2 * i] * Hs(b)[2 * i] + Hs(a)[2 * i + 1] * Hs(b)[2 * i + 1] for i in range(4)])
    if mn == "vmsumuhm":
        return pW([W(c)[i] + H(a)[2 * i] * H(b)[2 * i] + H(a)[2 * i + 1] * H(b)[2 * i + 1] for i in range(4)])
    if mn == "vadduws":
        return pW([sat(x + y, 0, 0xFFFFFFFF) for x, y in zip(W(a), W(b))])
    if mn == "vsubuws":
        return pW([sat(x - y, 0, 0xFFFFFFFF) for x, y in zip(W(a), W(b))])
    if mn == "vadduhs":
        return pH([sat(x + y, 0, 0xFFFF) for x, y in zip(H(a), H(b))])
    if mn == "vpkuwum":
        return pH([x & 0xFFFF for x in W(a) + W(b)])
    if mn == "vpkswss":
        return pH([sat(x, -32768, 32767) for x in Ws(a) + Ws(b)])
    if mn == "vsrb":
        return bytes(x >> (y & 7) for x, y in zip(a, b))
    if mn == "vsrab":
        return bytes(((x - 256 if x > 127 else x) >> (y & 7)) & 0xFF for x, y in zip(a, b))
    raise KeyError(mn)


def main():
    lifter = ppu_lifter.PPULifter()
    ops = ["vmulouh", "vmsumshm", "vmsumuhm", "vadduws", "vsubuws", "vadduhs", "vpkuwum", "vpkswss", "vsrb", "vsrab"]
    cases = 300
    rng = random.Random(1234)
    edge = [b"\x00" * 16, b"\xff" * 16, b"\x80" * 16, b"\x7f" * 16, bytes(range(16))]
    vecs = {mn: [] for mn in ops}
    for mn in ops:
        for k in range(cases):
            pick = lambda: edge[rng.randrange(len(edge))] if rng.random() < 0.2 else bytes(rng.randrange(256) for _ in range(16))
            vecs[mn].append((pick(), pick(), pick()))

    src = ["#include <stdint.h>", "#include <stdio.h>", "#include <string.h>",
           "typedef struct { uint8_t b[16]; } v128; typedef struct { v128 vr[32]; } kz3ctx;",
           "static int rd(uint8_t* p){ for(int i=0;i<16;i++){ unsigned x; if(scanf(\"%2x\",&x)!=1) return 0; p[i]=(uint8_t)x; } return 1; }",
           "int main(void){ kz3ctx c; kz3ctx* ctx=&c; char op[16];",
           " while(scanf(\"%15s\",op)==1){ memset(&c,0,sizeof c); rd(c.vr[1].b); rd(c.vr[2].b); rd(c.vr[3].b);"]
    for mn in ops:
        operands = "v4, v1, v2, v3" if mn in FOUR else "v4, v1, v2"
        stmt = lifter._translate_op(Instruction(0x10000, 0, mn, operands), None)
        if "TODO" in stmt:
            print(f"FAIL {mn}: lifter still emits a TODO: {stmt[:80]}")
            return 1
        src.append(f'  if(!strcmp(op,"{mn}")) {stmt}')
    src.append('  for(int i=0;i<16;i++) printf("%02x", c.vr[4].b[i]); printf("\\n"); } return 0; }')

    with tempfile.TemporaryDirectory() as td:
        cfile, exe = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cfile, "w").write("\n".join(src))
        # The repo path contains a space; mkShell's NIX_LDFLAGS (-L$PWD/outputs/out/lib) then splits into a bogus
        # -L argument. This standalone test needs no extra link paths.
        env = {k: v for k, v in os.environ.items() if not k.startswith("NIX_LDFLAGS")}
        subprocess.run(["cc", "-O1", "-o", exe, cfile], check=True, env=env)
        stdin = "".join(f"{mn} {a.hex()} {b.hex()} {c.hex()}\n" for mn in ops for a, b, c in vecs[mn])
        out = subprocess.run([exe], input=stdin, capture_output=True, text=True, check=True).stdout.split()
    i, bad = 0, {}
    for mn in ops:
        for a, b, c in vecs[mn]:
            if bytes.fromhex(out[i]) != model(mn, a, b, c):
                bad.setdefault(mn, (a.hex(), b.hex(), c.hex(), out[i], model(mn, a, b, c).hex()))
            i += 1
    for mn in ops:
        print(f"{'FAIL' if mn in bad else 'ok  '} {mn:9} {cases} cases" + (f"  first mismatch: {bad[mn]}" if mn in bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
