#!/usr/bin/env bash
# Pass 122: port/spu/kz3_native_post.c kz3_post_blend against a scalar model of the SPU code it replaces (mpy of the
# signed low halfwords, rotmai 8, a, cgt + selb at 255, per pixel with the first byte of the destination pixel as the
# coverage). Random buffers incl. extreme values, counts 4..800, in-place; refused arguments (count not a multiple of
# 4, zero, overlap, unaligned) must leave LS and registers untouched. Self-contained: no game data, patched toolkit only.
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
int kz3_post_blend(spu_context* ctx);
static spu_context A, B;
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main(void)
{
    int bad = 0, n_ok = 0, n_refused = 0;
    for (int t = 0; t < 3000; t++) {
        memset(A.ls_store, 0, sizeof A.ls_store);
        A.ls = A.ls_store;
        const uint32_t n = 4 * (1 + rnd() % 200), a = 0x8000 + 16 * (rnd() % 0x400), b = 0x20000 + 16 * (rnd() % 0x400);
        for (uint32_t i = 0; i < 4 * n + 16; i++) {
            const int m = t % 3;
            A.ls[a + i] = m == 1 ? ((rnd() & 1) ? 0 : 255) : (uint8_t)rnd();
            A.ls[b + i] = m == 1 ? ((rnd() & 1) ? 0 : 255) : (uint8_t)rnd();
        }
        uint32_t cn = n;
        const int mode = t % 11;   /* 0: count not a multiple of 4, 1: zero, 2: unaligned dest */
        if (mode == 0) cn = n + 1 + rnd() % 3;
        if (mode == 1) cn = 0;
        const uint32_t ca = mode == 2 ? a + 4 : a;
        B = A;
        B.ls = B.ls_store;
        A.gpr[3]._u32[0] = ca; A.gpr[4]._u32[0] = b; A.gpr[5]._u32[0] = cn;
        B.gpr[3] = A.gpr[3]; B.gpr[4] = A.gpr[4]; B.gpr[5] = A.gpr[5];
        const int took = kz3_post_blend(&B);
        if (mode <= 2) {
            if (took || memcmp(B.ls_store, A.ls_store, sizeof A.ls_store) || memcmp(&B.gpr[5], &A.gpr[5], 16)) { bad++; printf("refusal mode %d failed\n", mode); }
            n_refused++;
            continue;
        }
        if (!took) { bad++; printf("declined t=%d\n", t); continue; }
        /* scalar model, whole pixels */
        for (uint32_t p = 0; p < n; p++) {
            const int a0 = A.ls[a + 4 * p];
            for (int j = 0; j < 4; j++) {
                int v = A.ls[a + 4 * p + j] + (((int)A.ls[b + 4 * p + j] * (256 - a0)) >> 8);
                A.ls[a + 4 * p + j] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
        if (memcmp(A.ls_store, B.ls_store, sizeof A.ls_store)) { bad++; printf("differs t=%d n=%u\n", t, n); }
        n_ok++;
    }
    printf("blend: %d equal-checked, %d refusals, %d bad\n", n_ok, n_refused, bad);
    return bad != 0;
}
C
cc -std=gnu17 -O1 -msse4.1 -w -I"$ROOT/port/spu" -I"$T/runtime/spu" -I"$T/include" -I"$T/runtime" "$OUT/t.c" "$ROOT/port/spu/kz3_native_post.c" "$ROOT/port/spu/kz3_native_dof.c" "$ROOT/port/spu/kz3_native_lp.c" "$ROOT/tools/spu/native_xr_stubs.c" -lm -o "$OUT/t" || { echo "FAIL build"; exit 1; }
"$OUT/t" && echo PASS || { echo FAIL; exit 1; }
