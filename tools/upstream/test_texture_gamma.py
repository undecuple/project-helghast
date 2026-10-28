#!/usr/bin/env python3
"""Focused test for ps3recomp patch 2009 (RSX texture gamma: sRGB decode on fetch), the upstream fix list.

RSX TEXTURE_ADDRESS bits [23:20] ask a texture unit to convert sRGB-encoded channels to linear on fetch. v0.13.0's
fragment decompiler ignores them (sRGB textures come out too bright, reds turn yellow); the patch adds the HLSL
post-pass `rsx_fp_apply_gamma(hlsl, size, gamma_mask)` (3 bits per unit: R, G, B). Extracts it (and its `fp_insert`
helper) from libs/video/rsx_fp_decompiler.c of upstream (third_party/ps3recomp, v0.13.0) and of the tree under test
(default work/ps3recomp-patched; OLD_TREE / NEW_TREE override) and runs it on a fragment shader text. Checks: every
sample of a gamma unit (both `rsx_texN.` and `rsx_tex[N].` spellings, nested parentheses in the arguments) is wrapped
in rsx_srgb(..., float3(r, g, b)) with the unit's own mask bits; units with mask 0 are untouched; the return value is
the number of wrapped samples; the rsx_srgb helper is emitted exactly once; mask 0 changes nothing; a too small buffer
returns -1. Upstream has no such function. Negative control: the tree under test with the R and B mask bits swapped.
Needs a C compiler (cc). Prints one PASS/FAIL line."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t u64;
%s
static const char SRC[] =
    "float4 a = rsx_tex0.Sample(rsx_smp0, saturate(v.xy));\n"
    "float4 b = rsx_tex1.Sample(rsx_smp1, v.zw);\n"
    "float4 c = rsx_tex[3].SampleLevel(rsx_smp3, float2(v.x, (v.y + 1.0) * 0.5), 0.0);\n"
    "float4 d = rsx_tex0.SampleBias(rsx_smp0, v.xy, 1.0);\n";
int main(void) {
    char buf[8192]; int n_helper = 0;
    /* unit 0: R and G (0b011), unit 1: none, unit 3: R G B */
    strcpy(buf, SRC);
    int r = rsx_fp_apply_gamma(buf, sizeof buf, (u64)3 | ((u64)7 << 9));
    for (char* p = buf; (p = strstr(p, "float4 rsx_srgb(")); p++) n_helper++;
    const char* body = strstr(buf, "float4 a =");
    printf("ret=%%d helper=%%d\n%%s", r, n_helper, body ? body : "(no body)\n");
    char b2[8192]; strcpy(b2, SRC);
    int r0 = rsx_fp_apply_gamma(b2, sizeof b2, 0);
    printf("zero=%%d same=%%d\n", r0, strcmp(b2, SRC) == 0);
    char b3[sizeof SRC + 10]; strcpy(b3, SRC);
    printf("small=%%d\n", rsx_fp_apply_gamma(b3, sizeof b3, 1));
    return 0;
}
'''
WANT = """ret=3 helper=1
float4 a = rsx_srgb(rsx_tex0.Sample(rsx_smp0, saturate(v.xy)), float3(1, 1, 0));
float4 b = rsx_tex1.Sample(rsx_smp1, v.zw);
float4 c = rsx_srgb(rsx_tex[3].SampleLevel(rsx_smp3, float2(v.x, (v.y + 1.0) * 0.5), 0.0), float3(1, 1, 1));
float4 d = rsx_srgb(rsx_tex0.SampleBias(rsx_smp0, v.xy, 1.0), float3(1, 1, 0));
zero=0 same=1
small=-1"""

def run(src):
    ins = X.function(src, r"static int fp_insert\(char\* hlsl, u32 hlsl_size, char\* at, const char\* text\)\n\{")
    fn = X.function(src, r"int rsx_fp_apply_gamma\(char\* hlsl, u32 hlsl_size, u64 gamma_mask\)\n\{")
    if not (ins and fn): return "rsx_fp_apply_gamma absent"
    failed, out = X.build_run(HARNESS % (ins + "\n" + fn), "c")
    return "build failed: " + out[:500] if failed else out.strip()

old_t, new_t = X.trees()
rd = lambda t: open(os.path.join(t, "libs/video/rsx_fp_decompiler.c")).read()
r_old, r_new = run(rd(old_t)), run(rd(new_t))
bug = "bits & 1u, (bits >> 1) & 1u, (bits >> 2) & 1u"
src = rd(new_t)
assert bug in src, "mask bit order not found in the tree under test"
r_ctl = run(src.replace(bug, "(bits >> 2) & 1u, (bits >> 1) & 1u, bits & 1u"))
for label, r in (("upstream", r_old), ("tree under test", r_new), ("negative control", r_ctl)):
    print(f"{label:16s} {'matches the expected rewrite' if r == WANT else 'DIFFERS: ' + r.replace(chr(10), ' | ')[:300]}")
X.verdict("2009 texture gamma decode", r_old != WANT and "build failed" not in r_old, r_new == WANT,
          r_ctl != WANT and "build failed" not in r_ctl)
