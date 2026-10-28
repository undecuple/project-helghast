/* Pass 90: do upstream PR #370 (SSE2 float compares) and PR #369 (AVX2 truncating float lanes) give the same bits
 * as KZ3's own SPU helpers (patches 1000/1035/1038/1071/1079/1097/1105)? They do not, by design: KZ3's helpers
 * compare and add with the host's IEEE float ops (denormals are numbers, results round to nearest even), the PRs
 * implement upstream's SPU model (denormals are zero, results truncate toward zero). This test measures the
 * difference so the decision "9369/9370 stay out of the stack" has numbers behind it.
 *  1. fceq/fcgt/fcmeq/fcmgt: PR #370's SSE2 key/magnitude compares (copied below) against spu_fc* over random words,
 *     ordinary floats, and an edge list (+-0, denormals, FLT_MIN, exponent 255). Mismatches are counted per class.
 *  2. fa/fs/fm/fma: PR #369's lane rule (the double result with its 29 low significand bits cleared, narrowed) against
 *     spu_fa/fs/fm/fma on ordinary operands whose result is a normal single.
 * Build: tools/spu/test_r90_pr369_370.sh (needs the patched toolkit's runtime/spu). Exit 0 = the measured difference is
 * non-zero for both (the PRs are not drop-in); the SSE2 compares are also checked equal on the classes where they must be. */
#include <math.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <emmintrin.h>

int g_kz3_spu_xr = 1;
int g_kz3_spu_slowcg;
int g_kz3_fma_stats;
int g_kz3_xr_stats;
int g_kz3_spu_xrq = 1;
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t);
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra) { (void)op, (void)m, (void)a, (void)b, (void)c, (void)r, (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s, (void)ftz; }

/* ---- PR #370, verbatim: the SSE2 compares ---- */
static inline __m128i spu__sse_mag(__m128i x) {
    const __m128i m = _mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i denorm = _mm_cmpeq_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128());
    return _mm_andnot_si128(denorm, m);
}
static inline __m128i spu__sse_key(__m128i x) {
    const __m128i neg = _mm_srai_epi32(x, 31);
    return _mm_sub_epi32(_mm_xor_si128(spu__sse_mag(x), neg), neg);
}
#define PRCMP(name, cmp, key) static u128 name(u128 a, u128 b) { \
    __m128i A, B; memcpy(&A, &a, 16); memcpy(&B, &b, 16); \
    const __m128i r = cmp(key(A), key(B)); u128 out; memcpy(&out, &r, 16); return out; }
PRCMP(pr_fceq,  _mm_cmpeq_epi32, spu__sse_key)
PRCMP(pr_fcgt,  _mm_cmpgt_epi32, spu__sse_key)
PRCMP(pr_fcmeq, _mm_cmpeq_epi32, spu__sse_mag)
PRCMP(pr_fcmgt, _mm_cmpgt_epi32, spu__sse_mag)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static const uint32_t edge[] = { 0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000,
    0x80800000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF,
    0xFFFFFFFF, 0x7F800001, 0x40000000, 0xC0000000 };
#define NE (sizeof edge / sizeof edge[0])
static uint32_t pick(int cls)   /* 0 random words, 1 ordinary floats, 2 edge list, 3 edge-or-random */
{
    if (cls == 0) return rnd();
    if (cls == 1) { uint32_t e = 1 + rnd() % 254; return (rnd() & 0x80000000u) | e << 23 | (rnd() & 0x7FFFFF); }
    if (cls == 2) return edge[rnd() % NE];
    return (rnd() & 1) ? edge[rnd() % NE] : rnd();
}
static u128 vec(int cls) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = pick(cls); return v; }
static int isden(uint32_t x) { return (x & 0x7F800000u) == 0 && (x & 0x7FFFFFu) != 0; }
static int ise255(uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u; }

