/* Pass 159: independent model of port/spu/kz3_native_snd.c (Snd.Mixer K7a gain ramp, slot 26; K3 resampler, slot 28),
 * written from the kernel descriptions with plain 2-D addressing (floor, per-element ramps recomputed by repeated
 * accumulation, one output at a time) and the runtime's SPU helpers on splats for every float op (the reference
 * semantics). Synthetic constant tables (random taps, the 0..15 gain table, {0,1,2,3}) are written into the local store
 * by the test: no game data. usage: t k7a|k3 [ftz] */
#include "kz3_native_jobs.h"
#include "spu_helpers.h"
#if __has_include("spu_fpmode.h")
#include "spu_fpmode.h"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static spu_context O, A, B;
static uint32_t rs = 159;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static uint32_t ld32(const uint8_t* ls, uint32_t a) { a &= 0x3FFFC; return (uint32_t)ls[a] << 24 | (uint32_t)ls[a + 1] << 16 | (uint32_t)ls[a + 2] << 8 | ls[a + 3]; }
static void st32(uint8_t* ls, uint32_t a, uint32_t v) { a &= 0x3FFFC; ls[a] = v >> 24; ls[a + 1] = v >> 16; ls[a + 2] = v >> 8; ls[a + 3] = v; }
static uint32_t bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static float flt(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static u128 S(uint32_t x) { return spu_splat_u32(x); }
static uint32_t F(u128 v) { return v._u32[0]; }
static uint32_t fm(uint32_t a, uint32_t b) { return F(spu_fm(S(a), S(b))); }
static uint32_t fa(uint32_t a, uint32_t b) { return F(spu_fa(S(a), S(b))); }
static uint32_t fs(uint32_t a, uint32_t b) { return F(spu_fs(S(a), S(b))); }
static uint32_t fma_(uint32_t a, uint32_t b, uint32_t c) { return F(spu_fma(S(a), S(b), S(c))); }
static float rf(void) { return (float)(rnd() & 0xFFFFFF) / 8388608.0f - 1.0f; }

static void model_k7a(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t src = c->gpr[3]._u32[0], dst = c->gpr[4]._u32[0], cnt = c->gpr[5]._u32[0], g0 = c->gpr[6]._u32[0], step = c->gpr[7]._u32[0];
    if (src == 0 || dst == 0 || (F(spu_fcmeq(S(g0), S(0))) && F(spu_fcmeq(S(0), S(step))))) return;
    const uint32_t step16 = fm(0x41800000, step);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t G = fma_(step, ld32(ls, 0x16B20 + 4 * (i % 16)), g0);   /* the table is 16 consecutive floats */
        for (uint32_t b = 0; b < i / 16; b++) G = fa(step16, G);
        const uint32_t s = ld32(ls, src + 4 * i), d = ld32(ls, dst + 4 * i);
        st32(ls, dst + 4 * i, fma_(s, G, d));
    }
}

static void model_k3(spu_context* c)
{
    uint8_t* ls = c->ls;
    const uint32_t step = c->gpr[3]._u32[0], ph0 = c->gpr[4]._u32[0], src = c->gpr[5]._u32[0], dst = c->gpr[6]._u32[0], n = c->gpr[7]._u32[0];
    const uint32_t step4 = fm(0x40800000, step);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = fma_(step, ld32(ls, 0x16740 + 4 * (i % 4)), ph0);
        for (uint32_t g = 0; g < i / 4; g++) p = fa(step4, p);
        const float pf = flt(p);
        const uint32_t fx = pf <= 0.0f ? 0u : (uint32_t)((double)pf * 4096.0), fl = pf <= 0.0f ? 0u : (uint32_t)floorf(pf);
        const uint32_t row = 0x15600 + 16 * ((fx & 4095) >> 4), w0 = src - 12 + 4 * fl;
        uint32_t m[4];
        for (int j = 0; j < 4; j++) m[j] = fm(ld32(ls, w0 + 4 * j), ld32(ls, row + 4 * j));
        st32(ls, dst + 4 * i, fa(fa(m[1], m[3]), fa(m[2], m[0])));
    }
    const uint32_t pend = fma_(step, F(spu_cuflt(S(n), 155)), ph0);
    c->gpr[3]._u32[0] = fs(pend, F(spu_cuflt(spu_cfltu(S(pend), 173), 155)));
}

