/* Patch 1035 host test: the inlined SPU helpers against the old out-of-line paths.
 *
 *   tools/spu/test_spu_hot_helpers.sh   (compiles this twice: -msse4.1 and plain x86-64)
 *
 * Checks, over random and targeted inputs, bit for bit:
 *   spu_shufb (pshufb path when __SSE4_1__)  vs  a byte-loop reference written here (the old non-SSE code)
 *   spu_fma / spu_fms / spu_fnms             vs  libm fmaf per lane (the old code)
 *   spu_fi                                    vs  the old ldexpf formula
 *   spu_ls_read128 / spu_ls_write128          vs  big-endian byte loads/stores (and the 0xDF0 probe range
 *                                                 still reaching spu_ls_read_probe in policy mode)
 * Negative control: a shufb with a wrong special selector (0xE0 -> 0xFF) must be caught by the same
 * comparison; the test fails if it is not. Exit 0 = all pass. */
#include "spu_helpers.h"
int g_kz3_spu_xr = 0;     /* patch 1071 off: this test checks the host (IEEE) results */
int g_kz3_spu_slowcg = 2; /* patches 1038/1040: 2 = the fast paths without flush-to-zero; this test checks IEEE results
                            (1040's FTZ is checked by tools/spu/test_spu_fastcg.sh) */
int g_kz3_fma_stats;      /* patch 1040: fallback counters off; the stub below keeps the link self-contained */
int g_kz3_xr_stats;       /* patch 1088 probe off; stub keeps the link self-contained */
int g_kz3_spu_xrq = 1;    /* patch 1089: the inline exponent-255 lanes of fa/fs/fm (the runtime default) */
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
void kz3_spu_fma_note(const double s[4], int ftz)
{
    (void)s;
    (void)ftz;
}
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Link stubs for the inline LS accessors' diagnostic hooks (spu_channels.c in the real build). */
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa;
    (void)is_write;
    (void)p;
    (void)pc;
    (void)lr;
}

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static u128 rnd128(void)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = rnd();
    return r;
}
static int eq128(u128 a, u128 b)
{
    return memcmp(&a, &b, 16) == 0;
}

