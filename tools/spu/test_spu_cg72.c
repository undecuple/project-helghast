/* Patch 1097 host test (SPU pass 72): the rewritten SPU helpers against copies of the old ones.
 * 1. fsmb/fsmbi (nibble table): all 65536 masks against the byte loop.
 * 2. cbd/chd/cwd/cdd generate-controls (tables): every position -64..63 against the byte loops.
 * 3. cflts/cfltu/csflt/cuflt (SSE4.1 double lanes): every scale immediate 0..255 on random words, random floats near
 *    each conversion's limits, +-0, +-inf, NaN, subnormals, integers +-0.5 and values just under/over 2^31 and 2^32,
 *    against the per-lane loops (exponent-255 recompute off: the raw lane values must match too).
 *    Negative control: cfltu with floor(v - 2^31) (inexact subtraction) instead of floor(v) - 2^31 must be caught.
 * 4. fma/fms/fnms rounding midpoints (also built with -mfma: CG72_ISA=-mfma, the hardware fast path of spu__fma4): exact midpoints (short-mantissa products) must give glibc fmaf's value without
 *    reaching the libm fallback; inexact ones (a tiny c below the double's last bit) must still give fmaf's value,
 *    through the fallback. Negative control: the plain double-to-float rounding of those inexact sums must differ
 *    from fmaf in some lanes (so the test does exercise the case the exactness test exists for).
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_cg72.sh */
#include <math.h>
static long n_fmaf_calls;
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

int g_kz3_spu_xr = 0; /* exponent-255 recompute off: raw lane values compared */
int g_kz3_spu_slowcg;
int g_kz3_fma_stats;
int g_kz3_xr_stats;
int g_kz3_spu_xrq = 1;
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t);
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra)
{
    (void)op, (void)m, (void)a, (void)b, (void)c, (void)r, (void)ra;
}
static long n_fb_lanes;
void kz3_spu_fma_note(const double s[4], int ftz)
{
    (void)s, (void)ftz;
    n_fb_lanes++;
}

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)rs;
}
static float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- the old helpers (before patch 1097), copied ---- */
static u128 old_fsmbi(int32_t imm)
{
    u128 r;
    uint32_t v = imm & 0xFFFF;
    for (int i = 0; i < 16; i++) r._u8[SPU_W(i)] = ((v >> (15 - i)) & 1) ? 0xFFu : 0;
    return r;
}
static u128 old_cbd_pos(int pos)
{
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03;
    return r;
}
static u128 old_chd_pos(int pos)
{
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1;
    r._u8[SPU_W(p)] = 0x02;
    r._u8[SPU_W(p + 1)] = 0x03;
    return r;
}
static u128 old_cwd_pos(int pos)
{
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3;
    for (int k = 0; k < 4; k++) r._u8[SPU_W(p + k)] = (uint8_t)k;
    return r;
}
static u128 old_cdd_pos(int pos)
{
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7;
    for (int k = 0; k < 8; k++) r._u8[SPU_W(p + k)] = (uint8_t)k;
    return r;
}
static __attribute__((noinline)) u128 old_cflts(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(173 - i8));
    for (int i = 0; i < 4; i++) {
        double v = (double)a._f32[i] * f;
        if (v > 2147483647.0) v = 2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        r._s32[i] = (int32_t)v;
    }
    return r;
}
static __attribute__((noinline)) u128 old_cfltu(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(173 - i8));
    for (int i = 0; i < 4; i++) {
        double v = (double)a._f32[i] * f;
        if (v < 0) v = 0;
        if (v > 4294967295.0) v = 4294967295.0;
        r._u32[i] = (uint32_t)v;
    }
    return r;
}
static __attribute__((noinline)) u128 old_csflt(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(i8 - 155));
    for (int i = 0; i < 4; i++) r._f32[i] = (float)((double)a._s32[i] * f);
    return r;
}
static __attribute__((noinline)) u128 old_cuflt(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(i8 - 155));
    for (int i = 0; i < 4; i++) r._f32[i] = (float)((double)a._u32[i] * f);
    return r;
}
/* wrong on purpose: floor of an inexact v - 2^31 */
static u128 bad_cfltu(u128 a, int i8)
{
    u128 r;
    double f = exp2((double)(173 - i8));
    for (int i = 0; i < 4; i++) {
        double v = (double)a._f32[i] * f;
        if (v < 0) v = 0;
        if (v > 4294967295.0) v = 4294967295.0;
        r._u32[i] = (uint32_t)(int32_t)floor(v - 2147483648.0) ^ 0x80000000u;
    }
    return r;
}
/* noinline wrappers: the helpers see a non-constant immediate too */
static __attribute__((noinline)) u128 w_cflts(u128 a, int i)
{
    return spu_cflts(a, i);
}
static __attribute__((noinline)) u128 w_cfltu(u128 a, int i)
{
    return spu_cfltu(a, i);
}
static __attribute__((noinline)) u128 w_csflt(u128 a, int i)
{
    return spu_csflt(a, i);
}
static __attribute__((noinline)) u128 w_cuflt(u128 a, int i)
{
    return spu_cuflt(a, i);
}

