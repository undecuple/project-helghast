/* Patch 1038 host test (KZ3_SPU_FASTCG paths against the old ones, g_kz3_spu_slowcg = 1):
 * 1. the pshufb quadword byte shifts/rotates (spu__byteperm) for every shift count, out-of-range counts and
 *    random data. Negative control: an index without the SPU_W word swap (t instead of t ^ 3) must be caught.
 * 2. the four-lane fma/fms/fnms (spu__fma4) on random lanes, exact-zero sums, rounding midpoints and the float
 *    subnormal/overflow ranges, against the old path and glibc fmaf per lane. Negative control: plain float
 *    a*b+c (two roundings) must be caught.
 * 3. (patch 1040, pass 28) flush-to-zero of fma results (g_kz3_spu_slowcg 0 = fast with FTZ, 2 = fast without): the fast path against glibc fmaf with
 *    its subnormal results flushed, on tiny-range lanes, the double-rounding edge vector, and a decaying IIR tail
 *    (y = fma(y, 0.99, 0)), which must reach 0 without libm calls. Negative controls: FTZ off (subnormals kept)
 *    and a flush without the edge exclusion must be caught. The fallback-reason counter (kz3_spu_fma_note) is
 *    checked on one lane of each reason.
 * 4. (patch 1041, pass 28) shufb in host byte order against the byte-loop reference (patch 1082, pass 65: and the
 *    one-source spu_shufb1(a, c) against the reference with b = a)
 *    spu_shufb_ref on random controls (all four selector kinds); negative control: the same without the ^3
 *    word swap must be caught. Also times both shufb versions (a dependent chain, ns per op).
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_fastcg.sh */
#include <math.h>
static long n_fmaf_calls; /* the helpers' libm fallback, counted: the zero-sum case must not reach it */
static float counted_fmaf(float a, float b, float c)
{
    n_fmaf_calls++;
    return __builtin_fmaf(a, b, c);
}
#define fmaf counted_fmaf
#include "spu_context.h"
#include "spu_helpers.h"
#undef fmaf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_kz3_spu_xr = 0; /* patch 1071 off: this test checks the host (IEEE) results */
int g_kz3_spu_slowcg;
int g_kz3_fma_stats;   /* parts 1-2 run the fast paths without FTZ (g_kz3_spu_slowcg = 2): they check IEEE results */
int g_kz3_xr_stats;    /* patch 1088 probe off; stub keeps the link self-contained */
int g_kz3_spu_xrq = 1; /* patch 1089: the inline exponent-255 lanes of fa/fs/fm (the runtime default) */
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra)
{
    (void)op;
    (void)m;
    (void)a;
    (void)b;
    (void)c;
    (void)r;
    (void)ra;
}
static long fb_reason[5];
void kz3_spu_fma_note(const double s[4], int ftz)
{ /* the runtime's classifier (spu_drain.c), copied: [0] NaN/inf [1] overflow [2] subnormal [3] midpoint */
    (void)ftz;
    for (int i = 0; i < 4; i++) {
        const double a = __builtin_fabs(s[i]);
        int k;
        if (a != a || a == __builtin_inf())
            k = 0;
        else if (a >= 0x1p128)
            k = 1;
        else if (a == 0)
            continue;
        else if (a < 0x1p-126)
            k = 2;
        else {
            uint64_t u;
            memcpy(&u, &s[i], 8);
            if ((u & 0x1FFFFFFFull) != 0x10000000ull) continue;
            k = 3;
        }
        fb_reason[k]++;
    }
    fb_reason[4]++;
}
static u128 sf(float f)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._f32[i] = f;
    return r;
}
static float flushf(float r)
{
    if (r != 0.0f && fabsf(r) < 0x1p-126f) {
        uint32_t u;
        memcpy(&u, &r, 4);
        u &= 0x80000000u;
        memcpy(&r, &u, 4);
    }
    return r;
}
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)rs;
}
static u128 rq(void)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = rnd();
    return r;
}

/* wrong on purpose: host index t (no ^3) */
static u128 bad_rotqbyi(u128 a, int sh)
{
    const __m128i iv = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i t = _mm_and_si128(_mm_add_epi8(iv, _mm_set1_epi8((char)(sh & 15))), _mm_set1_epi8(15));
    __m128i A;
    memcpy(&A, &a, 16);
    A = _mm_shuffle_epi8(A, t);
    u128 r;
    memcpy(&r, &A, 16);
    return r;
}

