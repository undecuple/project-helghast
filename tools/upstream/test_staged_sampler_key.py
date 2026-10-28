#!/usr/bin/env python3
"""Focused test for the staged PR docs/upstream/submission/2004-live-draw-sampler-key.diff.

The D3D12 live-draw path caches one sampler descriptor per key (libs/video/rsx_live_draw.c, sampler_key). decode_sampler
reads: min/mag filter (filter bits 16-18, 24-26), the three address modes (wrap bits 0-3, 8-11, 16-19) and max/min LOD
(control0 bits 7-18 and 19-30). The upstream key keeps wrap bits 0-11 only and shifts the 21-bit LOD field left by 18
(14 bits survive), so states that differ only in the W address mode or in min LOD bits above 20 share a descriptor.

The test extracts the real sampler_key from upstream master (OLD_TREE) and from a copy with the diff applied, and
compares state pairs. Pairs decode_sampler tells apart must get different keys (upstream merges them: the bug); pairs
that differ only in bits decode_sampler ignores must keep the same key (no needless descriptors). Negative control:
the new key with the W term cut back to the old wrap mask must be caught. Needs cc. One PASS/FAIL line."""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _extract as X, _staged

def extract(tree, mutate=None):
    src = open(os.path.join(tree, "libs/video/rsx_live_draw.c"), newline="").read().replace("\r\n", "\n")
    m = re.search(r"static (u32|u64) sampler_key\(const rsx_dsp_texture\* t\)\s*\{", src)
    if not m: sys.exit("sampler_key not found")
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0); i += 1
    fn = src[m.start():i]
    if mutate:
        f2 = mutate(fn)
        if f2 == fn: sys.exit("FAIL: negative control did not apply")
        fn = f2
    return fn

# (name, a, b) = (filter, wrap, control0) pairs; SPLIT = decode_sampler distinguishes them, SAME = it cannot.
BASE = (0x00020000, 0x010101, 0x00000000)
SPLIT = [
    ("min LOD bit 25 (control0)", BASE, (0x00020000, 0x010101, 0x02000000)),
    ("min LOD bit 30 (control0)", BASE, (0x00020000, 0x010101, 0x40000000)),
    ("W address mode (wrap bits 16-19)", (0x00020000, 0x010101, 0), (0x00020000, 0x030101, 0)),
    ("max LOD bit 18 (control0; upstream already separates it)", BASE, (0x00020000, 0x010101, 0x00040000)),
]
SAME = [
    ("wrap bits 4-7 (unused)", BASE, (0x00020000, 0x0101F1, 0)),
    ("control0 bits 0-6 (unused)", BASE, (0x00020000, 0x010101, 0x0000007F)),
    ("filter bits 0-12 (unused)", BASE, (0x00020010, 0x010101, 0)),
]

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t u64;
typedef struct { u32 filter, wrap, control0, border_color; } rsx_dsp_texture;
%s
int main(void) {
    u32 v[][6] = { %s };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        rsx_dsp_texture a = { v[i][0], v[i][1], v[i][2], 0 }, b = { v[i][3], v[i][4], v[i][5], 0 };
        printf("%%d\n", (u64)sampler_key(&a) == (u64)sampler_key(&b));
    }
    return 0;
}
'''

def merged(tree, mutate=None):
    cases = SPLIT + SAME
    vals = ", ".join("{%s}" % ", ".join(hex(x) for x in a + b) for _, a, b in cases)
    with tempfile.TemporaryDirectory() as d:
        c, exe = f"{d}/t.c", f"{d}/t"
        open(c, "w").write(HARNESS % (extract(tree, mutate), vals))
        r = subprocess.run(["cc", "-O0", "-w", c, "-o", exe], capture_output=True, text=True)
        if r.returncode: sys.exit("compile error: " + r.stderr[:400])
        return [l == "1" for l in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()]

old_t, new_t = _staged.trees("2004-live-draw-sampler-key.diff")
old, new = merged(old_t), merged(new_t)
ctl = merged(new_t, lambda f: f.replace("wrap_w  = (t->wrap >> 16) & 0xF", "wrap_w  = 0"))
n = len(SPLIT)
for (name, _, _), o, w in zip(SPLIT, old, new[:n]):
    print(f"{'ok  ' if not w else 'BAD '} {name}: upstream {'merges' if o else 'separates'}, new {'merges' if w else 'separates'}")
for (name, _, _), w in zip(SAME, new[n:]):
    print(f"{'ok  ' if w else 'BAD '} {name}: new key {'keeps one descriptor' if w else 'splits (wasteful)'}")
upstream_bad = any(old[:n])                                   # upstream merges at least one distinguishable pair
new_ok = not any(new[:n]) and all(new[n:])
control_bad = any(ctl[:n])                                    # the cut-back W term merges the W pair again
X.verdict("2004 live-draw sampler key covers every field decode_sampler reads", upstream_bad, new_ok, control_bad)
