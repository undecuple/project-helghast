#!/usr/bin/env python3
"""Focused test for the staged PR docs/upstream/submission/5003-np-commerce2-createctx-args.diff.

sceNpCommerce2CreateCtx's SDK signature is (version, npId, handler, arg, ctx_id): the context id is the FIFTH argument.
Upstream master declares three parameters, so a title's call (five arguments in r3..r7, as the flat HLE adapter passes
them) writes the id over the third argument, the title's handler descriptor, and leaves its ctx_id variable untouched.
The test compiles libs/network/sceNpCommerce2.c against stub guest memory, calls it through a five-argument function
pointer (the way the adapter does), and checks that the handler descriptor bytes are unchanged and the id is stored
(big-endian) at the fifth argument. Negative control: the diff's function with the store redirected to the handler
fails the same check. Needs cc. Prints one PASS/FAIL line."""
import os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _extract as X, _staged

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint8_t* vm_base; uint32_t ppu_vm_size; uint32_t g_ww_lo, g_ww_hi; int g_resv_store_active;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int n) { (void)a; (void)v; (void)n; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
#include "%(src)s"
int main(void) {
    static uint8_t mem[0x100000]; vm_base = mem; ppu_vm_size = sizeof mem;
    memset(mem + 0x2000, 0xAA, 16);              /* the title's handler descriptor */
    memset(mem + 0x3000, 0xBB, 4);               /* the title's ctx_id variable */
    sceNpCommerce2Init();
    typedef int32_t (*five)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    int32_t rc = ((five)sceNpCommerce2CreateCtx)(2, 0x1000, 0x2000, 0, 0x3000);
    int handler_intact = 1; for (int i = 0; i < 16; i++) if (mem[0x2000 + i] != 0xAA) handler_intact = 0;
    int id_stored = mem[0x3000] == 0 && mem[0x3001] == 0 && mem[0x3002] == 0 && mem[0x3003] == 0;   /* id 0, BE */
    printf("%%d %%d\n", rc == 0 && handler_intact, id_stored);
    return 0;
}
'''

def ok(tree, redirect=False):
    src = open(os.path.join(tree, "libs/network/sceNpCommerce2.c")).read()
    if redirect:
        s2 = src.replace("vm_write32((u32)(uintptr_t)ctx, (u32)i);", "vm_write32(handler, (u32)i);")
        if s2 == src: sys.exit("FAIL: negative control did not apply")
        src = s2
    with tempfile.TemporaryDirectory() as d:
        p = f"{d}/c.c"; open(p, "w").write(src)
        failed, out = X.build_run(HARNESS % {"src": p}, flags=["-I", os.path.join(tree, "libs/network"), "-I", tree,
                                                              "-I", os.path.join(tree, "include"), "-I", os.path.join(tree, "libs")])
        return (not failed) and out.strip().splitlines()[-1:] == ["1 1"]

old, new = _staged.trees("5003-np-commerce2-createctx-args.diff")
X.verdict("5003 sceNpCommerce2CreateCtx stores the context id at the fifth argument", not ok(old), ok(new), not ok(new, redirect=True))
