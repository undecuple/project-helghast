/* Host test for patches 1162 (SSE absdb/avgb) and 1163 (FP NaN-first). Built by test_vekta_helpers.sh against the patched
 * toolkit's real spu_helpers.h (HDR) and, for 1163, the helper block cut out of the patched ppu_lifter.py (NEW_FP, symbols
 * renamed nw_*), compared with the old forms kept here verbatim (od_*) and the old scalar SPU loops (ref_*).
 * Prints "RESULT ... mismatches=N"; exit 1 on any mismatch. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include HDR
#include NEW_FP /* nw_ppu_fp_quiet, nw_ppu_fp_default_qnan, nw_ppu_fp_bin, nw_ppu_fmadd_core */

/* ---- old (pre-1163) helpers, verbatim apart from the od_ prefix ---- */
static inline double od_quiet(double x)
{
    uint64_t b;
    memcpy(&b, &x, 8);
    b |= 0x0008000000000000ULL;
    memcpy(&x, &b, 8);
    return x;
}
static inline double od_dq(void)
{
    uint64_t q = 0x7FF8000000000000ULL;
    double d;
    memcpy(&d, &q, 8);
    return d;
}
static inline double od_fp_bin(double a, double b, double r)
{
    if (a != a) return od_quiet(a);
    if (b != b) return od_quiet(b);
    if (r != r) return od_dq();
    return r;
}
static inline double od_fmadd_core(double a, double c, double b, int neg_b, int neg_res)
{
    if (a != a) return od_quiet(a);
    if (b != b) return od_quiet(b);
    if (c != c) return od_quiet(c);
    double r = fma(a, c, neg_b ? -b : b);
    if (r != r) return od_dq();
    return neg_res ? -r : r;
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}
static double bits2d(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static int same(double x, double y)
{
    return memcmp(&x, &y, 8) == 0;
}

int main(void)
{
    long bad_simd = 0, n_simd = 0, bad_fp = 0, n_fp = 0;
    /* ---- 1162: absdb/avgb vs the old scalar loops ---- */
    for (long it = 0; it < 3000000; it++) {
        u128 a, b, ra, rb, na, nb;
        for (int i = 0; i < 2; i++) {
            a._u64[i] = rnd();
            b._u64[i] = rnd();
        }
        if (it < 65536)
            for (int i = 0; i < 16; i++) {
                a._u8[i] = (uint8_t)((it >> (i & 7)) & 1 ? 255 : (i * 17));
                b._u8[i] = (uint8_t)(it >> (8 * (i & 1)));
            } /* edges */
        if (it % 5 == 0)
            for (int i = 0; i < 16; i++) {
                static const uint8_t e[4] = {0, 1, 254, 255};
                a._u8[i] = e[(rnd() >> 3) & 3];
                b._u8[i] = e[(rnd() >> 9) & 3];
            }
        for (int i = 0; i < 16; i++) {
            uint8_t x = a._u8[i], y = b._u8[i];
            ra._u8[i] = (uint8_t)(x > y ? x - y : y - x);
            rb._u8[i] = (uint8_t)(((uint32_t)x + (uint32_t)y + 1u) >> 1);
        }
        na = spu_absdb(a, b);
        nb = spu_avgb(a, b);
        n_simd += 2;
        if (memcmp(&ra, &na, 16)) bad_simd++;
        if (memcmp(&rb, &nb, 16)) bad_simd++;
    }
    /* ---- 1163: old vs new FP helpers, special-value cross product then random bits ---- */
    static const uint64_t sp[] = {0,
                                  0x8000000000000000ULL,
                                  1,
                                  0x000FFFFFFFFFFFFFULL,
                                  0x0010000000000000ULL,
                                  0x3FF0000000000000ULL,
                                  0xBFF0000000000000ULL,
                                  0x4000000000000000ULL,
                                  0x7FEFFFFFFFFFFFFFULL,
                                  0xFFEFFFFFFFFFFFFFULL,
                                  0x7FF0000000000000ULL,
                                  0xFFF0000000000000ULL,
                                  0x7FF0000000000001ULL,
                                  0xFFF0000000000001ULL,
                                  0x7FF8000000000000ULL,
                                  0xFFF8000000000000ULL,
                                  0x7FF8000000000123ULL,
                                  0xFFF4000000000042ULL,
                                  0x7FFFFFFFFFFFFFFFULL,
                                  0x0000000000000001ULL | 0x8000000000000000ULL};
    const int NS = (int)(sizeof sp / sizeof sp[0]);
    for (int i = 0; i < NS; i++)
        for (int j = 0; j < NS; j++) {
            double a = bits2d(sp[i]), b = bits2d(sp[j]);
            double rs_[4] = {a + b, a - b, a * b, a / b};
            for (int k = 0; k < 4; k++) {
                n_fp++;
                if (!same(od_fp_bin(a, b, rs_[k]), nw_ppu_fp_bin(a, b, rs_[k]))) bad_fp++;
            }
            for (int m = 0; m < NS; m++) {
                double c = bits2d(sp[m]);
                for (int f = 0; f < 4; f++) {
                    n_fp++;
                    if (!same(od_fmadd_core(a, c, b, f & 1, f >> 1), nw_ppu_fmadd_core(a, c, b, f & 1, f >> 1)))
                        bad_fp++;
                }
            }
        }
    for (long it = 0; it < 4000000; it++) {
        uint64_t x = rnd(), y = rnd(), z = rnd();
        if (it & 1) {
            x = (x & 0x800FFFFFFFFFFFFFULL) | ((x & 0x70000000000ULL) ? 0x3FF0000000000000ULL : 0x7FF0000000000000ULL);
        } /* bias to normal / inf-nan */
        double a = bits2d(x), b = bits2d(y), c = bits2d(z);
        double r4[4] = {a + b, a - b, a * b, a / b};
        for (int k = 0; k < 4; k++) {
            n_fp++;
            if (!same(od_fp_bin(a, b, r4[k]), nw_ppu_fp_bin(a, b, r4[k]))) bad_fp++;
        }
        n_fp++;
        if (!same(od_fmadd_core(a, c, b, it & 2, it & 4), nw_ppu_fmadd_core(a, c, b, it & 2, it & 4))) bad_fp++;
    }
    printf("RESULT simd compared=%ld mismatches=%ld\n", n_simd, bad_simd);
    printf("RESULT fp compared=%ld mismatches=%ld\n", n_fp, bad_fp);
    return (bad_simd || bad_fp) ? 1 : 0;
}
