#!/usr/bin/env python3
"""Run KZ3's WWS interrupt save/restore stub through ps3recomp's SMC micro-interpreter (patch 1002 check).

    nix develop .#re --command tools/sandbox.sh -- python3 tools/spu/test_smc_microstep.py [--ps3recomp DIR]

The WWS job-manager policy module (pm_012af080, LS 0xA00) does not save registers in lifted code on an
interrupt: its handler (LS 0xA34-0xACC) GENERATES a stub at LS 0x3FEC0 and calls it twice:
    save:    lqa $r77,0xA40 ; ila $r78,0x130 ; ila $r79,0x804
             loop { sf $r77,$r79,$r77 ; ai $r78,$r78,-16 ; stqx $r77,$r76(=0x3FEC0),$r78 } until r78 == 0
             0x3FFF0 = 0x35002780 (`bi $r79`) ; brasl $r79,0x3FEC0
    restore: the same from the quadword at 0xAD0.
So the stub is 76 `stqa`/`lqa` (r0..r75 to/from 0x3F9C0..0x3FE7F) and a `bi $r79`. No lift exists for it;
the runtime runs it with spu_smc_microstep() (runtime/spu/spu_channels.c). This test builds the stub from the
real PM bytes (EBOOT), extracts spu_smc_microstep() verbatim from the given ps3recomp tree, compiles it against
the tree's spu_context.h / spu_helpers.h, and checks:
  1. save: every register r0..r75 lands at 0x3F9C0 + 16*n, nothing else in LS changes, pc = r79;
  2. restore (registers clobbered first): r0..r75 come back, pc = r79.
Pinned ps3recomp: FAIL (lqa/stqa address computed as I16<<4). With patch 1002 (I16<<2): PASS.
"""
import argparse, os, struct, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kz3ver  # noqa: E402  KZ3_EBOOT_VER=100|114
EBOOT = kz3ver.EBOOT
# EBOOT file offset of the PM, loaded at LS 0xA00: v1.00 0x12af080 (EA 0x012BF080), 1.14 0x132c200 (byte-identical)
PM_OFF = kz3ver.off(0x12af080, "pm")


def extract(src, sig):
    i = src.index(sig)
    j = src.index("{", i) + 1
    depth = 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        j += 1
    return src[i:j]


def stub_words(pm, quad_ls):
    """Replay the handler's generator loop on the instruction quadword at quad_ls."""
    q = list(struct.unpack(">4I", pm[quad_ls - 0xA00:quad_ls - 0xA00 + 16]))
    ls = {}
    r78 = 0x130
    while r78:
        q = [(w - 0x804) & 0xFFFFFFFF for w in q]
        r78 -= 16
        ls[0x3FEC0 + r78] = list(q)
    ls[0x3FFF0] = [0x35002780] * 4
    return ls