int main(int argc, char** argv)
{
    const int k7 = argc > 1 && !strcmp(argv[1], "k7a");
    const int slot = k7 ? 26 : 28;
    kz3_native_fn fn = 0;
    for (unsigned i = 0; i < kz3_native_snd_n; i++) if (kz3_native_snd[i].slot == slot) fn = kz3_native_snd[i].fn;
    if (!fn) { printf("no slot %d entry\n", slot); return 1; }
#if __has_include("spu_fpmode.h")
    if (argc > 2 && !strcmp(argv[2], "ftz")) spu_fpmode_set_spu();
#endif
    int bad = 0, n_ok = 0, n_refused = 0, n_early = 0;
    const uint32_t lo = 0x8000;
    for (int t = 0; t < 12000; t++) {
        memset(&O, 0, sizeof O);
        O.ls = O.ls_store;
        for (uint32_t i = 0; i < 0x40000; i++) O.ls[i] = (uint8_t)rnd();
        for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) O.gpr[r]._u32[k] = rnd();
        for (int i = 0; i < 16; i++) st32(O.ls, 0x16B20 + 4 * i, bits((float)i));   /* synthetic constant tables */
        for (int i = 0; i < 4; i++) st32(O.ls, 0x16740 + 4 * i, bits((float)i));
        for (int i = 0; i < 1024; i++) st32(O.ls, 0x15600 + 4 * i, bits(rf() * 0.5f + (i % 4 == 1 ? 0.5f : 0.0f)));
        const int mode = t % 13, silent = t % 6 == 0, raw = t % 17 == 3;
        uint32_t o[5];
        if (k7) {
            const uint32_t cnt = 16 * (1 + rnd() % (t % 5 == 0 ? 2 : 40)), src = lo + (rnd() % 0x6000 & ~15u), dst = lo + 0x8000 + (rnd() % 0x6000 & ~15u);
            for (uint32_t i = 0; i < cnt + 32; i++) {
                if (!raw) {
                    st32(O.ls, src + 4 * i, silent ? ((rnd() & 3) ? 0 : 0x80000000u) : bits(rf()));
                    st32(O.ls, dst + 4 * i, silent && t % 12 == 0 ? 0 : bits(rf()));
                }
            }
            o[0] = mode == 7 ? dst : src; o[1] = dst; o[2] = cnt;
            o[3] = bits((float)(rnd() & 0xFFFF) / 65536.0f); o[4] = bits(((float)(rnd() & 0xFFFF) / 65536.0f - 0.5f) / 128.0f);
            if (mode == 7) o[0] = dst;                       /* in place */
            if (mode == 8) { o[3] = 0; o[4] = t % 2 ? 0x80000000u : 0; if (t % 3 == 0) o[3] = 0x80000000u; }   /* early return (g0 = step = +-0) */
            if (mode == 9) o[0] = 0;
            if (mode == 10) o[1] = 0;
            if (mode == 11) { o[3] = 0; }                    /* g0 = 0, step real */
        } else {
            const uint32_t n = 8 * (1 + rnd() % (t % 5 == 0 ? 2 : 48)), src = lo + 0x100 + (rnd() % 0x4000 & ~3u), dst = lo + 0x8000 + (rnd() % 0x4000 & ~15u);
            const float phase0 = (float)(rnd() & 0xFFFF) / 65536.0f * (t % 4 == 1 ? 64.0f : 1.0f), step = 0.05f + (float)(rnd() & 0xFFFF) / 65536.0f * (t % 3 == 0 ? 1.0f : 4.0f);
            for (uint32_t i = 0; i < 4 * n + 128 && !raw; i++) st32(O.ls, src - 16 + 4 * i, silent ? ((rnd() & 3) ? 0 : 0x80000000u) : bits(rf()));
            o[0] = bits(step); o[1] = bits(phase0); o[2] = src; o[3] = dst; o[4] = n;
            if (raw) { o[0] = bits(step); o[1] = bits(phase0); }
        }
        /* refusals: the kernel must return 0 and touch nothing */
        int refuse = 0;
        const uint32_t cn = o[k7 ? 2 : 4];
        if (k7) {
            if (mode == 0) { o[2] = 0; refuse = 1; }
            if (mode == 1) { o[2] = cn | 5; refuse = 1; }
            if (mode == 2) { o[0] += 4; refuse = 1; }
            if (mode == 3) { o[1] = o[0] + 16 * (1 + rnd() % 4); refuse = 1; }                 /* overlap */
            if (mode == 4) { o[1] = SPU_LS_SIZE - 16; o[2] = 64; refuse = 1; }                 /* past the LS */
            if (mode == 5) { o[1] = 0x16B00; refuse = 1; }                                     /* over the gain table */
            if (mode == 6) { o[2] = 0x20000; refuse = 1; }
        } else {
            if (mode == 0) { o[4] = 0; refuse = 1; }
            if (mode == 1) { o[4] = cn | 3; refuse = 1; }
            if (mode == 2) { o[3] += 4; refuse = 1; }
            if (mode == 3) { o[2] += 1; refuse = 1; }
            if (mode == 4) { o[2] = 8; refuse = 1; }
            if (mode == 5) { o[3] = 0x16000; refuse = 1; }                                     /* over the tap table */
            if (mode == 6) { o[3] = (o[2] & ~15u) - 16 + 16 * (rnd() % 2); o[1] = 0; refuse = 1; }                /* over the first windows */
            if (mode == 7) { o[1] = bits(1.0e6f); refuse = 1; }                                /* windows past the LS */
            if (mode == 8) { o[3] = SPU_LS_SIZE - 16; refuse = 1; }
        }
        for (int i = 0; i < 5; i++) O.gpr[3 + i]._u32[0] = o[i];
        B = O; B.ls = B.ls_store;
        const int took = fn(&B);
        if (refuse) {
            if (took || memcmp(B.ls_store, O.ls_store, sizeof O.ls_store) || memcmp(B.gpr, O.gpr, sizeof O.gpr)) { bad++; if (bad < 5) printf("refusal mode %d failed t=%d took=%d\n", mode, t, took); }
            n_refused++;
            continue;
        }
        if (!took) { bad++; if (bad < 5) printf("declined t=%d mode=%d\n", t, mode); continue; }
        A = O; A.ls = A.ls_store;
        if (k7) model_k7a(&A); else model_k3(&A);
        if (k7 && (mode == 8 || mode == 9 || mode == 10)) n_early++;
        int diff = memcmp(A.ls_store, B.ls_store, sizeof A.ls_store) != 0;
        if (k7) diff |= memcmp(A.gpr, B.gpr, sizeof A.gpr) != 0;
        else {   /* K3 returns r3 (all four words: the model gives word 0; the other words are the helper's on the original lanes) */
            for (int r = 0; r < 128; r++) if (r != 3 && memcmp(&A.gpr[r], &B.gpr[r], 16)) diff = 1;
            if (A.gpr[3]._u32[0] != B.gpr[3]._u32[0]) diff = 1;
        }
        if (diff) {
            long off = -1;
            for (long i = 0; i < (long)sizeof A.ls_store; i++) if (A.ls_store[i] != B.ls_store[i]) { off = i; break; }
            if (bad < 5) printf("differs t=%d mode=%d ls_off=0x%lX r3 %08X vs %08X\n", t, mode, off, A.gpr[3]._u32[0], B.gpr[3]._u32[0]);
            bad++;
        }
        n_ok++;
    }
    printf("%s: %d equal-checked (%d early returns), %d refusals, %d bad\n", k7 ? "k7a" : "k3", n_ok, n_early, n_refused, bad);
    return bad != 0;
}
