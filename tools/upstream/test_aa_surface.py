#!/usr/bin/env python3
"""Focused test for ps3recomp patch 2016 (antialiased render surfaces at sample resolution), the upstream fix list.

An RSX surface with antialiasing holds its sample grid in memory: SURFACE_FORMAT bits [15:12] = 3 (DIAGONAL_CENTERED_2)
is 2x1 samples per pixel, 4 (SQUARE_CENTERED_4) and 5 (SQUARE_ROTATED_4) are 2x2; 0..2 are one sample. Killzone 3 draws a
320x180 Z16 pass with format 0x5128 and reads its zeta back as a 640x360 texture. Extracts the real
`rsx_dsp_get_surface` from libs/video/rsx_dispatch.c of upstream (third_party/ps3recomp, v0.13.0) and of the tree under
test (default work/ps3recomp-patched; OLD_TREE / NEW_TREE override), builds it against the tree's own rsx_dispatch.h
with a register file holding SURFACE_CLIP 16,8 320x180 and each antialias mode, and checks the clip rectangle it
reports (clip x/y/w/h scaled by the sample grid). v0.13.0 ignored the field; v0.15.0 absorbed the rule (PR #281), so
upstream must now pass too. Negative control: the tree under test with the 2x2 rule limited to mode 5 must fail. Needs a C compiler (cc). Prints one PASS/FAIL line."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_dispatch.h"
%s
%s
static rsx_dispatch R;
int main(void) {
    for (unsigned aa = 0; aa <= 5; aa++) {
        rsx_dsp_surface s; memset(&s, 0, sizeof s);
        R.regs[M_RT_HORIZ >> 2]  = 16u | (320u << 16);
        R.regs[M_RT_VERT >> 2]   = 8u | (180u << 16);
        R.regs[M_RT_FORMAT >> 2] = 0x28u | (aa << 12);          /* depth Z16-ish bits as KZ3's 0x5128; aa in [15:12] */
        rsx_dsp_get_surface(&R, &s);
        printf("%%u %%u %%u %%u %%u\n", aa, s.clip_x, s.clip_y, s.clip_w, s.clip_h);
    }
    return 0;
}
'''
# aa -> expected (x, y, w, h)
WANT = {0: (16, 8, 320, 180), 1: (16, 8, 320, 180), 2: (16, 8, 320, 180), 3: (32, 8, 640, 180), 4: (32, 16, 640, 360), 5: (32, 16, 640, 360)}

def run(tree, src):
    fn = X.function(src, r"void rsx_dsp_get_surface\(const rsx_dispatch\* rsx, rsx_dsp_surface\* out\)\n\{")
    names = sorted(set(re.findall(r"\bM_[A-Z0-9_]+", fn)))
    defs = [X.define(src, "DMA_HANDLE_MAIN")] + [X.define(src, n) for n in names]
    dma = X.function(src, r"static u32 dma_to_location\(u32 handle\)\n\{")
    failed, out = X.build_run(HARNESS % ("\n".join(defs), dma + "\n" + fn), "c",
                              ("-I", os.path.join(tree, "libs/video"), "-I", os.path.join(tree, "include")))
    if failed:
        print(out[:800]); return None
    return {int(l.split()[0]): tuple(map(int, l.split()[1:])) for l in out.strip().splitlines()}

old_t, new_t = X.trees()
rd = lambda t: open(os.path.join(t, "libs/video/rsx_dispatch.c")).read()
r_old, r_new = run(old_t, rd(old_t)), run(new_t, rd(new_t))
bug = "(out->antialias == 4 || out->antialias == 5) ? 2u : 1u"   # upstream's 2x2 rule (PR #281)
src = rd(new_t)
assert bug in src, "AA sample rule not found in the tree under test"
r_ctl = run(new_t, src.replace(bug, "(out->antialias == 5) ? 2u : 1u"))
for label, r in (("upstream", r_old), ("tree under test", r_new), ("negative control", r_ctl)):
    bad = sorted(a for a in WANT if not r or r.get(a) != WANT[a])
    print(f"{label:16s} 320x180 clip at (16,8): " + (" ".join(f"aa{a}={r[a][2]}x{r[a][3]}" for a in sorted(r)) if r else "build failed")
          + f"   modes wrong: {bad}")
ok = lambda r: bool(r) and all(r.get(a) == WANT[a] for a in WANT)
X.verdict_absorbed("2016 AA surface sample resolution", "PR #281, v0.15.0", ok(r_old), ok(r_new), not ok(r_ctl))
