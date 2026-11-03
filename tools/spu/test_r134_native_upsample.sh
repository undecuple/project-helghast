#!/usr/bin/env bash
# Pass 134: port/spu/kz3_native_post.c kz3_post_upsample8 (the Upsample RGBA8 row kernel, job_011d4680 @0x52C0)
# against the closed-form model of the SPU code it replaces: per channel byte, q = o >> 1, nb = q -/+ 1 (o even/odd),
# h(y) = 3 X(y, q) + X(y, nb), out = (3 h(0) + h(ov)) >> 4, ov = -1 / +1 for the first / second output row. Random
# images incl. all-0 / all-255, widths 8..192, pitches with slack; the neighbours of the output must stay untouched;
# refused arguments (count not a multiple of 8, zero, unaligned, overlap, out of the local store) must leave LS and
# registers untouched. Negative control: a +8 rounding in the model must be caught. Self-contained: no game data,
# patched toolkit only (SKIP without it). The lifted-code comparison is tools/spu/native_diff.py --slot 18,19.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_context.h ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/t.c" <<'C'
#include "kz3_native_jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int kz3_post_upsample8(spu_context* ctx);
static spu_context A, B;
static uint32_t rs = 777;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static void set(spu_context* c, uint32_t d, uint32_t s, uint32_t sp, uint32_t dp, uint32_t wn)
{
    c->gpr[3]._u32[0] = d; c->gpr[4]._u32[0] = s; c->gpr[5]._u32[0] = sp; c->gpr[6]._u32[0] = dp; c->gpr[7]._u32[0] = wn;
}
int main(void)
{
    int bad = 0, n_ok = 0, n_refused = 0, caught = 0;
    for (int t = 0; t < 2000; t++) {
        memset(A.ls_store, 0, sizeof A.ls_store);
        A.ls = A.ls_store;
        const uint32_t wn = 8 * (1 + rnd() % 24), sp = 16 * ((wn / 2 + 2) + rnd() % 6), dp = 4 * wn + 16 * (rnd() % 3);
        const uint32_t src = 0x8000 + 16 * (rnd() % 0x200), dst = 0x20000 + 16 * (rnd() % 0x200);
        for (uint32_t i = 0; i < 0x40000; i++) A.ls[i] = t % 3 == 1 ? ((rnd() & 1) ? 0 : 255) : (uint8_t)rnd();
        const int mode = t % 13;   /* 0: wn not a multiple of 8, 1: zero, 2: unaligned dst, 3: overlap, 4: past the LS */
        uint32_t cw = wn, cd = dst, cs = src;
        if (mode == 0) cw = wn + 1 + rnd() % 7;
        if (mode == 1) cw = 0;
        if (mode == 2) cd = dst + 4;
        if (mode == 3) cd = src + 16;
        if (mode == 4) cd = SPU_LS_SIZE - 16;
        B = A; B.ls = B.ls_store;
        set(&A, cd, cs, sp, dp, cw); set(&B, cd, cs, sp, dp, cw);
        const int took = kz3_post_upsample8(&B);
        if (mode <= 4) {
            if (took || memcmp(B.ls_store, A.ls_store, sizeof A.ls_store) || memcmp(&B.gpr[3], &A.gpr[3], 80)) { bad++; printf("refusal mode %d failed\n", mode); }
            n_refused++;
            continue;
        }
        if (!took) { bad++; printf("declined t=%d\n", t); continue; }
        for (int oy = 0; oy < 2; oy++)
            for (uint32_t o = 0; o < wn; o++)
                for (int c = 0; c < 4; c++) {
                    const uint32_t q = o >> 1, nb = (o & 1) ? q + 1 : q - 1;
                    const int ov = oy ? 1 : -1;
#define X(y, p) A.ls[src + (y) * (int)sp + 4 * (int)(p) + c]
                    const int h0 = 3 * X(0, q) + X(0, nb), hv = 3 * X(ov, q) + X(ov, nb);
#undef X
                    const int v = (3 * h0 + hv) >> 4, vr = (3 * h0 + hv + 8) >> 4;
                    A.ls[dst + oy * dp + 4 * o + c] = (uint8_t)v;
                    if (vr != v) caught++;   /* a model with +8 rounding would disagree with the kernel here */
                }
        if (memcmp(A.ls_store, B.ls_store, sizeof A.ls_store)) { bad++; printf("differs t=%d wn=%u\n", t, wn); }
        n_ok++;
    }
    printf("upsample8: %d equal-checked, %d refusals, %d bad, rounding control %s\n", n_ok, n_refused, bad,
           caught > 0 ? "distinguished" : "NOT distinguished");
    return bad != 0 || caught <= 0;
}
C
cc -std=gnu17 -O1 -msse4.1 -w -I"$ROOT/port/spu" -I"$T/runtime/spu" -I"$T/include" -I"$T/runtime" "$OUT/t.c" "$ROOT/port/spu/kz3_native_post.c" "$ROOT/port/spu/kz3_native_dof.c" "$ROOT/port/spu/kz3_native_lp.c" "$ROOT/tools/spu/native_xr_stubs.c" -lm -o "$OUT/t" || { echo "FAIL build"; exit 1; }
"$OUT/t" && echo PASS || { echo FAIL; exit 1; }