HARNESS = r"""
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Patch 1017: g_spu_trampoline_fn is a macro on ctx->tramp_fn now, so there is no thread-local to define. */
void spu_indirect_branch(spu_context* c) { (void)c; }
int spu_interp_run_until(spu_context* c, uint32_t a, uint32_t b) { (void)c; (void)a; (void)b; return 0; }
int spu_overlay_match_sig(const uint8_t h[16]) { (void)h; return 0; }  /* patch 1028 log only */
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {(void)a;(void)w;(void)p;(void)pc;(void)lr;}
int g_spu_ls_watch_n = 0;
/* Patch 1104: the run loop asks whether a lifted entry exists at the PC; the generated stub has none. */
typedef void (*spu_lifted_fn)(spu_context*);
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }
@MICROSTEP@
static spu_context C;
static uint8_t before[SPU_LS_SIZE];
static void plant(const uint32_t* words, unsigned n) {      /* pairs: lsa, 4 words */
    for (unsigned i = 0; i < n; i += 5)
        for (int k = 0; k < 4; k++) {
            uint32_t v = words[i + 1 + k], a = words[i] + 4 * k;
            C.ls[a] = v >> 24; C.ls[a+1] = v >> 16; C.ls[a+2] = v >> 8; C.ls[a+3] = v;
        }
}
static const uint32_t SAVE[] = { @SAVE@ };
static const uint32_t RESTORE[] = { @RESTORE@ };
static uint32_t rd32(uint32_t a) { return (uint32_t)C.ls[a]<<24 | C.ls[a+1]<<16 | C.ls[a+2]<<8 | C.ls[a+3]; }
int main(void) {
    int bad = 0;
    spu_context_init(&C, 0);
    for (uint32_t a = 0; a < SPU_LS_SIZE; a++) C.ls[a] = (uint8_t)(a * 7 + 3);   /* patterned LS */
    plant(SAVE, sizeof SAVE / 4);
    memcpy(before, C.ls, SPU_LS_SIZE);
    for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) C.gpr[r]._u32[k] = 0x10000000u * k + 0x100u * r + 0x5A;
    C.gpr[79] = spu_splat_u32(0xA84);
    C.pc = 0x3FEC0;
    int ok = spu_smc_microstep(&C);
    if (!ok || C.pc != 0xA84) { printf("save: microstep returned %d pc=0x%05X (want 1, 0xA84)\n", ok, C.pc); bad++; }
    for (int r = 0; r <= 75; r++)
        for (int k = 0; k < 4; k++) {
            uint32_t a = 0x3F9C0 + 16 * r + 4 * k;
            uint32_t want = 0x10000000u * k + 0x100u * r + 0x5A;
            if (rd32(a) != want) { if (bad < 8) printf("save: LS 0x%05X = %08X, want r%d word %d = %08X\n", a, rd32(a), r, k, want); bad++; }
        }
    unsigned stray = 0, first = 0;
    for (uint32_t a = 0; a < SPU_LS_SIZE; a++)
        if (C.ls[a] != before[a] && !(a >= 0x3F9C0 && a < 0x3FE80)) { if (!stray) first = a; stray++; }
    if (stray) { printf("save: %u stray LS bytes changed outside 0x3F9C0-0x3FE7F (first 0x%05X)\n", stray, first); bad++; }
    plant(RESTORE, sizeof RESTORE / 4);
    for (int r = 0; r <= 75; r++) C.gpr[r] = spu_splat_u32(0xDEAD0000u + r);
    C.gpr[79] = spu_splat_u32(0xAD0);
    C.pc = 0x3FEC0;
    ok = spu_smc_microstep(&C);
    if (!ok || C.pc != 0xAD0) { printf("restore: microstep returned %d pc=0x%05X (want 1, 0xAD0)\n", ok, C.pc); bad++; }
    for (int r = 0; r <= 75; r++)
        for (int k = 0; k < 4; k++)
            if (C.gpr[r]._u32[k] != 0x10000000u * k + 0x100u * r + 0x5A) {
                if (bad < 16) printf("restore: r%d word %d = %08X\n", r, k, C.gpr[r]._u32[k]); bad++; break; }
    printf("%s: save/restore stub of the KZ3 WWS policy module through spu_smc_microstep (%d problem(s))\n",
           bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps3recomp", default=os.path.join(kz3ver.SPU_OUT, "ps3recomp-patched"))
    a = ap.parse_args()
    pt = os.path.abspath(a.ps3recomp)
    pm = open(EBOOT, "rb").read()[PM_OFF:PM_OFF + 12160]
    # The generator code itself, checked against the PM bytes so the test tracks the real handler.
    assert pm[0xA54 - 0xA00:0xA58 - 0xA00] == bytes.fromhex("3081484d"), "lqa $r77,0xA40 not at 0xA54"
    assert pm[0xA9C - 0xA00:0xAA0 - 0xA00] == bytes.fromhex("30815a4d"), "lqa $r77,0xAD0 not at 0xA9C"
    src = open(os.path.join(pt, "runtime/spu/spu_channels.c"), encoding="utf-8", errors="replace").read()
    # Since patch 1068 spu_smc_microstep() is a wrapper (pm-end event marks) around spu_smc_microstep_body(); the
    # harness takes the body under the old name (the definition, not the forward declaration).
    body = "static int spu_smc_microstep_body(spu_context* ctx)\n{"
    if body in src:
        fn = extract(src, body).replace("spu_smc_microstep_body", "spu_smc_microstep", 1)
    else:
        fn = extract(src, "static int spu_smc_microstep(spu_context* ctx)")

    def arr(ls):
        return ", ".join(f"0x{addr:X}u, " + ", ".join(f"0x{w:08X}u" for w in ws) for addr, ws in sorted(ls.items()))

    c = (HARNESS.replace("@MICROSTEP@", fn).replace("@SAVE@", arr(stub_words(pm, 0xA40)))
         .replace("@RESTORE@", arr(stub_words(pm, 0xAD0))))
    with tempfile.TemporaryDirectory(dir=kz3ver.SPU_OUT) as t:
        cf, exe = os.path.join(t, "t.c"), os.path.join(t, "t")
        open(cf, "w").write(c)
        subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-I", os.path.join(pt, "runtime/spu"),
                        "-I", os.path.join(pt, "include"), "-o", exe, cf], check=True)
        r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout, end="")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
