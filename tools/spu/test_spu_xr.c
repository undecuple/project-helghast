/* Patch 1071 host test: SPU extended-range single precision on exponent-255 lanes (SPU ISA v1.2 s.9.1: no Inf/NaN,
 * exponent 255 is ordinary, overflow saturates to +-Smax = 0x7FFFFFFF, exponent 0 is zero, truncation).
 * Checks the ISA rules on such lanes, that every other lane is bit-identical with the patch off (g_kz3_spu_xr = 0),
 * and the round-56 case: the normalise idiom x * fi(x, frsqest(x)) with x = 0 gives 0, not NaN.
 * Patch 1079: the forced-inline ops (recompute out of line) are bit-identical to the 1071 forms (check_1079).
 * Built by tools/spu/test_spu_xr.sh against work/ps3recomp-patched (no game data). */
#include <math.h>
#include <stdint.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_kz3_spu_slowcg = 0; /* the default fast paths, with the 1040 flush-to-zero */
int g_kz3_fma_stats;
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
int g_kz3_spu_xr = 1;
void kz3_spu_fma_note(const double s[4], int ftz)
{
    (void)s;
    (void)ftz;
}

static int fails;
static u128 v1(uint32_t w)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = w;
    return r;
}
static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
#define CHECK(name, got, want)                                                                                         \
    do {                                                                                                               \
        uint32_t g_ = (got), w_ = (want);                                                                              \
        if (g_ != w_) {                                                                                                \
            fails++;                                                                                                   \
            printf("FAIL %s: got 0x%08X want 0x%08X\n", name, g_, w_);                                                 \
        }                                                                                                              \
    } while (0)

static uint32_t rnd(void)
{
    return (uint32_t)rand() << 16 ^ (uint32_t)rand();
}
/* a normal single in 1..254 exponent range with a modest spread, so sums/products stay normal */
static uint32_t rnorm(void)
{
    return (rnd() & 0x807FFFFFu) | ((uint32_t)(100 + rnd() % 56) << 23);
}

/* ---- patch 1079: the forced-inline ops against the 1071 forms (the bodies as they were before 1079) ---- */
#define REF2(name, sc, expr)                                                                                           \
    static u128 name(u128 a, u128 b)                                                                                   \
    {                                                                                                                  \
        u128 r;                                                                                                        \
        for (int i = 0; i < 4; i++) r._f32[i] = sc;                                                                    \
        SPU_XR_FIX2(r, a, b, expr);                                                                                    \
        return r;                                                                                                      \
    }
REF2(ref_fa, a._f32[i] + b._f32[i], spu__fa_lane(x, y))
REF2(ref_fs, a._f32[i] - b._f32[i], spu__fa_lane(x, y ^ 0x80000000u))
REF2(ref_fm, a._f32[i] * b._f32[i], spu__fm_lane(x, y))
#define REFC(name, sc, expr)                                                                                           \
    static u128 name(u128 a, u128 b)                                                                                   \
    {                                                                                                                  \
        u128 r;                                                                                                        \
        for (int i = 0; i < 4; i++) {                                                                                  \
            const float fa = a._f32[i], fb = b._f32[i];                                                                \
            (void)fa;                                                                                                  \
            (void)fb;                                                                                                  \
            r._u32[i] = (sc) ? 0xFFFFFFFFu : 0;                                                                        \
        }                                                                                                              \
        SPU_XR_FIXM(spu__xr_mask(a) | spu__xr_mask(b), r, a, b, expr);                                                 \
        return r;                                                                                                      \
    }