static float conv_edge(int k)
{
    static const float e[] = {0.0f,           -0.0f,           0.5f,
                              -0.5f,          0.999999940f,    1.0f,
                              1.5f,           -1.5f,           2147483520.0f,
                              2147483648.0f,  2147483904.0f,   -2147483648.0f,
                              -2147483904.0f, 4294967040.0f,   4294967296.0f,
                              4294967808.0f,  0x1p-149f,       -0x1p-149f,
                              0x1p-126f,      0x1.fffffep127f, -0x1.fffffep127f};
    if (k < (int)(sizeof e / sizeof e[0])) return e[k];
    switch (k - (int)(sizeof e / sizeof e[0])) {
    case 0:
        return __builtin_inff();
    case 1:
        return -__builtin_inff();
    case 2:
        return __builtin_nanf("");
    default:
        return -__builtin_nanf("");
    }
}
#define N_EDGE 25

int main(void)
{
    long bad = 0, n = 0;
    /* 1. fsmbi / fsmb */
    for (int v = 0; v < 65536; v++) {
        u128 x = old_fsmbi(v), y = spu_fsmbi(v), w;
        u128 a;
        a._u32[0] = (uint32_t)v | (rnd() << 16);
        a._u32[1] = rnd(), a._u32[2] = rnd(), a._u32[3] = rnd();
        w = spu_fsmb(a);
        n += 2;
        if (memcmp(&x, &y, 16) && bad++ < 5) printf("FAIL fsmbi %04X\n", v);
        if (memcmp(&x, &w, 16) && bad++ < 5) printf("FAIL fsmb %04X\n", v);
    }
    /* 2. generate-controls */
    for (int pos = -64; pos < 64; pos++) {
        u128 x[4] = {old_cbd_pos(pos), old_chd_pos(pos), old_cwd_pos(pos), old_cdd_pos(pos)};
        u128 y[4] = {spu_cbd_pos(pos), spu_chd_pos(pos), spu_cwd_pos(pos), spu_cdd_pos(pos)};
        for (int k = 0; k < 4; k++) {
            n++;
            if (memcmp(&x[k], &y[k], 16) && bad++ < 5) printf("FAIL c%cd pos %d\n", "bhwd"[k], pos);
        }
    }
    printf("fsmb/fsmbi and cbd/chd/cwd/cdd: %ld cases, %ld mismatches\n", n, bad);
    /* 3. conversions */
    long cn = 0, cbad = 0, cneg = 0;
    for (int i8 = 0; i8 < 256; i8++)
        for (int it = 0; it < 6000; it++) {
            u128 a;
            for (int l = 0; l < 4; l++) {
                const uint32_t r = rnd();
                if (it < 30) /* edges, each in all lanes once */
                    a._f32[l] = conv_edge((it + l * 7) % N_EDGE);
                else if (r % 4 == 0) /* floats near the limits of this scale */
                    a._f32[l] =
                        ldexpf(1.0f + (float)(rnd() & 0xFFFFFF) / 16777216.0f, (int)(i8 - 173 + 29 + rnd() % 5)) *
                        ((rnd() & 1) ? -1.0f : 1.0f);
                else if (r % 4 == 1) /* small values (fractions after scaling) */
                    a._f32[l] = ldexpf((float)(int32_t)rnd() / 2147483648.0f, (int)(i8 - 173 + (int)(rnd() % 8)));
                else
                    a._u32[l] = rnd();
            }
            u128 x[4] = {old_cflts(a, i8), old_cfltu(a, i8), old_csflt(a, i8), old_cuflt(a, i8)};
            u128 y[4] = {w_cflts(a, i8), w_cfltu(a, i8), w_csflt(a, i8), w_cuflt(a, i8)};
            for (int k = 0; k < 4; k++) {
                cn++;
                if (memcmp(&x[k], &y[k], 16)) {
                    /* csflt/cuflt: a NaN result cannot occur (integer input); both are plain values */
                    if (cbad++ < 5)
                        printf("FAIL %s i8=%d a=%08X %08X %08X %08X old=%08X %08X %08X %08X new=%08X %08X %08X %08X\n",
                               (const char*[]){"cflts", "cfltu", "csflt", "cuflt"}[k], i8, a._u32[0], a._u32[1],
                               a._u32[2], a._u32[3], x[k]._u32[0], x[k]._u32[1], x[k]._u32[2], x[k]._u32[3],
                               y[k]._u32[0], y[k]._u32[1], y[k]._u32[2], y[k]._u32[3]);
                }
            }
            u128 z = bad_cfltu(a, i8);
            if (memcmp(&z, &x[1], 16)) cneg++;
        }
    /* the constant-immediate forms (as the lifted code calls them) */
    for (int it = 0; it < 200000; it++) {
        u128 a;
        for (int l = 0; l < 4; l++) a._f32[l] = (float)(int32_t)rnd() / 65536.0f;
        u128 x = old_cfltu(a, 161), y = spu_cfltu(a, 161), x2 = old_cflts(a, 173), y2 = spu_cflts(a, 173);
        u128 x3 = old_csflt(a, 155), y3 = spu_csflt(a, 155);
        cn += 3;
        if ((memcmp(&x, &y, 16) || memcmp(&x2, &y2, 16) || memcmp(&x3, &y3, 16)) && cbad++ < 5)
            printf("FAIL constant-immediate conversion it %d\n", it);
    }
    printf("conversions: %ld cases, %ld mismatches; negative control (floor of inexact v - 2^31) caught %ld\n", cn,
           cbad, cneg);
    /* 4. fma midpoints, both FTZ modes (0 = default, 2 = no FTZ) */
    long fn = 0, fbad = 0, exact_mid = 0, inexact = 0, neg = 0, calls_exact = 0, calls_inexact = 0;
    for (int mode = 0; mode <= 2; mode += 2) {
        g_kz3_spu_slowcg = mode;
        for (int it = 0; it < 400000; it++) {
            u128 a, b, c;
            const int inex = it & 1;
            for (int l = 0; l < 4; l++) {
                /* a has 13 significant bits, b 12 and 13: products of 24-26 bits, many exactly on a midpoint
                 * (25 bits, last bit set). c: zero, a short value, or (inex) a tiny value far below the product */
                const uint32_t ma = (rnd() & 0xFFF) | 0x1001, mb = (rnd() & 0x7FF) | 0x801 | (rnd() & 0x1000);
                a._f32[l] = ldexpf((float)ma, (int)(rnd() % 40) - 20) * ((rnd() & 1) ? -1.0f : 1.0f);
                b._f32[l] = ldexpf((float)mb, (int)(rnd() % 40) - 20);
                const float p = a._f32[l] * b._f32[l];
                if (inex)
                    c._f32[l] = ldexpf(p, -60) * ((rnd() & 1) ? -1.0f : 1.0f) * (1.0f + (float)(rnd() & 7) / 8.0f);
                else
                    c._f32[l] = (rnd() & 1) ? 0.0f : ldexpf((float)(rnd() & 0xFF), (int)(rnd() % 40) - 20);
            }
            for (int op = 0; op < 3; op++) {
                const long c0 = n_fmaf_calls;
                u128 r = op == 0 ? spu_fma(a, b, c) : op == 1 ? spu_fms(a, b, c) : spu_fnms(a, b, c);
                const long used = n_fmaf_calls - c0;
                int mid = 0;
                for (int l = 0; l < 4; l++) {
                    const float A = a._f32[l], B = op == 2 ? -b._f32[l] : b._f32[l],
                                C = op == 1 ? -c._f32[l] : c._f32[l];
                    float ref = __builtin_fmaf(A, B, C);
                    if (mode == 0 && ref != 0.0f && fabsf(ref) < 0x1p-126f) ref = u2f(f2u(ref) & 0x80000000u);
                    const double s = (double)A * (double)B + (double)C;
                    uint64_t u;
                    memcpy(&u, &s, 8);
                    /* exactness of the double sum in binary128 (exact here: exponent gaps stay below 87 bits) */
                    const int exact = (__float128)A * (__float128)B + (__float128)C == (__float128)s;
                    if ((u & 0x1FFFFFFFull) == 0x10000000ull) mid |= exact ? 1 : 2;
                    if (inex && (float)s != ref) neg++;
                    fn++;
                    if (f2u(r._f32[l]) != f2u(ref) && fbad++ < 5)
                        printf("FAIL fma op %d mode %d lane %d: %a*%a+%a got %a want %a\n", op, mode, l, A, B, C,
                               r._f32[l], ref);
                }
                if (mid == 1) {
                    exact_mid++;
                    calls_exact += used;
                }
                if (mid & 2) {
                    inexact++;
                    calls_inexact += used;
                }
            }
        }
    }
    printf("fma: %ld lanes, %ld mismatches vs fmaf; exact-midpoint vectors %ld (libm calls %ld, must be 0), "
           "inexact-midpoint "
           "vectors %ld (libm calls %ld); negative control (plain rounding of inexact sums) differs in %ld lanes\n",
           fn, fbad, exact_mid, calls_exact, inexact, calls_inexact, neg);
    int ok = bad == 0 && cbad == 0 && cneg > 0 && fbad == 0 && exact_mid > 10000 && calls_exact == 0 &&
             inexact > 1000 && neg > 100;
#if !defined(__FMA__) /* with -mfma the fallback is the hardware fmaf (not counted) */
    ok = ok && calls_inexact > 0;
#endif
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