typedef u128 (*fi_t)(u128, int);
typedef u128 (*fq_t)(u128, u128);
/* non-inline wrappers so both paths run the same helper */
#define WI(n)                                                                                                          \
    static u128 w_##n(u128 a, int s)                                                                                   \
    {                                                                                                                  \
        return spu_##n(a, s);                                                                                          \
    }
#define WQ(n)                                                                                                          \
    static u128 w_##n(u128 a, u128 b)                                                                                  \
    {                                                                                                                  \
        return spu_##n(a, b);                                                                                          \
    }
WI(shlqbyi)
WI(rotqbyi)
WI(rotqmbyi)
WQ(shlqby) WQ(rotqby) WQ(shlqbybi) WQ(rotqbybi) WQ(rotqmby) WQ(rotqmbybi)

    int main(void)
{
    struct {
        const char* n;
        fi_t f;
    } fi[] = {{"shlqbyi", w_shlqbyi}, {"rotqbyi", w_rotqbyi}, {"rotqmbyi", w_rotqmbyi}};
    struct {
        const char* n;
        fq_t f;
    } fq[] = {{"shlqby", w_shlqby},     {"rotqby", w_rotqby},   {"shlqbybi", w_shlqbybi},
              {"rotqbybi", w_rotqbybi}, {"rotqmby", w_rotqmby}, {"rotqmbybi", w_rotqmbybi}};
    long bad = 0, n = 0;
    for (int it = 0; it < 20000; it++) {
        u128 a = rq();
        for (int k = 0; k < 3; k++)
            for (int sh = -64; sh < 64; sh++) { /* immediates: the helpers mask them */
                g_kz3_spu_slowcg = 1;
                u128 x = fi[k].f(a, sh);
                g_kz3_spu_slowcg = 0;
                u128 y = fi[k].f(a, sh);
                n++;
                if (memcmp(&x, &y, 16)) {
                    if (bad++ < 5) printf("FAIL %s sh=%d\n", fi[k].n, sh);
                }
            }
        for (int k = 0; k < 6; k++)
            for (int j = 0; j < 160; j++) {
                u128 b = rq();
                if (j < 128)
                    b._u32[0] = (j < 64) ? (uint32_t)j * (k == 2 || k == 3 || k == 5 ? 8u : 1u)
                                         : (uint32_t)(-(j - 64)) * (k == 5 ? 8u : 1u);
                g_kz3_spu_slowcg = 1;
                u128 x = fq[k].f(a, b);
                g_kz3_spu_slowcg = 0;
                u128 y = fq[k].f(a, b);
                n++;
                if (memcmp(&x, &y, 16)) {
                    if (bad++ < 5) printf("FAIL %s b=%08X\n", fq[k].n, b._u32[0]);
                }
            }
    }
    printf("byteperm: %ld cases, %ld mismatches\n", n, bad);
    long caught = 0, m = 0;
    for (int it = 0; it < 20000; it++) {
        u128 a = rq();
        for (int sh = 0; sh < 16; sh++) {
            g_kz3_spu_slowcg = 1;
            u128 x = w_rotqbyi(a, sh);
            u128 y = bad_rotqbyi(a, sh);
            m++;
            caught += memcmp(&x, &y, 16) != 0;
        }
    }
    printf("negative control (no SPU_W swap): %ld of %ld caught\n", caught, m);
    /* 2. fma */
    long fb = 0, fn = 0, fref = 0, fneg = 0;
    for (int it = 0; it < 4000000; it++) {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) {
            uint32_t k = rnd() % 8, x = rnd(), y = rnd(), w = rnd();
            float fa_, fb_, fc_;
            if (k < 3) { /* random normal-ish */
                x = (x & 0x807FFFFF) | ((100 + rnd() % 56) << 23);
                y = (y & 0x807FFFFF) | ((100 + rnd() % 56) << 23);
                w = (w & 0x807FFFFF) | ((90 + rnd() % 76) << 23);
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                memcpy(&fc_, &w, 4);
            } else if (k == 3) { /* exact zero sum: c = -(a*b) with a*b exact in float */
                x = (x & 0x80700000) | (127u << 23);
                y = (y & 0x80700000) | (120u << 23);
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                fc_ = -(fa_ * fb_);
                if (rnd() & 1) {
                    fa_ = 0.0f;
                    fc_ = (rnd() & 1) ? -0.0f : 0.0f;
                }
            } else if (k == 4) { /* rounding midpoint: a*b exact, c places a 1 just below the float lsb */
                x = (x & 0x807FF000) | (127u << 23);
                y = (127u << 23) | (rnd() & 0xFF);
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                fc_ = ldexpf((rnd() & 1) ? 1.0f : -1.0f, -24 - (int)(rnd() % 3));
            } else if (k == 5) { /* subnormal / tiny results */
                x = (x & 0x807FFFFF) | ((1 + rnd() % 60) << 23);
                y = (y & 0x807FFFFF) | ((1 + rnd() % 60) << 23);
                w = (w & 0x807FFFFF) | ((rnd() % 4) << 23);
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                memcpy(&fc_, &w, 4);
            } else if (k == 6) { /* overflow range */
                x = (x & 0x807FFFFF) | ((190 + rnd() % 64) << 23);
                y = (y & 0x807FFFFF) | ((190 + rnd() % 64) << 23);
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                memcpy(&fc_, &w, 4);
            } else {
                memcpy(&fa_, &x, 4);
                memcpy(&fb_, &y, 4);
                memcpy(&fc_, &w, 4);
            } /* any bits incl. NaN/inf */
            a._f32[l] = fa_;
            b._f32[l] = fb_;
            c._f32[l] = fc_;
        }
        for (int op = 0; op < 3; op++) {
            u128 x, y;
            g_kz3_spu_slowcg = 1;
            x = op == 0 ? spu_fma(a, b, c) : op == 1 ? spu_fms(a, b, c) : spu_fnms(a, b, c);
            g_kz3_spu_slowcg = 2;
            y = op == 0 ? spu_fma(a, b, c) : op == 1 ? spu_fms(a, b, c) : spu_fnms(a, b, c);
            for (int l = 0; l < 4; l++) {
                float rf = fmaf(a._f32[l], op == 2 ? -b._f32[l] : b._f32[l], op == 1 ? -c._f32[l] : c._f32[l]);
                float pl = a._f32[l] * (op == 2 ? -b._f32[l] : b._f32[l]) + (op == 1 ? -c._f32[l] : c._f32[l]);
                int nn = y._f32[l] != y._f32[l];
                fn++;
                if (memcmp(&x._u32[l], &y._u32[l], 4) && !(nn && x._f32[l] != x._f32[l])) {
                    if (fb++ < 5)
                        printf("FAIL fma op%d %a %a %a: %a vs %a\n", op, a._f32[l], b._f32[l], c._f32[l], x._f32[l],
                               y._f32[l]);
                }
                if (memcmp(&rf, &y._u32[l], 4) && !(nn && rf != rf)) fref++;
                if (memcmp(&pl, &y._u32[l], 4) && !(nn && pl != pl)) fneg++;
            }
        }
    }
    printf("fma4: %ld lanes, %ld mismatches vs old path, %ld vs glibc fmaf\n", fn, fb, fref);
    printf("negative control (float a*b+c, two roundings): %ld lanes differ\n", fneg);
    /* exact-zero sums in all four lanes: the new path must not call fmaf, the old one does (control) */
    long z_new, z_old;
    {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) {
            a._f32[l] = 1.5f + l;
            b._f32[l] = (l & 1) ? -0.25f : 2.0f;
            c._f32[l] = -(a._f32[l] * b._f32[l]);
        }
        n_fmaf_calls = 0;
        g_kz3_spu_slowcg = 2;
        volatile float sink = spu_fma(a, b, c)._f32[0];
        z_new = n_fmaf_calls;
        n_fmaf_calls = 0;
        g_kz3_spu_slowcg = 1;
        sink = spu_fma(a, b, c)._f32[0];
        z_old = n_fmaf_calls;
        (void)sink;
    }
    printf("zero-sum fmaf calls: new path %ld, old path %ld (control)\n", z_new, z_old);
    /* 3. flush-to-zero (patch 1040) */
    long t_n = 0, t_bad = 0, t_off = 0, t_calls;
    g_kz3_spu_slowcg = 2;
    n_fmaf_calls = 0;
    for (int it = 0; it < 2000000; it++) {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) { /* tiny products and addends, incl. subnormal inputs and cancellation */
            uint32_t x = (rnd() & 0x807FFFFF) | ((1 + rnd() % 70) << 23),
                     y = (rnd() & 0x807FFFFF) | ((50 + rnd() % 60) << 23),
                     w = (rnd() & 0x807FFFFF) | ((rnd() % 6) << 23);
            memcpy(&a._f32[l], &x, 4);
            memcpy(&b._f32[l], &y, 4);
            memcpy(&c._f32[l], &w, 4);
        }
        for (int op = 0; op < 3; op++) {
            g_kz3_spu_slowcg = 0;
            u128 y = op == 0 ? spu_fma(a, b, c) : op == 1 ? spu_fms(a, b, c) : spu_fnms(a, b, c);
            g_kz3_spu_slowcg = 2;
            u128 o = op == 0 ? spu_fma(a, b, c) : op == 1 ? spu_fms(a, b, c) : spu_fnms(a, b, c);
            for (int l = 0; l < 4; l++) {
                float rf = flushf(fmaf(a._f32[l], op == 2 ? -b._f32[l] : b._f32[l], op == 1 ? -c._f32[l] : c._f32[l]));
                t_n++;
                if (memcmp(&rf, &y._u32[l], 4) && t_bad++ < 5)
                    printf("FAIL ftz op%d %a %a %a: %a vs %a\n", op, a._f32[l], b._f32[l], c._f32[l], y._f32[l], rf);
                if (memcmp(&rf, &o._u32[l], 4)) t_off++;
            }
        }
    }
    printf("ftz: %ld tiny-range lanes, %ld mismatches vs flushed glibc fmaf; FTZ off differs on %ld (control)\n", t_n,
           t_bad, t_off);
    /* the edge: a*b = -(2^-150 + 4688 * 2^-196), c = FLT_MIN; the exact sum lies just below the midpoint between
     * the largest subnormal and FLT_MIN, so the true result is subnormal (flushed: +0), while the double sum rounds
     * onto the midpoint and then to FLT_MIN. */
    float ea = ldexpf((float)(8388608 + 2896), -98), eb = -ldexpf((float)(8388608 - 2895), -98), ec = 0x1p-126f;
    u128 A4 = sf(ea), B4 = sf(eb), C4 = sf(ec);
    g_kz3_spu_slowcg = 0;
    float e_new = spu_fma(A4, B4, C4)._f32[0], e_ref = flushf(fmaf(ea, eb, ec));
    double es = (double)ea * (double)eb + (double)ec;
    float e_noedge = flushf((float)es); /* negative control: flush after the double sum, no edge exclusion */
    printf("edge: new %a, flushed fmaf %a, flush without the edge test %a (control)\n", e_new, e_ref, e_noedge);
    /* decaying IIR tail: y = 0.99 * y from 1e-30, 4 lanes */
    long tail_calls[2], tail_zero[2];
    for (int f = 0; f < 2; f++) {
        g_kz3_spu_slowcg = f ? 0 : 2;
        u128 yv = sf(1e-30f), k = sf(0.99f), z = sf(0.0f);
        n_fmaf_calls = 0;
        tail_zero[f] = -1;
        for (int i = 0; i < 20000; i++) {
            yv = spu_fma(yv, k, z);
            if (tail_zero[f] < 0 && yv._f32[0] == 0.0f) tail_zero[f] = i;
        }
        tail_calls[f] = n_fmaf_calls;
    }
    printf(
        "IIR tail 20000 steps: FTZ off: fmaf calls %ld, zero at step %ld; FTZ on: fmaf calls %ld, zero at step %ld\n",
        tail_calls[0], tail_zero[0], tail_calls[1], tail_zero[1]);
    /* fallback reasons: one lane each */
    g_kz3_spu_slowcg = 2;
    g_kz3_fma_stats = 1;
    {
        u128 a = sf(1.0f), b = sf(1.0f), c = sf(0.0f);
        a._f32[0] = __builtin_inff();
        a._f32[1] = 0x1p100f, b._f32[1] = 0x1p100f;
        a._f32[2] = 0x1p-100f, b._f32[2] = 0x1p-40f;
        a._f32[3] = 1.0f + 0x1p-23f, c._f32[3] = 0x1p-24f; /* 1 + 2^-23 + 2^-24: a float midpoint */
        (void)spu_fma(a, b, c);
    }
    g_kz3_fma_stats = 0;
    printf("fallback reasons: naninf %ld overflow %ld subnormal %ld midpoint %ld vectors %ld\n", fb_reason[0],
           fb_reason[1], fb_reason[2], fb_reason[3], fb_reason[4]);
    int ok3 = t_bad == 0 && t_off > 1000 && e_new == 0.0f && e_ref == 0.0f && e_noedge == 0x1p-126f &&
              tail_calls[1] == 0 && tail_zero[1] >= 0 && tail_zero[0] < 0 &&