int main(void)
{
    const char* cn[4] = { "random words", "ordinary floats", "edge list", "edge or random" };
    int bad_nodenorm = 0, any_diff = 0;
    typedef u128 (*fn2)(u128, u128);
    struct { const char* n; fn2 ours, pr; } cmp[4] = { { "fceq", spu_fceq, pr_fceq }, { "fcgt", spu_fcgt, pr_fcgt },
        { "fcmeq", spu_fcmeq, pr_fcmeq }, { "fcmgt", spu_fcmgt, pr_fcmgt } };
    printf("-- #370 SSE2 compares vs KZ3 spu_fc* (1e6 vectors per class; lanes with a denormal operand counted apart)\n");
    for (int o = 0; o < 4; o++)
        for (int c = 0; c < 4; c++) {
            long lanes = 0, diff = 0, diff_den = 0, diff_nonden = 0, diff_e255 = 0;
            for (int n = 0; n < 1000000; n++) {
                u128 a = vec(c), b = vec(c);
                u128 r1 = cmp[o].ours(a, b), r2 = cmp[o].pr(a, b);
                for (int i = 0; i < 4; i++) {
                    lanes++;
                    if (r1._u32[i] == r2._u32[i]) continue;
                    diff++;
                    if (isden(a._u32[i]) || isden(b._u32[i])) diff_den++;
                    else { diff_nonden++; if (ise255(a._u32[i]) || ise255(b._u32[i])) diff_e255++; }
                }
            }
            printf("  %-5s %-16s lanes %8ld differ %7ld (denormal operand %7ld, other %ld of which exp255 %ld)\n",
                   cmp[o].n, cn[c], lanes, diff, diff_den, diff_nonden, diff_e255);
            bad_nodenorm += diff_nonden > diff_e255 ? 1 : 0;   /* unexplained: neither denormal nor exponent 255 */
            any_diff += diff > 0;
        }

    printf("-- #369 truncating lane rule vs KZ3 nearest-even (ordinary operands, normal results)\n");
    long fa_n = 0, fa_d = 0, fm_n = 0, fm_d = 0, fma_n = 0, fma_d = 0;
    for (int n = 0; n < 2000000; n++) {
        u128 a = vec(1), b = vec(1), c = vec(1);
        u128 ra = spu_fa(a, b), rm = spu_fm(a, b), rf = spu_fma(a, b, c);
        for (int i = 0; i < 4; i++) {
            const double x = a._f32[i], y = b._f32[i], z = c._f32[i];
            double d[3] = { x + y, x * y, x * y + z };   /* product exact in double; a*b+z rounded once in double */
            u128* res[3] = { &ra, &rm, &rf };
            long* cnt[3][2] = { { &fa_n, &fa_d }, { &fm_n, &fm_d }, { &fma_n, &fma_d } };
            for (int k = 0; k < 3; k++) {
                uint64_t u; memcpy(&u, &d[k], 8);
                const int e = (int)((u >> 52) & 0x7FF) - 1023;
                if (e < -126 || e > 127 || (u << 1) == 0) continue;      /* the PR sends these lanes to its scalar path */
                if (k == 0 && abs((int)((a._u32[i] >> 23) & 255) - (int)((b._u32[i] >> 23) & 255)) > 29) continue;
                if (k == 2 && (u & 0x1FFFFFFFull) == 0) continue;         /* possible 24-bit boundary: PR's exact path */
                u &= ~0x1FFFFFFFull; double t; memcpy(&t, &u, 8);
                const float tr = (float)t;
                uint32_t tb; memcpy(&tb, &tr, 4);
                (*cnt[k][0])++;
                if (tb != res[k]->_u32[i]) (*cnt[k][1])++;
            }
        }
    }
    printf("  fa   lanes %9ld truncated != ours %9ld (%.1f %%)\n", fa_n, fa_d, 100.0 * fa_d / fa_n);
    printf("  fm   lanes %9ld truncated != ours %9ld (%.1f %%)\n", fm_n, fm_d, 100.0 * fm_d / fm_n);
    printf("  fma  lanes %9ld truncated != ours %9ld (%.1f %%)\n", fma_n, fma_d, 100.0 * fma_d / fma_n);
    /* 3. the adaptation #370 would need to be bit-identical to KZ3: the same integer keys without the denormal flush */
    printf("-- adapted #370 (no denormal flush) vs KZ3 spu_fc*, xr on\n");
    long ad_diff = 0;
    for (int o = 0; o < 4; o++)
        for (int c = 0; c < 4; c++)
            for (int n = 0; n < 1000000; n++) {
                u128 a = vec(c), b = vec(c);
                __m128i A, B; memcpy(&A, &a, 16); memcpy(&B, &b, 16);
                const __m128i ma = _mm_and_si128(A, _mm_set1_epi32(0x7FFFFFFF)), mb = _mm_and_si128(B, _mm_set1_epi32(0x7FFFFFFF));
                const __m128i na = _mm_srai_epi32(A, 31), nb = _mm_srai_epi32(B, 31);
                const __m128i ka = _mm_sub_epi32(_mm_xor_si128(ma, na), na), kb = _mm_sub_epi32(_mm_xor_si128(mb, nb), nb);
                __m128i r = o == 0 ? _mm_cmpeq_epi32(ka, kb) : o == 1 ? _mm_cmpgt_epi32(ka, kb)
                          : o == 2 ? _mm_cmpeq_epi32(ma, mb) : _mm_cmpgt_epi32(ma, mb);
                u128 got, want = cmp[o].ours(a, b); memcpy(&got, &r, 16);
                ad_diff += memcmp(&got, &want, 16) != 0;
            }
    printf("  vectors differing: %ld of 16000000\n", ad_diff);
    {
        volatile uint32_t sink = 0; u128 a = vec(1), b = vec(1), acc = a;
        clock_t t0 = clock();
        for (long n = 0; n < 200000000L; n++) { u128 r = spu_fcgt(acc, b); acc._u32[0] += r._u32[1] + 1; acc._u32[1] ^= r._u32[0]; }
        clock_t t1 = clock();
        __m128i B; memcpy(&B, &b, 16);
        for (long n = 0; n < 200000000L; n++) {
            __m128i A; memcpy(&A, &acc, 16);
            const __m128i ma = _mm_and_si128(A, _mm_set1_epi32(0x7FFFFFFF)), mb = _mm_and_si128(B, _mm_set1_epi32(0x7FFFFFFF));
            const __m128i na = _mm_srai_epi32(A, 31), nb = _mm_srai_epi32(B, 31);
            const __m128i r = _mm_cmpgt_epi32(_mm_sub_epi32(_mm_xor_si128(ma, na), na), _mm_sub_epi32(_mm_xor_si128(mb, nb), nb));
            u128 rr; memcpy(&rr, &r, 16); acc._u32[0] += rr._u32[1] + 1; acc._u32[1] ^= rr._u32[0];
        }
        clock_t t2 = clock(); sink += acc._u32[0];
        printf("  fcgt chain: KZ3 %.2f ns, adapted integer %.2f ns (sink %u)\n", (t1 - t0) * 1e9 / CLOCKS_PER_SEC / 2e8,
               (t2 - t1) * 1e9 / CLOCKS_PER_SEC / 2e8, (unsigned)sink);
    }
    const int pass = ad_diff == 0 && bad_nodenorm == 0 && fa_d > 0 && fm_d > 0 && fma_d > 0 && any_diff > 0;
    printf("%s: #370 differs only on denormal operands%s; #369 truncation differs from KZ3 rounding on every op\n",
           pass ? "PASS" : "FAIL", bad_nodenorm ? " (UNEXPECTED other differences)" : "");
    return pass ? 0 : 1;
}