/* The old byte loop (spu_helpers.h without SSE4.1), copied so both builds compare against it. */
static u128 shufb_old(u128 a, u128 b, u128 c, int wrong)
{
    uint8_t cat[32];
    for (int j = 0; j < 16; j++) cat[j] = a._u8[SPU_W(j)];
    for (int j = 0; j < 16; j++) cat[16 + j] = b._u8[SPU_W(j)];
    u128 r;
    for (int t = 0; t < 16; t++) {
        uint8_t s = c._u8[SPU_W(t)], v;
        if ((s & 0xE0) == 0xE0)
            v = wrong ? 0xFF : 0x80;
        else if ((s & 0xC0) == 0xC0)
            v = 0xFF;
        else if ((s & 0xC0) == 0x80)
            v = 0x00;
        else
            v = cat[s & 0x1F];
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
static u128 fi_old(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t rb = b._u32[i], ra = a._u32[i], exp = (rb >> 23) & 0xFFu, base_bits = rb & 0xFFFFFC00u;
        float base;
        memcpy(&base, &base_bits, sizeof base);
        float step = ldexpf((float)(rb & 0x3FFu), (int)exp - 127 - 13);
        if (base < 0.0f) step = -step;
        float y = ldexpf((float)(ra & 0x7FFFFu), -19);
        r._f32[i] = base - step * y;
    }
    return r;
}

/* Floats biased towards the cases that matter: random bits (NaN, inf, subnormal, zero), and normal
 * values in a narrow exponent range so a*b and c overlap (cancellation, rounding midpoints). */
static float rndf(void)
{
    uint32_t u = rnd();
    switch (rnd() & 3) {
    case 0:
        break; /* any bit pattern */
    case 1:
        u = (u & 0x807FFFFFu) | ((120u + (rnd() & 15u)) << 23);
        break;
    case 2:
        u = (u & 0x807FFFFFu) | ((rnd() & 7u) << 23);
        break; /* tiny / subnormal products */
    default:
        u = (u & 0x80FFF000u) | (127u << 23);
        break; /* short mantissas: exact sums */
    }
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* Bit-identical, except that any NaN equals any NaN: the sign of a NaN result depends on how the compiler
 * evaluates the negated operand of fms/fnms (seen with hardware FMA too), and SPU single precision has no
 * NaNs (extended range), so no guest result depends on it. */
static int feq(float x, float y)
{
    return (isnan(x) && isnan(y)) || memcmp(&x, &y, 4) == 0;
}

int main(void)
{
    int fail = 0;
    const int N = 2000000;

    /* shufb: random controls, plus controls drawn only from the special/selector ranges. */
    long bad = 0, neg = 0;
    for (int n = 0; n < N; n++) {
        u128 a = rnd128(), b = rnd128(), c = rnd128();
        if (n & 1)
            for (int i = 0; i < 16; i++) c._u8[i] = (uint8_t)(0x80 | (rnd() & 0x7F));
        u128 want = shufb_old(a, b, c, 0);
        if (!eq128(spu_shufb(a, b, c), want)) bad++;
        if (!eq128(shufb_old(a, b, c, 1), want)) neg++; /* negative control through the same compare */
    }
    printf("shufb   %s: %ld mismatches of %d; negative control caught %ld\n",
#if defined(__SSE4_1__)
           "pshufb", bad, N, neg);
#else
           "byteloop", bad, N, neg);
#endif
    if (bad || !neg) fail = 1;

    /* fma family vs libm fmaf; count how often the fallback condition fired. */
    long fbad = 0, slow = 0;
    for (int n = 0; n < N; n++) {
        u128 a, b, c;
        for (int i = 0; i < 4; i++) {
            a._f32[i] = rndf();
            b._f32[i] = rndf();
            c._f32[i] = rndf();
        }
        u128 r1 = spu_fma(a, b, c), r2 = spu_fms(a, b, c), r3 = spu_fnms(a, b, c);
        for (int i = 0; i < 4; i++) {
            float w1 = fmaf(a._f32[i], b._f32[i], c._f32[i]);
            float w2 = fmaf(a._f32[i], b._f32[i], -c._f32[i]);
            float w3 = fmaf(a._f32[i], -b._f32[i], c._f32[i]);
            if (!feq(w1, r1._f32[i]) || !feq(w2, r2._f32[i]) || !feq(w3, r3._f32[i])) {
                if (fbad++ < 4)
                    printf("  fma mismatch a=%a b=%a c=%a: %a/%a %a/%a %a/%a\n", a._f32[i], b._f32[i], c._f32[i], w1,
                           r1._f32[i], w2, r2._f32[i], w3, r3._f32[i]);
            }
            double s = (double)a._f32[i] * b._f32[i] + c._f32[i];
            uint64_t u;
            memcpy(&u, &s, 8);
            uint32_t e = (uint32_t)(u >> 52) & 0x7FF;
            if ((u & 0x1FFFFFFFull) == 0x10000000ull || e - (1023u - 126u) >= 254u) slow++;
        }
    }
    /* Midpoint cases on purpose: a*b exact in double with the float rounding bit set and a tiny c. */
    long mid = 0, naive = 0;
    for (int n = 0; n < N / 4; n++) {
        uint32_t ua = (127u << 23) | (rnd() & 0x7FFFFFu), ub = (127u << 23) | (rnd() & 0x7FFFFFu);
        float a, b;
        memcpy(&a, &ua, 4);
        memcpy(&b, &ub, 4);
        double p = (double)a * b;
        uint64_t up;
        memcpy(&up, &p, 8);
        up = (up & ~0x1FFFFFFFull) | 0x10000000ull;
        memcpy(&p, &up, 8);                     /* a nearby midpoint */
        float c = (float)((p - (double)a * b)); /* push a*b+c onto it */
        float cs[3] = {c, nextafterf(c, 0.0f), nextafterf(c, 1e30f)};
        for (int k = 0; k < 3; k++) {
            float w = fmaf(a, b, cs[k]), g = spu__fmaf(a, b, cs[k]);
            if (!feq(w, g)) fbad++;
            double s = (double)a * b + cs[k];
            uint64_t u;
            memcpy(&u, &s, 8);
            if ((u & 0x1FFFFFFFull) == 0x10000000ull) mid++;
            if (!feq(w, (float)s)) naive++; /* negative control: double rounding without the fallback */
        }
    }
    /* Exact zero sums (patch 1035's inline zero case): c = -(a*b) with a*b exact in float, and signed zeros. */
    long zero = 0;
    for (int n = 0; n < N / 4; n++) {
        uint32_t ua = (rnd() & 0x80000000u) | ((100u + (rnd() & 63u)) << 23) | (rnd() & 0x7F0000u);
        uint32_t ub = (rnd() & 0x80000000u) | ((100u + (rnd() & 63u)) << 23) | (rnd() & 0x7F0000u);
        float a, b;
        memcpy(&a, &ua, 4);
        memcpy(&b, &ub, 4);
        if (n & 1) a = (n & 2) ? -0.0f : 0.0f;
        float c = -(a * b); /* exact: 8-bit mantissas */
        if (n & 4) c = -c;  /* +0/-0 variants when a*b is zero */
        float w = fmaf(a, b, c), g = spu__fmaf(a, b, c);
        if (!feq(w, g)) fbad++;
        if (w == 0.0f) zero++;
    }
    printf("fma     %s: %ld mismatches (fma/fms/fnms x %d x 4 lanes + %d midpoint probes); "
           "fallback lanes %ld, midpoint hits %ld; negative control (no fallback) wrong %ld; zero sums %ld\n",
#if defined(__FMA__)
           "hw", fbad, N, 3 * (N / 4), slow, mid, naive, zero);
#else
           "double", fbad, N, 3 * (N / 4), slow, mid, naive, zero);
#endif
    if (fbad || !mid || !naive || !zero) fail = 1;

    long ibad = 0;
    for (int n = 0; n < N; n++) {
        u128 a = rnd128(), b = rnd128();
        u128 r = spu_fi(a, b), w = fi_old(a, b);
        for (int i = 0; i < 4; i++)
            if (memcmp(&r._f32[i], &w._f32[i], 4) && !(isnan(r._f32[i]) && isnan(w._f32[i]))) ibad++;
    }
    printf("fi      : %ld mismatches of %d x 4 lanes\n", ibad, N);
    if (ibad) fail = 1;

    /* LS accessors: big-endian round trip, and the probe range still calls the probe. */
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    ctx->ls = ctx->ls_store;
    long lbad = 0;
    for (int n = 0; n < 200000; n++) {
        uint32_t lsa = rnd() & 0x3FFFFu;
        u128 v = rnd128();
        spu_ls_write128(ctx, lsa, v);
        const uint8_t* p = &ctx->ls[lsa & 0x3FFF0u];
        for (int i = 0; i < 4; i++) {
            uint32_t be =
                (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
            if (be != v._u32[i]) lbad++;
        }
        if (!eq128(spu_ls_read128(ctx, lsa ^ (rnd() & 0xF)), v)) lbad++;
    }
    ctx->image_id = 2;
    ctx->policy_mode = 1;
    g_wws_read_probe = 0;
#ifdef SPU_LS_DIAG_GATES
    g_spu_wws_probe = 1, g_spu_ls_rdiag = -1; /* patch 1118 (PR #445): the WWS probe is opt-in (SPU_WWS_PROBE) */
#endif
    for (uint32_t lsa = 0xD00; lsa < 0xF00; lsa += 4) (void)spu_ls_read128(ctx, lsa);
    printf("ls      : %ld mismatches; wws probe hits in 0xD00..0xEFF = %d (want 48 = the cap, from 48 in range)\n",
           lbad, g_wws_read_probe);
    if (lbad || g_wws_read_probe != 48) fail = 1;

    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