#if !defined(__FMA__) /* -mfma (FASTCG_ISA, patch 1097): the fallback is the hardware fmaf, not counted */
              tail_calls[0] > 1000 &&
#endif
             
              fb_reason[0] == 1 && fb_reason[1] == 1 && fb_reason[2] == 1 && fb_reason[3] == 1 && fb_reason[4] == 1;
    printf("ftz part %s\n", ok3 ? "PASS" : "FAIL");
    (void)t_calls;
    /* 4. shufb (patch 1041) */
    long s_n = 0, s_bad = 0, s_neg = 0;
    for (int it = 0; it < 2000000; it++) {
        u128 a = rq(), b = rq(), c = rq();
        if (it & 1) /* mostly data selectors */
            for (int i = 0; i < 4; i++) c._u32[i] &= (rnd() & 1) ? 0x1F1F1F1Fu : 0x7F7F7F7Fu;
        u128 x = spu_shufb(a, b, c);
        u128 y = spu_shufb_ref(a, b, c);
        s_n++;
        if (memcmp(&x, &y, 16) && s_bad++ < 5) printf("FAIL shufb it %d\n", it);
        /* patch 1082: the one-source form against the reference with b = a */
        const u128 x1 = spu_shufb1(a, c), y1 = spu_shufb_ref(a, a, c);
        if (memcmp(&x1, &y1, 16) && s_bad++ < 5) printf("FAIL shufb1 it %d\n", it);
        /* negative control: index without the word swap */
        __m128i A, B, C;
        memcpy(&A, &a, 16);
        memcpy(&B, &b, 16);
        memcpy(&C, &c, 16);
        const __m128i t = _mm_and_si128(C, _mm_set1_epi8(0x1F));
        __m128i r = _mm_or_si128(_mm_shuffle_epi8(A, _mm_add_epi8(t, _mm_set1_epi8(0x70))),
                                 _mm_shuffle_epi8(B, _mm_add_epi8(t, _mm_set1_epi8((char)0xF0))));
        const __m128i lut =
            _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
        r = _mm_blendv_epi8(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F))), C);
        if (memcmp(&r, &y, 16)) s_neg++;
    }
    printf("shufb: %ld cases, %ld mismatches vs byte-loop reference; negative control (no ^3) caught %ld\n", s_n, s_bad,
           s_neg);
    /* timing: separate loops per version (a select inside one loop let GCC compute both) */
    double sh_lat[2], sh_thr[2];
