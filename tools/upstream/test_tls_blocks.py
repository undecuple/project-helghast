#!/usr/bin/env python3
"""Focused test for ps3recomp patch 3003 (PPU TLS blocks are page sized, not 0x8000), the upstream fix list.

Upstream absorbed the block size in v0.15.0 (PR #253; its own tools/test_ppu_tls_blocks.py runs in
test_upstream_suite.sh), so upstream must now pass too.
Extracts the real `ppu_tls_new_block` from runtime/ppu/ppu_loader.cpp of upstream (third_party/ps3recomp) and of
the tree under test (default work/ps3recomp-patched; OLD_TREE / NEW_TREE override), and hands out 100 thread TLS blocks
from the 1 MB window at 0x10F00000 (g_tls_memsz = 0x240, as a small title). Checks: every block is granted (v0.13.0
counted the 0x7000 r13 bias as storage, so only 31 fit and the rest silently shared the main block), no two blocks
overlap each other or the main block's image, and the .tdata image is copied. Negative control: the tree under test
with the old size line put back must fail. Needs a C++ compiler (c++). Prints one PASS/FAIL line."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#define PPU_TLS_IMG 0x10F00000u
static uint8_t* vm_base; static uint32_t ppu_vm_size = 0x11000000u;
static uint32_t g_tls_vaddr = 0x1000, g_tls_filesz = 0x100, g_tls_memsz = 0x240;
%s
int main() {
    vm_base = (uint8_t*)calloc(0x11000000u, 1);
    for (uint32_t i = 0; i < g_tls_filesz; i++) vm_base[g_tls_vaddr + i] = (uint8_t)(i + 1);
    const int N = 100; uint32_t data[N]; int granted = 0, overlap = 0, badcopy = 0;
    for (int i = 0; i < N; i++) {
        uint32_t tp = ppu_tls_new_block();
        data[i] = tp ? tp - 0x7000u : 0;                      /* TLS data sits at r13 - 0x7000 */
        if (!tp) continue;
        granted++;
        if (data[i] < PPU_TLS_IMG + g_tls_memsz) overlap++;   /* main block's image at PPU_TLS_IMG */
        for (int j = 0; j < i; j++) if (data[j] && data[i] < data[j] + g_tls_memsz && data[j] < data[i] + g_tls_memsz) overlap++;
        if (memcmp(vm_base + data[i], vm_base + g_tls_vaddr, g_tls_filesz)) badcopy++;
    }
    printf("%%d %%d %%d\n", granted, overlap, badcopy);
    return 0;
}
'''

def run(src):
    fn = X.function(src, r"static uint32_t ppu_tls_new_block\(void\)")
    if not fn: return None
    failed, out = X.build_run(HARNESS % fn, "c++")
    if failed: return None
    return tuple(map(int, out.split()))

old_t, new_t = X.trees()
rd = lambda t: open(os.path.join(t, "runtime/ppu/ppu_loader.cpp")).read()
res = {}
for label, src in (("upstream", rd(old_t)), ("tree under test", rd(new_t))):
    res[label] = run(src)
    print(f"{label:16s} blocks granted/overlaps/bad copies: {res[label]} (want (100, 0, 0))")
bug = "total = (g_tls_memsz + 0xFFFu) & ~0xFFFu;"
src = rd(new_t)
assert bug in src, "patched size line not found"
ctl = run(src.replace(bug, "total = (g_tls_memsz + 0x7000u + 0xFFFu) & ~0xFFFu;"))
print(f"{'negative control':16s} blocks granted/overlaps/bad copies: {ctl} (old size line put back)")
good = lambda r: r == (100, 0, 0)
X.verdict_absorbed("3003 PPU TLS block size", "PR #253, v0.15.0", good(res["upstream"]), good(res["tree under test"]),
                   not good(ctl))
