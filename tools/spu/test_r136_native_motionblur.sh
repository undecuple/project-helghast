#!/usr/bin/env bash
# Pass 136: port/spu/kz3_native_post.c mb_blur_row_5460 (Gfx.Post.MotionBlur blur row, job_011f7d00 @0x5460, gate
# slots 22/23) against an independent scalar model written here from spu-native-specs.md s.11.3 / the RE notes
# native-post.md s.1: the velocity is decoded to pixel units with floor division (tap column = px + floor(sg*vx*c /
# 2^20), tap row = floor(sg*vy*c / 2^16)) instead of the kernel's mpys shifts and byte offsets, 2-D addressing instead
# of the SPU's masked byte sum, the 12 taps summed per channel on splats (nearest plus-side tap first, the far taps of
# each side at half weight), the float ops are the runtime's SPU helpers (the reference semantics). 3000 synthetic
# states (realistic floats or raw random bits, velocity modes zero vector / 0-255 corners / random / b0 = 0, widths
# 4..244, pitch high-halfword garbage) must match bit for bit in the whole local store and all registers; refused
# arguments (n not a multiple of 4, zero, unaligned, dst under the tap window, past the local store) must leave
# everything untouched. Negative control: the kernel built with -DKZ3_NATIVE_NEGCTL (the two tap sides summed the
# other way round) must be caught by the model (the output is bytes of x*256, so an order error shows in about 1 % of
# the states; 3000 states make that certain). Part 2, only with game data: native_diff.py against the lifted SPU code
# (slot 22, the 1.00 twin 23 when its lift exists, and the negative control must FAIL). No MotionBlur records exist
# under work/ (slot 2 was never called on K4), so there is no recorded-state part. Self-contained otherwise (SKIP
# without the patched toolkit).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_context.h ]] || { echo "SKIP: no patched toolkit at $T"; echo "RESULT: SKIP"; exit 0; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/t.c" <<'C'
#include "kz3_native_jobs.h"
#include "spu_helpers.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static spu_context O, A, B;
static uint32_t rs = 136;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t ld32(const uint8_t* ls, uint32_t a) { a &= 0x3FFFC; return (uint32_t)ls[a] << 24 | (uint32_t)ls[a + 1] << 16 | (uint32_t)ls[a + 2] << 8 | ls[a + 3]; }
static void st32(uint8_t* ls, uint32_t a, uint32_t v) { a &= 0x3FFFC; ls[a] = v >> 24; ls[a + 1] = v >> 16; ls[a + 2] = v >> 8; ls[a + 3] = v; }
static u128 ldq(const uint8_t* ls, uint32_t a) { u128 r; a &= 0x3FFF0; for (int i = 0; i < 4; i++) r._u32[i] = ld32(ls, a + 4 * i); return r; }
static long fdiv(long v, long d) { return (long)floor((double)v / (double)d); }
static const int C[6] = {3020, 2520, 1960, 1400, 840, 280};
/* the model: every write the kernel must produce, from the spec's description alone */
static void model(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t dst = c->gpr[3]._u32[0], src = c->gpr[4]._u32[0], vel = c->gpr[5]._u32[0], n = c->gpr[7]._u32[0];
    const long P = (int16_t)(c->gpr[6]._u32[0] & 0xFFFF);
    for (uint32_t px = 0; px < n; px++) {
        const int b0 = ls[(vel + 4 * px) & 0x3FFFF], b1 = ls[(vel + 4 * px + 1) & 0x3FFFF], b2 = ls[(vel + 4 * px + 2) & 0x3FFFF];
        const long vx = 16 * b1 - 2048, vy = b2 - 128;
        /* tap k: side s (0 minus, 1 plus), ring c; T[k] in the spec's order k = 6 s + ring */
        float T[12][4];
        for (int k = 0; k < 12; k++) {
            const long sg = k < 6 ? -1 : 1;
            const long col = (long)px + fdiv(sg * vx * C[k % 6], 65536L * 16), row = fdiv(sg * vy * C[k % 6], 65536L);
            const u128 q = ldq(ls, (uint32_t)((long)src + 16 * col + row * P));
            for (int ch = 0; ch < 4; ch++) memcpy(&T[k][ch], &q._u32[ch], 4);
        }
        float S4[4];
        for (int ch = 0; ch < 4; ch++) {
            u128 s = spu_splat_u32(*(uint32_t*)&T[11][ch]);
            u128 h = spu_splat_u32(0x3F000000u);
#define TAP(k) spu_splat_u32(*(uint32_t*)&T[k][ch])
            for (int k = 10; k >= 7; k--) s = spu_fa(s, TAP(k));
            s = spu_fma(TAP(6), h, s);
            for (int k = 5; k >= 1; k--) s = spu_fa(s, TAP(k));
            s = spu_fma(TAP(0), h, s);
#undef TAP
            memcpy(&S4[ch], &s._u32[0], 4);
        }
        uint32_t s0b; memcpy(&s0b, &S4[0], 4);
        const u128 s0 = spu_splat_u32(s0b), rec = spu_fi(s0, spu_frest(s0));
        const u128 sc = spu_cuflt(spu_splat_u32((uint32_t)b0), 147);
        for (int ch = 0; ch < 4; ch++) {
            uint32_t sb; memcpy(&sb, &S4[ch], 4);
            u128 t = spu_fm(spu_splat_u32(sb), rec);
            t = spu_fm(t, sc);
            ls[(dst + 4 * px + ch) & 0x3FFFF] = (uint8_t)spu_cfltu(t, 165)._u32[0];
        }
    }
}
int main(void)
{
    kz3_native_fn fn = 0;
    for (unsigned i = 0; i < kz3_native_post_n; i++) if (kz3_native_post[i].slot == 22) fn = kz3_native_post[i].fn;
    if (!fn) { printf("no slot 22 entry\n"); return 1; }
    int bad = 0, n_ok = 0, n_refused = 0, n_zero_vec = 0;
    static const uint32_t wps[] = {40, 128, 256, 64, 20, 176};
    static const uint8_t corner[4] = {0, 127, 128, 255};
    for (int t = 0; t < 3000; t++) {
        memset(&O, 0, sizeof O);
        O.ls = O.ls_store;
        for (uint32_t i = 0; i < 0x40000; i++) O.ls[i] = (uint8_t)rnd();
        for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) O.gpr[r]._u32[k] = rnd();
        const uint32_t Wp = wps[t % 6], n = t % 9 == 0 ? 4 : 4 * (1 + rnd() % ((Wp - 12) / 4));
        const uint32_t scratch = 0x10000, src = scratch + 16 * (6 * Wp + 6), vel = 0x1F800, dst = 0x1FC00;
        if (t % 3) /* realistic floats; else raw random bits */
            for (uint32_t i = 0; i < 14 * Wp; i++)
                for (int k = 0; k < 4; k++) {
                    float f = (float)(rnd() % 170001) / 10000.0f;
                    if (k == 0 && t % 11 == 0 && rnd() % 8 == 0) f = 0.0f;
                    uint32_t v; memcpy(&v, &f, 4); st32(O.ls, scratch + 16 * i + 4 * k, v);
                }
        for (uint32_t i = 0; i < n + 4; i++) {
            const int mode = t % 6;
            uint8_t b[4] = {(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
            if (mode == 0) { b[1] = b[2] = 128; if (i < n) n_zero_vec++; }
            else if (mode == 1) { for (int k = 0; k < 3; k++) b[k] = (rnd() & 1) ? 0 : 255; }
            else if (mode == 3) b[0] = 0;
            else if (mode == 4) { b[1] = corner[rnd() % 4]; b[2] = corner[rnd() % 4]; }
            memcpy(O.ls + vel + 4 * i, b, 4);
        }
        uint32_t pitch = 16 * Wp;
        if (t % 4 == 3) pitch |= rnd() << 16;
        uint32_t cn = n, cd = dst, cs = src;
        const int mode = t % 13;   /* 0: n not a multiple of 4, 1: zero, 2: unaligned, 3: dst under the tap window, 4: past the LS */
        if (mode == 0) cn = n + 1 + rnd() % 3;
        if (mode == 1) cn = 0;
        if (mode == 2) cs = src + 4;
        if (mode == 3) cd = src + 16 * n + 32;
        if (mode == 4) cd = SPU_LS_SIZE - 4 * n + 16;   /* the last 16 bytes would fall past the local store */
        O.gpr[3] = spu_splat_u32(cd); O.gpr[4] = spu_splat_u32(cs); O.gpr[5] = spu_splat_u32(vel);
        O.gpr[6] = spu_splat_u32(pitch); O.gpr[7] = spu_splat_u32(cn);
        B = O; B.ls = B.ls_store;
        const int took = fn(&B);
        if (mode <= 4) {
            if (took || memcmp(B.ls_store, O.ls_store, sizeof O.ls_store) || memcmp(B.gpr, O.gpr, sizeof O.gpr)) { bad++; printf("refusal mode %d failed\n", mode); }
            n_refused++;
            continue;
        }
        if (!took) { bad++; printf("declined t=%d\n", t); continue; }
        A = O; A.ls = A.ls_store;
        model(&A);
        if (memcmp(A.ls_store, B.ls_store, sizeof A.ls_store) || memcmp(A.gpr, B.gpr, sizeof A.gpr)) {
            long off = -1;
            for (long i = 0; i < (long)sizeof A.ls_store; i++) if (A.ls_store[i] != B.ls_store[i]) { off = i; break; }
            if (bad < 5) printf("differs t=%d Wp=%u n=%u ls_off=0x%lX\n", t, Wp, n, off);
            bad++;
        }
        n_ok++;
    }
    printf("mb_blur: %d equal-checked, %d refusals, %d bad, %d zero-vector pixels\n", n_ok, n_refused, bad, n_zero_vec);
    return bad != 0;
}
C
SRC=("$ROOT/port/spu/kz3_native_post.c" "$ROOT/tools/spu/native_xr_stubs.c")
INC=(-I"$ROOT/port/spu" -I"$T/runtime/spu" -I"$T/include" -I"$T/runtime")
fail=0
cc -std=gnu17 -O1 -msse4.1 -w -fno-strict-aliasing -ffp-contract=off "${INC[@]}" "$OUT/t.c" "${SRC[@]}" -lm -o "$OUT/t" || { echo "FAIL build"; echo "RESULT: FAIL"; exit 1; }
"$OUT/t" && echo "PASS model" || { echo "FAIL model"; fail=1; }
cc -std=gnu17 -O1 -msse4.1 -w -fno-strict-aliasing -ffp-contract=off -DKZ3_NATIVE_NEGCTL "${INC[@]}" "$OUT/t.c" "${SRC[@]}" -lm -o "$OUT/tn" || { echo "FAIL build (negative control)"; echo "RESULT: FAIL"; exit 1; }
if "$OUT/tn" > "$OUT/neg.log" 2>&1; then echo "FAIL negative control not caught"; fail=1; else echo "PASS negative control caught ($(tail -1 "$OUT/neg.log"))"; fi
# Part 2: the lifted SPU code (game data only)
if [[ -f $ROOT/work/spu/spu_lift.json && -f $ROOT/work/spu/lift/job_011f7d00/spu_recomp.c ]]; then
    out=$(cd "$ROOT" && python3 tools/spu/native_diff.py --slot 22 --trials 2000 2>&1 | tail -3)
    echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot 22: $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot 22: $out"; fail=1; }
    out=$(cd "$ROOT" && NATIVE_DIFF_CFLAGS=-DKZ3_NATIVE_NEGCTL python3 tools/spu/native_diff.py --slot 22 --trials 2000 2>&1 | tail -2)
    echo "$out" | grep -q '^FAIL' && echo "PASS native_diff negative control FAILs as it must ($(echo "$out" | grep -o 'equal=[0-9]* differ=[0-9]*'))" || { echo "FAIL native_diff negative control passed: $out"; fail=1; }
    if [[ -f $ROOT/work/spu-100/spu_lift.json && -f $ROOT/work/spu-100/lift/job_0117b900/spu_recomp.c ]]; then
        out=$(cd "$ROOT" && KZ3_EBOOT_VER=100 python3 tools/spu/native_diff.py --slot 23 --trials 2000 --ps3recomp "$T" 2>&1 | tail -3)
        echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot 23 (1.00 twin): $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot 23: $out"; fail=1; }
    else echo "SKIP native_diff slot 23: no 1.00 lift"; fi
else echo "SKIP native_diff (no game data / lift)"; fi
[[ $fail == 0 ]] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; exit 1; }