#define SHUFB_TIME(F, OUT_LAT, OUT_THR)                                                                                \
    do {                                                                                                               \
        u128 a = rq(), b = rq(), c = rq(), x[8];                                                                       \
        for (int k = 0; k < 8; k++) x[k] = rq();                                                                       \
        struct timespec t0, t1, t2;                                                                                    \
        clock_gettime(CLOCK_MONOTONIC, &t0);                                                                           \
        for (int i = 0; i < 20000000; i++) {                                                                           \
            a = F(a, b, c);                                                                                            \
            __asm__ volatile("" : "+x"(a));                                                                            \
        }                                                                                                              \
        clock_gettime(CLOCK_MONOTONIC, &t1);                                                                           \
        for (int i = 0; i < 5000000; i++)                                                                              \
            for (int k = 0; k < 8; k++) {                                                                              \
                x[k] = F(x[k], b, c);                                                                                  \
                __asm__ volatile("" : "+x"(x[k]));                                                                     \
            }                                                                                                          \
        clock_gettime(CLOCK_MONOTONIC, &t2);                                                                           \
        OUT_LAT = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 20000000.0;                            \
        OUT_THR = ((t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec)) / 40000000.0;                            \
    } while (0)
    for (int rep = 0; rep < 2; rep++) { /* second pass reported (warm) */
        SHUFB_TIME(spu_shufb_1035, sh_lat[0], sh_thr[0]);
        SHUFB_TIME(spu_shufb, sh_lat[1], sh_thr[1]);
    }
    printf("shufb latency chain: patch-1035 version %.3f ns/op, patch-1041 version %.3f ns/op\n", sh_lat[0], sh_lat[1]);
    printf("shufb throughput (8 chains): patch-1035 version %.3f ns/op, patch-1041 version %.3f ns/op\n", sh_thr[0],
           sh_thr[1]);
    int ok4 = s_bad == 0 && s_neg > s_n / 2;
    int ok =
        ok4 && ok3 && z_new == 0 && bad == 0 && caught > m / 2 && fb == 0 && fref == 0 && fneg > 1000;
#if !defined(__FMA__) /* the old path's libm calls (control): not counted with -mfma */
    ok = ok && z_old == 4;
#endif
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
