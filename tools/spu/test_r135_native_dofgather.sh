#!/usr/bin/env bash
# Pass 135: port/spu/kz3_native_dof.c dof_gather_51F0 (Gfx.Post.DepthOfField gather, job_011f9c80 @0x51F0, gate
# slots 20/21) against an independent scalar model written here from the RE notes s.2: the 36 disc
# taps come from the disc geometry (columns x = -3..3 in 1/512 pixel units, y ascending, the centre left out) instead
# of the kernel's tables, the offsets use floor division instead of the arithmetic shift, 2-D addressing instead of
# the SPU's byte offsets, phase 2 per channel on splats instead of quads; the float ops are the runtime's SPU helpers
# (the reference semantics). 2000 synthetic states (realistic floats or raw random bits, CoC all zero / all 255 /
# random / {0, 1, 37, 255} / zero groups, widths 4..244, pitch high-halfword garbage) must match bit for bit in the
# whole local store and all registers; refused arguments (n not a multiple of 4, zero, unaligned, dst over the row
# buffer, past the local store) must leave everything untouched. Negative control: the kernel built with
# -DKZ3_NATIVE_NEGCTL (ascending tap order) must be caught by the model. Part 2, only with game data: native_diff.py
# against the lifted SPU code (slot 20, the 1.00 twin 21 when its lift exists, the slot-6 K4 records derived into
# gather rows, and the negative control must FAIL). Self-contained otherwise (SKIP without the patched toolkit).
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
static uint32_t rs = 135;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t ld32(const uint8_t* ls, uint32_t a) { a &= 0x3FFFC; return (uint32_t)ls[a] << 24 | (uint32_t)ls[a + 1] << 16 | (uint32_t)ls[a + 2] << 8 | ls[a + 3]; }
static void st32(uint8_t* ls, uint32_t a, uint32_t v) { a &= 0x3FFFC; ls[a] = v >> 24; ls[a + 1] = v >> 16; ls[a + 2] = v >> 8; ls[a + 3] = v; }
static u128 ldq(const uint8_t* ls, uint32_t a) { u128 r; a &= 0x3FFF0; for (int i = 0; i < 4; i++) r._u32[i] = ld32(ls, a + 4 * i); return r; }
static void stq(uint8_t* ls, uint32_t a, u128 r) { a &= 0x3FFF0; for (int i = 0; i < 4; i++) st32(ls, a + 4 * i, r._u32[i]); }
static int fdiv16(int v) { return (int)floor((double)v / 65536.0); }
/* the model: every write the kernel must produce, from the plan's description alone */
static void model(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t dst = c->gpr[3]._u32[0], src = c->gpr[4]._u32[0], strm = c->gpr[5]._u32[0], rowb = c->gpr[6]._u32[0], n = c->gpr[8]._u32[0];
    const int P = (int16_t)(c->gpr[7]._u32[0] & 0xFFFF);
    int tx[36], ty[36], m = 0;
    for (int x = -3; x <= 3; x++) {
        const int r = x == 0 ? 3 : 4 - abs(x);
        for (int y = -r; y <= r; y++) {
            if (x == 0 && y == 0) continue;
            tx[m] = 512 * x; ty[m] = 512 * y; m++;
        }
    }
    for (uint32_t g = 0; g < n / 4; g++) {
        int c4[4], any = 0;
        for (int p = 0; p < 4; p++) { c4[p] = ls[(strm + 16 * g + 4 * p) & 0x3FFFF]; any |= c4[p]; }
        for (int p = 0; p < 4; p++) {
            const int px = 4 * (int)g + p;
            u128 acc = ldq(ls, src + 16 * px);
            if (!any) acc = spu_fma(acc, spu_splat_u32(0x24), acc);
            else
                for (int grp = 0; grp < 6; grp++)
                    for (int t = 5; t >= 0; t--) {
                        const int k = 6 * grp + t, dx = fdiv16(tx[k] * c4[p]), dy = fdiv16(ty[k] * c4[p]);
                        acc = spu_fa(acc, ldq(ls, (uint32_t)((int)src + 16 * (px + dx) + dy * P)));
                    }
            stq(ls, rowb + 16 * px, acc);
        }
    }
    for (uint32_t px = 0; px < n; px++) {
        const uint32_t cc = ls[(strm + 4 * px) & 0x3FFFF];
        u128 a = spu_fm(spu_fm(spu_cuflt(spu_splat_u32(cc), 147), spu_splat_u32(0x3F808081u)), spu_splat_u32(0x41800000u));
        if (spu_fcgt(a, spu_splat_u32(0x3F800000u))._u32[0]) a = spu_splat_u32(0x3F800000u);
        const u128 S = ldq(ls, rowb + 16 * px);
        const u128 s0 = spu_splat_u32(S._u32[0]), rec = spu_fi(s0, spu_frest(s0));
        for (int ch = 0; ch < 4; ch++) {
            u128 t = spu_fm(spu_splat_u32(S._u32[ch]), rec);
            t = spu_fm(t, a);
            t = spu_fm(t, spu_splat_u32(0x3F7F0000u));
            uint32_t u = spu_cfltu(t, 165)._u32[0];
            if ((int32_t)u > 255) u = 255;
            ls[(dst + 4 * px + ch) & 0x3FFFF] = (uint8_t)u;
        }
    }
}
int main(void)
{
    kz3_native_fn fn = 0;
    for (unsigned i = 0; i < kz3_native_dof_n; i++) if (kz3_native_dof[i].slot == 20) fn = kz3_native_dof[i].fn;
    if (!fn) { printf("no slot 20 entry\n"); return 1; }
    int bad = 0, n_ok = 0, n_refused = 0, n_skipgroups = 0;
    static const uint32_t wps[] = {40, 128, 256, 64, 20, 176};
    for (int t = 0; t < 2000; t++) {
        memset(&O, 0, sizeof O);
        O.ls = O.ls_store;
        for (uint32_t i = 0; i < 0x40000; i++) O.ls[i] = (uint8_t)rnd();
        for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) O.gpr[r]._u32[k] = rnd();
        const uint32_t Wp = wps[t % 6], n = t % 9 == 0 ? 4 : 4 * (1 + rnd() % ((Wp - 12) / 4));
        const uint32_t scratch = 0x10000, src = scratch + 16 * (6 * Wp + 6), rowb = 0x1E400, strm = 0x1F800, dst = 0x1FC00;
        if (t % 3) /* realistic floats; else raw random bits */
            for (uint32_t i = 0; i < 14 * Wp; i++)
                for (int k = 0; k < 4; k++) {
                    float f = k == 0 ? (float)(rnd() % 20001) / 10000.0f : (float)(rnd() % 80001) / 10000.0f;
                    if (k == 0 && t % 11 == 0 && rnd() % 8 == 0) f = 0.0f;
                    uint32_t v; memcpy(&v, &f, 4); st32(O.ls, scratch + 16 * i + 4 * k, v);
                }
        static const uint8_t few[4] = {0, 1, 37, 255};
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = (uint8_t)rnd();
            const int mode = t % 6;
            if (mode == 0) c = 0; else if (mode == 1) c = 255; else if (mode == 3) c = few[rnd() % 4];
            else if (mode == 4 && (i / 4) % 2 == 0) c = 0; else if (mode == 5 && (i / 4) % 3 == 0) c = 0;
            O.ls[strm + 4 * i] = c;
        }
        for (uint32_t g = 0; g < n / 4; g++)
            if (!(O.ls[strm + 16 * g] | O.ls[strm + 16 * g + 4] | O.ls[strm + 16 * g + 8] | O.ls[strm + 16 * g + 12])) n_skipgroups++;
        uint32_t pitch = 16 * Wp;
        if (t % 4 == 3) pitch |= rnd() << 16;
        uint32_t cn = n, cd = dst, cs = src;
        const int mode = t % 13;   /* 0: n not a multiple of 4, 1: zero, 2: unaligned, 3: dst over the row buffer, 4: past the LS */
        if (mode == 0) cn = n + 1 + rnd() % 3;
        if (mode == 1) cn = 0;
        if (mode == 2) cs = src + 4;
        if (mode == 3) cd = rowb + 16;
        if (mode == 4) cd = SPU_LS_SIZE - 4 * n + 16;   /* the last 16 bytes would fall past the local store */
        O.gpr[3] = spu_splat_u32(cd); O.gpr[4] = spu_splat_u32(cs); O.gpr[5] = spu_splat_u32(strm);
        O.gpr[6] = spu_splat_u32(rowb); O.gpr[7] = spu_splat_u32(pitch); O.gpr[8] = spu_splat_u32(cn);
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
    printf("dof_gather: %d equal-checked, %d refusals, %d bad, %d all-zero CoC groups\n", n_ok, n_refused, bad, n_skipgroups);
    return bad != 0;
}
C
SRC=("$ROOT/port/spu/kz3_native_dof.c" "$ROOT/tools/spu/native_xr_stubs.c")
INC=(-I"$ROOT/port/spu" -I"$T/runtime/spu" -I"$T/include" -I"$T/runtime")
fail=0
cc -std=gnu17 -O1 -msse4.1 -w -fno-strict-aliasing -ffp-contract=off "${INC[@]}" "$OUT/t.c" "${SRC[@]}" -lm -o "$OUT/t" || { echo "FAIL build"; echo "RESULT: FAIL"; exit 1; }
"$OUT/t" && echo "PASS model" || { echo "FAIL model"; fail=1; }
cc -std=gnu17 -O1 -msse4.1 -w -fno-strict-aliasing -ffp-contract=off -DKZ3_NATIVE_NEGCTL "${INC[@]}" "$OUT/t.c" "${SRC[@]}" -lm -o "$OUT/tn" || { echo "FAIL build (negative control)"; echo "RESULT: FAIL"; exit 1; }
if "$OUT/tn" > "$OUT/neg.log" 2>&1; then echo "FAIL negative control not caught"; fail=1; else echo "PASS negative control caught ($(tail -1 "$OUT/neg.log"))"; fi
# Part 2: the lifted SPU code (game data only)
if [[ -f $ROOT/work/spu/spu_lift.json && -f $ROOT/work/spu/lift/job_011f9c80/spu_recomp.c ]]; then
    REC=(); [[ -d $ROOT/work/spu/r122/rec ]] && REC=(--records "$ROOT/work/spu/r122/rec" --records-slot 6)
    out=$(cd "$ROOT" && python3 tools/spu/native_diff.py --slot 20 --trials 2000 "${REC[@]}" 2>&1 | tail -3)
    echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot 20: $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot 20: $out"; fail=1; }
    out=$(cd "$ROOT" && NATIVE_DIFF_CFLAGS=-DKZ3_NATIVE_NEGCTL python3 tools/spu/native_diff.py --slot 20 --trials 200 2>&1 | tail -2)
    echo "$out" | grep -q '^FAIL' && echo "PASS native_diff negative control FAILs as it must" || { echo "FAIL native_diff negative control passed: $out"; fail=1; }
    if [[ -f $ROOT/work/spu-100/spu_lift.json && -f $ROOT/work/spu-100/lift/job_0117d880/spu_recomp.c ]]; then
        out=$(cd "$ROOT" && KZ3_EBOOT_VER=100 python3 tools/spu/native_diff.py --slot 21 --trials 2000 --ps3recomp "$T" "${REC[@]}" 2>&1 | tail -3)
        echo "$out" | grep -q '^PASS' && echo "PASS native_diff slot 21 (1.00 twin): $(echo "$out" | grep -o 'equal=.*nr=[0-9]*')" || { echo "FAIL native_diff slot 21: $out"; fail=1; }
    else echo "SKIP native_diff slot 21: no 1.00 lift"; fi
else echo "SKIP native_diff (no game data / lift)"; fi
[[ $fail == 0 ]] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; exit 1; }