REFC(ref_fceq, fa == fb, spu__sf_key(x) == spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
REFC(ref_fcgt, fa > fb, spu__sf_key(x) > spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
REFC(ref_fcmeq, fabsf(fa) == fabsf(fb), spu__sf_mag(x) == spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
REFC(ref_fcmgt, fabsf(fa) > fabsf(fb), spu__sf_mag(x) > spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
static u128 ref_cflts(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(173 - i8));
    for (int i = 0; i < 4; i++) {
        double v = (double)a._f32[i] * f;
        if (v > 2147483647.0) v = 2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        r._s32[i] = (int32_t)v;
    }
    SPU_XR_FIXM(spu__xr_mask(a), r, a, a, spu__cfl_lane(x, 173 - (i8 & 0xFF), 0));
    return r;
}
static u128 ref_cfltu(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(173 - i8));
    for (int i = 0; i < 4; i++) {
        double v = (double)a._f32[i] * f;
        if (v < 0) v = 0;
        if (v > 4294967295.0) v = 4294967295.0;
        r._u32[i] = (uint32_t)v;
    }
    SPU_XR_FIXM(spu__xr_mask(a), r, a, a, spu__cfl_lane(x, 173 - (i8 & 0xFF), 1));
    return r;
}
static u128 ref_csflt(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(i8 - 155));
    for (int i = 0; i < 4; i++) r._f32[i] = (float)((double)a._s32[i] * f);
    SPU_XR_FIXM(spu__xr_mask(r), r, a, a,
                (int32_t)x
                    ? spu__sf_pack((int32_t)x < 0 ? 0x80000000u : 0, (int32_t)x < 0 ? 0u - x : x, -(155 - (i8 & 0xFF)))
                    : 0);
    return r;
}
static u128 ref_cuflt(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(i8 - 155));
    for (int i = 0; i < 4; i++) r._f32[i] = (float)((double)a._u32[i] * f);
    SPU_XR_FIXM(spu__xr_mask(r), r, a, a, x ? spu__sf_pack(0, x, -(155 - (i8 & 0xFF))) : 0);
    return r;
}
/* spu__fma4 as patches 1038/1040/1071 left it (one function, everything inline) */
static u128 ref_fma4(u128 a, u128 b, u128 c, int negb, int negc)
{
    const int cgm = g_kz3_spu_slowcg;
    if (cgm != 1) {
        __m128 A, B, C;
        memcpy(&A, &a, 16);
        memcpy(&B, &b, 16);
        memcpy(&C, &c, 16);
        const __m128 sgn = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
        if (negb) B = _mm_xor_ps(B, sgn);
        if (negc) C = _mm_xor_ps(C, sgn);
        const __m128d sl = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(A), _mm_cvtps_pd(B)), _mm_cvtps_pd(C));
        const __m128d sh = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(A, A)), _mm_cvtps_pd(_mm_movehl_ps(B, B))),
                                      _mm_cvtps_pd(_mm_movehl_ps(C, C)));
        const __m128d absm = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d lo = _mm_set1_pd(0x1p-126), hi = _mm_set1_pd(0x1p128), z = _mm_setzero_pd();
        const __m128i m29 = _mm_set1_epi64x(0x1FFFFFFFll), mid = _mm_set1_epi64x(0x10000000ll);
        const __m128d al = _mm_and_pd(sl, absm), ah = _mm_and_pd(sh, absm);
        __m128d okl =
            _mm_or_pd(_mm_andnot_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(_mm_and_si128(_mm_castpd_si128(sl), m29), mid)),
                                    _mm_and_pd(_mm_cmpge_pd(al, lo), _mm_cmplt_pd(al, hi))),
                      _mm_cmpeq_pd(sl, z));
        __m128d okh =
            _mm_or_pd(_mm_andnot_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(_mm_and_si128(_mm_castpd_si128(sh), m29), mid)),
                                    _mm_and_pd(_mm_cmpge_pd(ah, lo), _mm_cmplt_pd(ah, hi))),
                      _mm_cmpeq_pd(sh, z));
        if ((_mm_movemask_pd(okl) & _mm_movemask_pd(okh)) == 3) {
            const __m128 r4 = _mm_movelh_ps(_mm_cvtpd_ps(sl), _mm_cvtpd_ps(sh));
            u128 r;
            memcpy(&r, &r4, 16);
            return r;
        }
        if (cgm == 0) {
            const __m128d edge = _mm_set1_pd(0x1p-126 - 0x1p-150);
            okl = _mm_or_pd(okl, _mm_andnot_pd(_mm_cmpeq_pd(al, edge), _mm_cmplt_pd(al, lo)));
            okh = _mm_or_pd(okh, _mm_andnot_pd(_mm_cmpeq_pd(ah, edge), _mm_cmplt_pd(ah, lo)));
            if ((_mm_movemask_pd(okl) & _mm_movemask_pd(okh)) == 3) {
                __m128 r4 = _mm_movelh_ps(_mm_cvtpd_ps(sl), _mm_cvtpd_ps(sh));
                const __m128 fsgn = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
                const __m128 tiny = _mm_cmplt_ps(_mm_andnot_ps(fsgn, r4), _mm_set1_ps(0x1p-126f));
                r4 = _mm_blendv_ps(r4, _mm_and_ps(r4, fsgn), tiny);
                u128 r;
                memcpy(&r, &r4, 16);
                return r;
            }
        }
    }
    u128 r;
    for (int i = 0; i < 4; i++)
        r._f32[i] = spu__fmaf(a._f32[i], negb ? -b._f32[i] : b._f32[i], negc ? -c._f32[i] : c._f32[i]);
    SPU_XR_FIXM(spu__xr_mask(a) | spu__xr_mask(b) | spu__xr_mask(c) | spu__xr_mask(r), r, a, b,
                spu__fma_lane(x, y, c._u32[_i], negb ? 0x80000000u : 0u, negc ? 0x80000000u : 0u));
    if (cgm == 0)
        for (int i = 0; i < 4; i++)
            if (r._f32[i] != 0.0f && __builtin_fabsf(r._f32[i]) < 0x1p-126f) r._u32[i] &= 0x80000000u;
    return r;
}
/* a word drawn to hit every class: exponent 255, 0, 254, 1, small/large normals, any bits */
static uint32_t rword(void)
{
    const uint32_t r = rnd(), s = r & 0x80000000u;
    switch (rnd() % 10) {
    case 0:
        return s | 0x7F800000u | (rnd() & 0x7FFFFFu);
    case 1:
        return s | (rnd() & 0x7FFFFFu);
    case 2:
        return s | (254u << 23) | (rnd() & 0x7FFFFFu);
    case 3:
        return s | (1u << 23) | (rnd() & 0x7FFFFFu);
    case 4:
        return rnd() & 0x80000000u ? 0 : 0x80000000u;
    case 5:
        return rnd();
    default:
        return rnorm();
    }
}
/* 1079: every op bit-identical to its 1071 form, for xr on/off and the three fma modes */
static long check_1079(void)
{
    long bad = 0, n = 0, nan_payload = 0;
    srand(62);
    const int i8s[] = {173, 155, 160, 150, 128, 200, 0, 255};
    for (int it = 0; it < 400000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 4; i++) {
            a._u32[i] = rword();
            b._u32[i] = rword();
            c._u32[i] = rword();
        }
        if (it & 1) b = a; /* equal operands for the compares */
        const int i8 = i8s[it & 7];
        for (int xr = 0; xr < 2; xr++) {
            g_kz3_spu_xr = xr;
            for (int cg = 0; cg < 3; cg++) {
                g_kz3_spu_slowcg = cg;
                u128 got[15], want[15];
                got[0] = spu_fa(a, b), want[0] = ref_fa(a, b);
                got[1] = spu_fs(a, b), want[1] = ref_fs(a, b);
                got[2] = spu_fm(a, b), want[2] = ref_fm(a, b);
                got[3] = spu_fceq(a, b), want[3] = ref_fceq(a, b);
                got[4] = spu_fcgt(a, b), want[4] = ref_fcgt(a, b);
                got[5] = spu_fcmeq(a, b), want[5] = ref_fcmeq(a, b);
                got[6] = spu_fcmgt(a, b), want[6] = ref_fcmgt(a, b);
                got[7] = spu_cflts(a, i8), want[7] = ref_cflts(a, i8);
                got[8] = spu_cfltu(a, i8), want[8] = ref_cfltu(a, i8);
                got[9] = spu_csflt(a, i8), want[9] = ref_csflt(a, i8);
                got[10] = spu_cuflt(a, i8), want[10] = ref_cuflt(a, i8);
                got[11] = spu_fma(a, b, c), want[11] = ref_fma4(a, b, c, 0, 0);
                got[12] = spu_fms(a, b, c), want[12] = ref_fma4(a, b, c, 0, 1);
                got[13] = spu_fnms(a, b, c), want[13] = ref_fma4(a, b, c, 1, 0);
                got[14] = spu_fa(spu_fm(a, b), c), want[14] = ref_fa(ref_fm(a, b), c);
                for (int k = 0; k < 15; k++)
                    for (int i = 0; i < 4; i++, n++) {
                        const uint32_t g = got[k]._u32[i], w = want[k]._u32[i];
                        /* KZ3_SPU_XR=0 only: which NaN a host op returns from two NaN operands depends on the operand
                         * order the compiler picks for a commutative op, in the old form as in the new */
                        if (g != w && !xr && (g & 0x7FFFFFFFu) > 0x7F800000u && (w & 0x7FFFFFFFu) > 0x7F800000u) {
                            nan_payload++;
                            continue;
                        }
                        if (g != w && bad++ < 5)
                            printf("FAIL 1079 op %d lane %d xr %d cg %d i8 %d: 0x%08X vs 0x%08X\n", k, i, xr, cg, i8, g,
                                   w);
                    }
            }
        }
    }
    g_kz3_spu_xr = 1;
    g_kz3_spu_slowcg = 0;
    printf(
        "1079: %ld lanes compared with the 1071 forms, %ld differ (+ %ld host-NaN payload choices with KZ3_SPU_XR=0)\n",
        n, bad, nan_payload);
    return bad;
}

