#!/usr/bin/env python3
"""Focused regression test for ps3recomp patch 2004's live-draw sampler key (the upstream fix list).

Extracts the real `sampler_key` function from upstream (third_party/ps3recomp, v0.15.0: a u64 key since PR #321, which
keys filter, wrap and both LODs) and from the patched tree (work/ps3recomp-patched), compiles each against a minimal
rsx_dsp_texture with the four fields it reads, and checks sampler states that differ only in a field decode_sampler uses:
  - the upstream key must merge them (the bug: these pairs share one D3D12 sampler descriptor), except the pairs
    PR #321 fixed (min LOD), where upstream must separate them too,
  - the new key must keep them apart,
and a control pair (border colour differs, but no wrap mode is BORDER) that both keys may merge.
Checks the key only: not decode_sampler's D3D12 output, not the descriptor cache. Needs a C compiler (cc)."""
import os, re, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
OLD = os.path.join(ROOT, "third_party/ps3recomp/libs/video/rsx_live_draw.c")
NEW = os.environ.get("NEW_TREE", os.path.join(ROOT, "work/ps3recomp-patched")) + "/libs/video/rsx_live_draw.c"

def extract(path):
    src = open(path).read()
    m = re.search(r"static (u32|u64|void) sampler_key\(const rsx_dsp_texture\* t[^)]*\)\s*\{", src)
    if not m: sys.exit(f"sampler_key not found in {path}")
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0); i += 1
    return src[m.start():i]

# (name, (filter, wrap, control0, border) A, B): states that decode differently. UP_FIXED: pairs upstream keys apart now.
UP_FIXED = {"min LOD top bits (control0 bit 25)"}
PAIRS = [
    ("min LOD top bits (control0 bit 25)", (0x00020000, 0x010101, 0x00000000, 0), (0x00020000, 0x010101, 0x02000000, 0)),
    ("LOD bias only (filter bits 0-12)",   (0x00020000, 0x010101, 0x00000000, 0), (0x00020010, 0x010101, 0x00000000, 0)),
    ("anisotropy only (control0 bits 4-6)",(0x02020000, 0x010101, 0x00000000, 0), (0x02020000, 0x010101, 0x00000040, 0)),
    ("border colour with BORDER wrap",     (0x00020000, 0x040404, 0x00000000, 0xFF000000), (0x00020000, 0x040404, 0x00000000, 0xFFFFFFFF)),
]
CONTROL = ("border colour, no BORDER wrap", (0x00020000, 0x010101, 0, 0xFF000000), (0x00020000, 0x010101, 0, 0xFFFFFFFF))

HARNESS = r'''
#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct { u32 filter, wrap, control0, border_color; } rsx_dsp_texture;
%s
static void key4(const rsx_dsp_texture* t, u32 k[4]) {
#ifdef NEWKEY
    sampler_key(t, k);
#else
    u64 x = sampler_key(t); memset(k, 0, 16); memcpy(k, &x, sizeof x);
#endif
}
int main(void) {
    u32 v[][8] = { %s };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        rsx_dsp_texture a = { v[i][0], v[i][1], v[i][2], v[i][3] }, b = { v[i][4], v[i][5], v[i][6], v[i][7] };
        u32 ka[4], kb[4]; key4(&a, ka); key4(&b, kb);
        printf("%%d\n", memcmp(ka, kb, 16) == 0);
    }
    return 0;
}
'''

def run(path, new):
    cases = PAIRS + [CONTROL]
    vals = ", ".join("{%s}" % ", ".join(hex(x) for x in a + b) for _, a, b in cases)
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(c, "w").write(HARNESS % (extract(path), vals))
        subprocess.run(["cc", "-O0", "-w"] + (["-DNEWKEY"] if new else []) + [c, "-o", exe], check=True)
        return [l == "1" for l in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()]

old, new = run(OLD, False), run(NEW, True)
fail = 0
for (name, _, _), o, n in zip(PAIRS, old, new):
    ok = (not o if name in UP_FIXED else o) and not n
    fail += not ok
    print(f"{'PASS' if ok else 'FAIL'}  {name}: upstream key {'merges' if o else 'separates'}"
          f"{' (fixed upstream, PR #321)' if name in UP_FIXED else ''}, new key {'merges' if n else 'separates'}")
print(f"info  {CONTROL[0]}: old {'merges' if old[-1] else 'separates'}, new {'merges' if new[-1] else 'separates'} (merging is correct: border colour unused)")
print(f"{len(PAIRS) - fail}/{len(PAIRS)} pass"); sys.exit(1 if fail else 0)
