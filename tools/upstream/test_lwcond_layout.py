#!/usr/bin/env python3
"""Focused test for ps3recomp patch 0003a (sys_lwcond_t layout), the upstream fix list.

Extracts the real `sys_lwcond_create` from upstream (third_party/ps3recomp) and from a tree under test (default
work/ps3recomp-patched; NEW_TREE=dir to override), compiles it against a guest-memory stub, creates an lwcond and
checks the 8-byte sys_lwcond_t: +0x00 u32 lwmutex EA, +0x04 u32 queue id (nonzero), nothing written past +0x08.
Built without _WIN32, so the non-Windows id path is compiled; the layout stores are shared by both paths.
Checks layout only, not wait/signal (0003b; patch 1122's tools/spu/test_r104_lv2_sync.sh covers those). Needs a C++ compiler (c++)."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__)); import _extract as X

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
TREES = {"upstream": os.path.join(ROOT, "third_party/ps3recomp"),
         "tree under test": os.environ.get("NEW_TREE", os.path.join(ROOT, "work/ps3recomp-patched"))}
HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
static uint8_t mem[0x10000];
static void vm_write32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
static void vm_write64(uint32_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
static uint32_t rd(uint32_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
struct ppu_context { uint64_t gpr[32]; uint32_t thread_id; };
%s
%s
int main() {
    const uint32_t LWC = 0x1000, MTX = 0x2468ACE0u & 0xFFFF;   /* lwmutex EA (any value; not dereferenced) */
    memset(mem, 0xAA, sizeof mem);
    ppu_context c = {}; c.gpr[3] = LWC; c.gpr[4] = 0x13572468u; sys_lwcond_create(&c);
    unsigned guard_ok = 1; for (int i = 8; i < 16; i++) guard_ok &= mem[LWC + i] == 0xAA;
    printf("%%08X %%08X %%u %%llu\n", rd(LWC), rd(LWC + 4), guard_ok, (unsigned long long)c.gpr[3]);
    (void)MTX; return 0;
}
'''
fail = 0
for label, tree in TREES.items():
    src = open(os.path.join(tree, "runtime/ppu/ppu_sysprx.cpp")).read()
    fn = X.function(src, r"static void sys_lwcond_create\(ppu_context\* ctx\)")
    defs = "\n".join(X.define(src, n) for n in ("LWC_MUTEX", "LWC_QUEUE", "LWC_ID_BASE")) if "LWC_MUTEX" in src else ""
    if "lv2s_queue q; } g_lwc" in src:   # KZ3 patch 1122: platform-neutral lwcond slots (lv2_sync_core.h queue)
        import re
        slot = re.search(r"static struct LwcSlot \{[^}]*\} g_lwc\[LWC_SLOTS\];\s*static volatile long g_lwc_next = 0;", src).group(0)
        defs += ("\n" + X.define(src, "LWC_SLOTS") + "\ntypedef struct lv2s_queue { void* head; void* tail; long n; } lv2s_queue;"
                 "\n#define PPU_FENCE(o)\n#define _InterlockedIncrement(p) (++*(p))\n" + slot)
    with tempfile.TemporaryDirectory() as d:
        open(f"{d}/t.cpp", "w").write(HARNESS % (defs, fn))
        subprocess.run(["c++", "-O0", "-w", f"{d}/t.cpp", "-o", f"{d}/t"], check=True)
        w0, w4, guard, ret = subprocess.run([f"{d}/t"], capture_output=True, text=True, check=True).stdout.split()
    ok = w0 == "13572468" and w4 != "00000000" and w4 != "AAAAAAAA" and guard == "1" and ret == "0"
    print(f"{label:16s} +0x00={w0} +0x04={w4} past-struct-untouched={guard == '1'} ret={ret} -> "
          f"{'layout OK' if ok else 'layout WRONG'}")
    if label != "upstream": fail += not ok
sys.exit(1 if fail else 0)