/* 1089: the inline exponent-255 lanes of fa/fs/fm (and the per-lane rest, the compares) against the 1071 forms, on words that reach them:
 * exponent 255 (the K4 words 0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFC00 and random), zeros, the smallest normal binade,
 * near-cancelling pairs (b = -a +- a few units), exponent gaps around 28/29, products and sums near 2^128, 2^129, 2^-126. */
static uint32_t rword89(uint32_t other)
{
    const uint32_t s = rnd() & 0x80000000u;
    static const uint32_t K[] = {0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FFFFC00u, 0x7FB50000u, 0x7FB17215u, 0x7F800000u,
                                 0xFF800000u, 0x00800000u, 0x80800000u, 0x00FFFFFFu, 0x3F800000u, 0x5F800000u,
                                 0x1F800000u, 0x20000000u, 0x00000000u, 0x80000000u, 0x00000001u, 0x7F7FFFFFu};
    const int oe = (int)((other >> 23) & 0xFF);
    switch (rnd() % 12) {
    case 0:
        return K[rnd() % (sizeof K / sizeof K[0])];
    case 1:
        return s | 0x7F800000u | (rnd() & 0x7FFFFFu);
    case 2:
        return (other ^ 0x80000000u) + (rnd() % 7) - 3; /* near cancellation */
    case 3: {
        int e = oe - 26 - (int)(rnd() % 35);
        if (e < 1) e = 1; /* exponent gap 26..60 */
        return s | ((uint32_t)e << 23) | (rnd() & 0x7FFFFFu);
    }
    case 4: {
        int e = 255 - oe + 126 + (int)(rnd() % 5) - 2;
        if (e < 1) e = 1;
        if (e > 255) e = 255; /* product near 2^128/2^129 */
        return s | ((uint32_t)e << 23) | (rnd() & 0x7FFFFFu);
    }
    case 5: {
        int e = 127 - 126 - (oe - 127) + (int)(rnd() % 5) - 2;
        if (e < 1) e = 1;
        if (e > 255) e = 255; /* product near 2^-126 */
        return s | ((uint32_t)e << 23) | (rnd() & 0x7FFFFFu);
    }
    case 6:
        return s | (1u << 23) | (rnd() & 0x7FFFFFu);
    case 7:
        return s | (rnd() & 0x7FFFFFu);
    case 8:
        return s | (254u << 23) | (rnd() & 0x7FFFFFu);
    case 9:
        return other;
    default:
        return rword();
    }
}
static long check_1089(void)
{
    long bad = 0, n = 0, lanes255 = 0;
    srand(67);
    for (int it = 0; it < 2000000; it++) {
        u128 a, b;
        for (int i = 0; i < 4; i++) {
            a._u32[i] = (rnd() & 3) ? rword89(0x3F800000u) : rword();
            if (rnd() & 1) a._u32[i] = rword89(rword());
            b._u32[i] = rword89(a._u32[i]);
            if (rnd() % 8 == 0) {
                const uint32_t t = a._u32[i];
                a._u32[i] = b._u32[i];
                b._u32[i] = t;
            }
            lanes255 += ((a._u32[i] | b._u32[i]) & 0x7F800000u) == 0x7F800000u;
        }
        for (int vec = 0; vec < 2; vec++) {
            g_kz3_spu_xrq = vec;
            u128 got[7], want[7];
            got[0] = spu_fa(a, b), want[0] = ref_fa(a, b);
            got[1] = spu_fs(a, b), want[1] = ref_fs(a, b);
            got[2] = spu_fm(a, b), want[2] = ref_fm(a, b);
            got[3] = spu_fceq(a, b), want[3] = ref_fceq(a, b);
            got[4] = spu_fcgt(a, b), want[4] = ref_fcgt(a, b);
            got[5] = spu_fcmeq(a, b), want[5] = ref_fcmeq(a, b);
            got[6] = spu_fcmgt(a, b), want[6] = ref_fcmgt(a, b);
            for (int k = 0; k < 7; k++)
                for (int i = 0; i < 4; i++, n++)
                    if (got[k]._u32[i] != want[k]._u32[i] && bad++ < 8)
                        printf("FAIL 1089 op %d vec %d: a=0x%08X b=0x%08X -> 0x%08X want 0x%08X\n", k, vec, a._u32[i],
                               b._u32[i], got[k]._u32[i], want[k]._u32[i]);
        }
    }
    /* cost of the recompute on the K4 probe's top cases (0 * frest(0), Smax + x, Smax * Smax, 0 - 0xFFFFFFFF) */
    for (int vec = 0; vec < 2; vec++) {
        g_kz3_spu_xrq = vec;
        u128 z = v1(0), e = v1(0x7FFFFC00u), sm = v1(0x7FFFFFFFu), m1 = v1(0xFFFFFFFFu), acc = v1(0x3F800000u);
        z._u32[1] = 0x3F800000u;
        e._u32[1] = 0x3F000000u;
        sm._u32[2] = 0x40000000u;
        const int N = 2000000;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++) {
            u128 p = spu_fm(z, e), q = spu_fa(sm, acc), r = spu_fm(sm, sm), t = spu_fs(z, m1);
            acc._u32[3] ^= p._u32[0] ^ q._u32[0] ^ r._u32[0] ^ t._u32[0];
            __asm__ volatile("" : "+x"(acc));
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("1089 bench xrq=%d: %.2f ns per 4 ops (fm 0*e255, fa Smax+x, fm Smax*Smax, fs 0-FFFFFFFF)\n", vec,
               ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / N);
    }
    g_kz3_spu_xrq = 1;
    printf("1089: %ld lanes compared with the 1071 forms (%ld operand pairs with an exponent-255 word), %ld differ\n",
           n, lanes255, bad);
    return bad;
}

