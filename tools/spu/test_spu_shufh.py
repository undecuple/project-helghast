#!/usr/bin/env python3
"""Patch 1108 host test, lifter half: spu_lifter._shufb1_hoist rewrites only the shufb1 uses whose control the
function never writes, and only in functions with a loop whose only uses of ctx are gpr, the LS accessors and
SPU_RET. Synthetic bodies; no game data. Usage: test_spu_shufh.py PATCHED_TOOLKIT_DIR"""
import os
import sys

sys.path.insert(0, os.path.join(sys.argv[1], "tools"))
from spu_lifter import _shufb1_hoist  # noqa: E402

LOOP = ["loc_00000100:",
        "ctx->gpr[5] = spu_shufb1(ctx->gpr[4], ctx->gpr[11]);",
        "ctx->gpr[6] = spu_shufb1(ctx->gpr[5], ctx->gpr[12]);",
        "ctx->gpr[7] = spu_ls_read128(ctx, ctx->gpr[3]._u32[0] + 0x10);",
        "ctx->gpr[12] = spu_ai(ctx->gpr[12], 1);",
        "if (ctx->gpr[9]._u32[0] != 0) goto loc_00000100;",
        "SPU_RET(ctx);"]
cases = [
    ("loop, r11 invariant, r12 written", LOOP, [11]),
    ("no loop", [b for b in LOOP if "goto" not in b], []),
    ("call in the function", LOOP[:1] + ["job_x_spu_func_00000200(ctx);"] + LOOP[1:], []),
    ("ctx->pc use", LOOP[:1] + ["ctx->pc = 0x100u;"] + LOOP[1:], []),
    ("address of a register", LOOP[:1] + ["spu_x(&ctx->gpr[11]);"] + LOOP[1:], []),
    ("word write counts as a write", LOOP[:1] + ["ctx->gpr[11]._u32[0] = 3;"] + LOOP[1:], []),
    ("compound write counts as a write", LOOP[:1] + ["ctx->gpr[11]._u32[1] |= 3;"] + LOOP[1:], []),
    ("comparison is not a write", LOOP[:1] + ["if (ctx->gpr[11]._u32[0] == 0) goto loc_00000100;"] + LOOP[1:], [11]),
    ("forward goto only", ["if (ctx->gpr[9]._u32[0] != 0) goto loc_00000200;"] + LOOP[1:3] + ["loc_00000200:", "SPU_RET(ctx);"], []),
]
bad = 0
for name, body, want in cases:
    new, decs = _shufb1_hoist(list(body))
    ok = decs == want
    if want:
        ok = ok and any("spu_shufb1_h(ctx->gpr[4], spu__sd11, ctx->gpr[11])" in b for b in new) \
            and any("spu_shufb1(ctx->gpr[5], ctx->gpr[12])" in b for b in new)
    else:
        ok = ok and new == list(body)
    if not ok:
        bad += 1
        print(f"FAIL {name}: decs {decs}, want {want}")
print(f"test_spu_shufh.py: {len(cases)} lifter cases, {bad} failed")
sys.exit(1 if bad else 0)
