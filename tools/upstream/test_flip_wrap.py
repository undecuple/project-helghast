#!/usr/bin/env python3
"""Focused test for ps3recomp patch 1010 (flip HLE wraps only the runtime's own ring), the upstream fix list.

Extracts the real `gcm_flip_into_fifo` (and `gcm_flip_wrap_ok` when present) from upstream and from a tree under test
(default work/ps3recomp-patched; NEW_TREE=dir), compiles them against stubs (guest memory, IO mapping, get offset),
and flips on two contexts that are both "nearly full" with the walker far past their begin:
  runtime ctx: buffer-full callback descriptor -> GCM_FIFO_CALLBACK_SENTINEL_EA (as cellGcmSetupContext installs)
  title ctx:   callback descriptor -> a title function
Expected for the tree under test, per GCM_FLIP_WRAP: unset: runtime wraps, title doesn't; 0: neither; 1: both.
Upstream wraps both (the bug for title contexts). Checks the wrap decision and the stores (marker, JUMP, current),
not the walker. Needs a C compiler (cc)."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__)); import _extract as X

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
TREES = {"upstream": os.path.join(ROOT, "third_party/ps3recomp"),
         "tree under test": os.environ.get("NEW_TREE", os.path.join(ROOT, "work/ps3recomp-patched"))}
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32; typedef unsigned long long u64;
#define VM_HLE_INJECT_BASE 0x00F00000u
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8
%s
static unsigned char mem[0x01000000];
static u32 vm_read32(u32 a) { return (u32)mem[a] << 24 | mem[a+1] << 16 | mem[a+2] << 8 | mem[a+3]; }
static void vm_write32(u32 a, u32 v) { for (int i = 0; i < 4; i++) mem[a+i] = (unsigned char)(v >> (24 - 8*i)); }
#define IO_BASE 0x00100000u
#define IO_SIZE 0x00800000u
static u32 gcm_ea2io(u32 ea) { return (ea >= IO_BASE && ea < IO_BASE + IO_SIZE) ? ea - IO_BASE : 0xFFFFFFFFu; }
static u32 s_fifo_getoff;
static volatile u32 s_fifo_flips_queued;                 /* counted by gcm_flip_into_fifo since #343 (9343) */
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM] = {1, 1};
%s
%s
/* ctx at C: begin B, end B+0x30000 (KZ3-sized), current near the end; callback descriptor at C+0x10 */
static int flip(u32 C, u32 B, u32 func, u32 *cur_after, u32 *jump) {
    u32 cur = B + 0x20000;                                 /* end - cur = 0x10000: room for the marker, below GCM_FLIP_WRAP_BYTES */
    vm_write32(C, B); vm_write32(C + 4, B + 0x30000); vm_write32(C + 8, cur);
    vm_write32(C + 0xC, C + 0x10); vm_write32(C + 0x10, func); vm_write32(C + 0x14, 0);
    vm_write32(cur + 4, 0);
    s_fifo_getoff = gcm_ea2io(B) + 0x28000;               /* walker well past begin + GCM_RECYCLE_MARGIN */
    int r = gcm_flip_into_fifo(C, 1);
    *cur_after = vm_read32(C + 8); *jump = vm_read32(cur + 4);
    return r && *cur_after == B && *jump == (0x20000000u | gcm_ea2io(B));
}
int main(void) {
    u32 ca, j;
    int rt = flip(0x00200000u, 0x00200100u, GCM_FIFO_CALLBACK_SENTINEL_EA, &ca, &j);
    int ti = flip(0x00400000u, 0x00400100u, 0x012D6A78u, &ca, &j);
    printf("%%d %%d\n", rt, ti);
    return 0;
}
'''
NAMES = ("GCM_FLIP_WRAP_BYTES", "GCM_RECYCLE_MARGIN", "GCM_RECYCLE_SLACK", "GCM_FLIP_MARKER", "GCM_FIFO_CALLBACK_SENTINEL_EA")
EXPECT = {None: (1, 0), "0": (0, 0), "1": (1, 1)}
fail = 0
for label, tree in TREES.items():
    src = open(os.path.join(tree, "libs/video/cellGcmSys.c")).read()
    defs = "\n".join(X.define(src, n) for n in NAMES)
    ok_fn = X.function(src, r"static int gcm_flip_wrap_ok\(u32 ctx\)") or ""
    fn = X.function(src, r"static int gcm_flip_into_fifo\(u32 ctx, u32 bufferId\)")
    with tempfile.TemporaryDirectory() as d:
        open(f"{d}/t.c", "w").write(HARNESS % (defs, ok_fn, fn))
        subprocess.run(["cc", "-O0", "-w", f"{d}/t.c", "-o", f"{d}/t"], check=True)
        for env in (None, "0", "1"):
            e = dict(os.environ); e.pop("GCM_FLIP_WRAP", None)
            if env is not None: e["GCM_FLIP_WRAP"] = env
            rt, ti = map(int, subprocess.run([f"{d}/t"], env=e, capture_output=True, text=True, check=True).stdout.split())
            want = EXPECT[env] if label != "upstream" else (1, 1)
            ok = (rt, ti) == want
            if label != "upstream": fail += not ok
            print(f"{label:16s} GCM_FLIP_WRAP={env if env is not None else 'unset':5s} runtime ctx "
                  f"{'wraps' if rt else 'no wrap'}, title ctx {'wraps' if ti else 'no wrap'}"
                  + ("" if label == "upstream" else f" -> {'PASS' if ok else 'FAIL'}"))
sys.exit(1 if fail else 0)