int main(void)
{
    const uint32_t X = 0x7FB50000u, BIG = 0x7F800000u /* 2^128 */, ONE = 0x3F800000u;
    CHECK("fm 0*x255", spu_fm(v1(0), v1(X))._u32[0], 0);
    CHECK("fm x255*0", spu_fm(v1(X), v1(0))._u32[1], 0);
    CHECK("fm 0.5*2^128", spu_fm(v1(0x3F000000u), v1(BIG))._u32[0], 0x7F000000u);
    CHECK("fm 2*2^128 saturates", spu_fm(v1(0x40000000u), v1(BIG))._u32[0], 0x7FFFFFFFu);
    CHECK("fm -2*2^128 saturates", spu_fm(v1(0xC0000000u), v1(BIG))._u32[0], 0xFFFFFFFFu);
    CHECK("fm 2^127*4 saturates (host Inf)", spu_fm(v1(0x7F000000u), v1(0x40800000u))._u32[0], 0x7FFFFFFFu);
    CHECK("fa 2^128+2^128", spu_fa(v1(BIG), v1(BIG))._u32[0], 0x7FFFFFFFu);
    CHECK("fa 2^128-2^128", spu_fa(v1(BIG), v1(0xFF800000u))._u32[0], 0);
    CHECK("fs x-x", spu_fs(v1(X), v1(X))._u32[2], 0);
    CHECK("fa x+1 = x", spu_fa(v1(X), v1(ONE))._u32[0], X);
    CHECK("fma 0*x+1", spu_fma(v1(0), v1(X), v1(ONE))._u32[0], ONE);
    CHECK("fms 1*x-x", spu_fms(v1(ONE), v1(X), v1(X))._u32[0], 0);
    CHECK("fnms c-0*x", spu_fnms(v1(0), v1(X), v1(0x40400000u))._u32[3], 0x40400000u);
    CHECK("fma 2^127*4+0 saturates", spu_fma(v1(0x7F000000u), v1(0x40800000u), v1(0))._u32[0], 0x7FFFFFFFu);
    CHECK("fcgt 2^128>1", spu_fcgt(v1(BIG), v1(ONE))._u32[0], 0xFFFFFFFFu);
    CHECK("fcgt 1>2^128", spu_fcgt(v1(ONE), v1(BIG))._u32[0], 0);
    CHECK("fcgt -2^128>-1", spu_fcgt(v1(0xFF800000u), v1(0xBF800000u))._u32[0], 0);
    CHECK("fcmgt |-2^128|>1", spu_fcmgt(v1(0xFF800000u), v1(ONE))._u32[0], 0xFFFFFFFFu);
    CHECK("fceq x==x", spu_fceq(v1(0x7FC00000u), v1(0x7FC00000u))._u32[0], 0xFFFFFFFFu);
    CHECK("fcmeq |x|==|-x|", spu_fcmeq(v1(0x7FC00000u), v1(0xFFC00000u))._u32[0], 0xFFFFFFFFu);
    CHECK("cflts 2^128", spu_cflts(v1(BIG), 173)._u32[0], 0x7FFFFFFFu);
    CHECK("cflts -2^128", spu_cflts(v1(0xFF800000u), 173)._u32[0], 0x80000000u);
    CHECK("cfltu 2^128", spu_cfltu(v1(BIG), 173)._u32[0], 0xFFFFFFFFu);
    CHECK("cfltu -2^128", spu_cfltu(v1(0xFF800000u), 173)._u32[0], 0);
    CHECK("csflt 2^30*2^100 saturates", spu_csflt(v1(0x40000000u), 255)._u32[0], 0x7FFFFFFFu);
    CHECK("cuflt 2^31*2^100 saturates", spu_cuflt(v1(0x80000000u), 255)._u32[0], 0x7FFFFFFFu);
    /* the round-56 idiom (job_011c8080): rsqrt estimate of 0 is exponent 255, refined by fi, times 0 */
    {
        const u128 x = v1(0), est = spu_frsqest(x), r = spu_fi(x, est), n = spu_fm(x, r);
        if (((r._u32[0] >> 23) & 0xFF) != 255 || (r._u32[0] & 0x00400000u) == 0x00400000u && r._u32[0] == 0x7FF50000u) {
            fails++;
            printf("FAIL fi(0, frsqest(0)) = 0x%08X (want an exponent-255 estimate, not a host NaN)\n", r._u32[0]);
        }
        CHECK("0 * fi(0, frsqest(0))", n._u32[0], 0);
        g_kz3_spu_xr = 0;
        const u128 n0 = spu_fm(x, spu_fi(x, est));
        g_kz3_spu_xr = 1;
        uint32_t w = n0._u32[0];
        if (!((w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu))) {
            fails++;
            printf("FAIL KZ3_SPU_XR=0 path: 0x%08X is not the host NaN\n", w);
        } else
            printf("KZ3_SPU_XR=0: 0 * fi(0, frsqest(0)) = 0x%08X (host NaN, the round-56 bone-matrix pattern)\n", w);
    }
    /* every other lane: bit-identical with the patch off */
    srand(56);
    long diff = 0, n = 0;
    for (int it = 0; it < 200000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 4; i++) {
            a._u32[i] = rnorm();
            b._u32[i] = rnorm();
            c._u32[i] = rnorm();
        }
        if (it % 7 == 0) a._u32[it & 3] = 0;
        u128 r1[10], r0[10];
        for (int pass = 0; pass < 2; pass++) {
            g_kz3_spu_xr = pass == 0;
            u128* r = pass == 0 ? r1 : r0;
            r[0] = spu_fa(a, b);
            r[1] = spu_fs(a, b);
            r[2] = spu_fm(a, b);
            r[3] = spu_fma(a, b, c);
            r[4] = spu_fnms(a, b, c);
            r[5] = spu_fcgt(a, b);
            r[6] = spu_fcmgt(a, b);
            r[7] = spu_cflts(a, 173);
            r[8] = spu_fi(a, spu_frest(b));
            r[9] = spu_csflt(a, 155);
        }
        g_kz3_spu_xr = 1;
        for (int k = 0; k < 10; k++)
            for (int i = 0; i < 4; i++) {
                n++;
                const uint32_t h = r0[k]._u32[i];
                if ((h & 0x7F800000u) == 0x7F800000u && k != 5 && k != 6 && k != 7) continue; /* host Inf/NaN lane */
                if (r1[k]._u32[i] != h) {
                    if (diff++ < 5) printf("FAIL op %d lane %d: 0x%08X vs host 0x%08X\n", k, i, r1[k]._u32[i], h);
                }
            }
    }
    if (diff) fails++;
    if (check_1079()) fails++;
    if (check_1089()) fails++;
    printf("%ld lanes compared, %ld differ from the host results\n", n, diff);
    /* cost: one fm vector, patch on vs off */
    {
        volatile uint32_t sink = 0;
        u128 a = v1(0x3F8CCCCDu), b = v1(0x3F000000u), c = v1(0x3F800000u); /* a -> 0.5a + 0.5 stays normal */
        for (int pass = 0; pass < 2; pass++) {
            g_kz3_spu_xr = pass == 0;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int i = 0; i < 20000000; i++) {
                a = spu_fma(a, b, b);
                a = spu_fm(a, c);
                sink += a._u32[0];
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            printf("bench %s: %.2f ns per fma+fm\n", pass == 0 ? "xr on " : "xr off",
                   ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 20000000.0);
        }
        (void)sink;
    }
    printf(fails ? "test_spu_xr: %d FAILED\n" : "test_spu_xr: all passed%.0d\n", fails);
    return fails != 0;
}
