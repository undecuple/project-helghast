/* Patch 1131 host test (SPU pass 110): the SSE4.1/AVX2 fi, frest, frsqest, shl, rot, rotm and rotma against copies of
 * the per-lane loops they replaced (the loops are also kept in the header as spu__*_ref; the copies here guard against
 * a change to both).
 * 1. frest / frsqest: every 32-bit input (each lane depends only on its own word).
 * 2. fi: 64 M random (a, b) pairs, plus every b exponent 0..255 with random mantissas and signs, b = +-0 and the
 *    10-bit step field all-ones; with the patch-1071 recompute off (raw lanes) and on (exponent-255 lanes through
 *    spu__fi_lane in both).
 * 3. shl / rot / rotm / rotma: 16 M random pairs, plus every count 0..127 in each lane with random high bits.
 * Negative control: frest without the exponent clamp (253 - e for e >= 253) must be caught.
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_r110_vec.sh */
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_kz3_spu_xr = 0;
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
void kz3_spu_fma_note(const double s[4], int ftz)
{
    (void)s, (void)ftz;
}

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}

/* ---- copies of the loops (ps3recomp spu_helpers.h before patch 1131) ---- */
static const uint32_t lut_rest[32] = {0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
                                      0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
                                      0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
                                      0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07};
static const uint32_t lut_rsq[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D, 0x21E4F9, 0x21E4F9, 0x1E00E9,
    0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB, 0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA,
    0x0B58A1, 0x0B58A1, 0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83, 0x7FFDF4,
    0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C, 0x64F561, 0x64F561, 0x5F7149, 0x5F7149,
    0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F, 0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4,
    0x4510E4, 0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA};
static uint32_t ref_frest(uint32_t bits, int noclamp)
{
    uint32_t sign = bits & 0x80000000u, exp = (bits >> 23) & 0xFFu, idx = (bits >> 18) & 0x1Fu;
    if (exp == 0u) return sign | 0x7FFFFFFFu;
    uint32_t rexp = (exp >= 253u && !noclamp) ? 0u : (253u - exp);
    return sign | (rexp << 23) | (lut_rest[idx] & 0x7FFFFFu);
}
static uint32_t ref_frsqest(uint32_t bits)
{
    uint32_t exp = (bits >> 23) & 0xFFu, idx = (bits >> 18) & 0x3Fu;
    uint32_t rexp = (exp == 0u) ? 0xFFu : (190u - (exp + 1u) / 2u);
    return (rexp << 23) | (lut_rsq[idx] & 0x7FFFFFu);
}
static uint32_t ref_fi(uint32_t ra, uint32_t rb)
{
    uint32_t exp = (rb >> 23) & 0xFFu;
    if (exp == 255u && g_kz3_spu_xr) return spu__fi_lane(ra, rb);
    uint32_t base_bits = rb & 0xFFFFFC00u;
    float base;
    memcpy(&base, &base_bits, 4);
    uint32_t step_frac = rb & 0x3FFu;
    uint64_t sc_bits = (uint64_t)(1023 + (int)exp - 140) << 52;
    double sc;
    memcpy(&sc, &sc_bits, 8);
    volatile float step = (float)((double)step_frac * sc);
    if (base < 0.0f) step = -step;
    volatile float y = (float)(ra & 0x7FFFFu) * 0x1p-19f;
    volatile float p = step * y;
    float r = base - p;
    uint32_t u;
    memcpy(&u, &r, 4);
    return u;
}
static uint32_t ref_sh(int op, uint32_t a, uint32_t b)
{
    uint32_t sh;
    switch (op) {
    case 0:
        sh = b & 0x3F;
        return sh > 31 ? 0 : a << sh;
    case 1:
        sh = b & 31;
        return sh ? (a << sh) | (a >> (32 - sh)) : a;
    case 2:
        sh = (0 - b) & 0x3F;
        return sh > 31 ? 0 : a >> sh;
    default:
        sh = (0 - b) & 0x3F;
        return (uint32_t)(sh > 31 ? ((int32_t)a >> 31) : ((int32_t)a >> sh));
    }
}
static u128 mk(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a, r._u32[1] = b, r._u32[2] = c, r._u32[3] = d;
    return r;
}
static const char* shn[4] = {"shl", "rot", "rotm", "rotma"};
static u128 do_sh(int op, u128 a, u128 b)
{
    return op == 0 ? spu_shl(a, b) : op == 1 ? spu_rot(a, b) : op == 2 ? spu_rotm(a, b) : spu_rotma(a, b);
}

int main(void)
{
    int fail = 0;
    long bad;
    /* 1. frest / frsqest, every input */
    long bad_rest = 0, bad_rsq = 0, neg = 0;
    for (uint64_t x = 0; x < (1ull << 32); x += 4) {
        const uint32_t w = (uint32_t)x;
        const u128 in = mk(w, w + 1, w + 2, w + 3);
        const u128 r1 = spu_frest(in), r2 = spu_frsqest(in);
        for (int i = 0; i < 4; i++) {
            if (r1._u32[i] != ref_frest(w + i, 0)) bad_rest++;
            if (r2._u32[i] != ref_frsqest(w + i)) bad_rsq++;
            if (r1._u32[i] != ref_frest(w + i, 1)) neg++;
        }
    }
    printf("frest   all 2^32 inputs: %ld mismatches\n", bad_rest);
    printf("frsqest all 2^32 inputs: %ld mismatches\n", bad_rsq);
    printf("negative control (frest without the exponent clamp): %ld lanes differ (must be > 0)\n", neg);
    fail |= bad_rest != 0 || bad_rsq != 0 || neg == 0;

    /* 2. fi */
    for (int xr = 0; xr <= 1; xr++) {
        g_kz3_spu_xr = xr;
        bad = 0;
        long n = 0;
        for (long k = 0; k < (16l << 20); k++) {
            uint32_t a[4], b[4];
            for (int i = 0; i < 4; i++) {
                a[i] = rnd();
                b[i] = rnd();
                switch (k & 7) { /* edge shapes in one lane of every other vector */
                case 1:
                    b[i] = (b[i] & 0x807FFFFFu) | ((uint32_t)(k >> 3) & 0xFF) << 23;
                    break;
                case 3:
                    b[i] = (b[i] & 0x80000000u) | (b[i] & 0x3FFu ? 0x3FFu : 0);
                    break;
                case 5:
                    b[i] |= 0x7F800000u;
                    break; /* exponent 255 */
                case 7:
                    b[i] &= 0x807FFFFFu;
                    break; /* exponent 0 */
                }
            }
            const u128 r = spu_fi(mk(a[0], a[1], a[2], a[3]), mk(b[0], b[1], b[2], b[3]));
            for (int i = 0; i < 4; i++, n++)
                if (r._u32[i] != ref_fi(a[i], b[i])) {
                    if (bad < 3)
                        printf("  fi mismatch a=%08x b=%08x got %08x want %08x\n", a[i], b[i], r._u32[i],
                               ref_fi(a[i], b[i]));
                    bad++;
                }
        }
        printf("fi (recompute %s) %ld lanes: %ld mismatches\n", xr ? "on" : "off", n, bad);
        fail |= bad != 0;
    }
    g_kz3_spu_xr = 0;

    /* 3. shifts */
    for (int op = 0; op < 4; op++) {
        bad = 0;
        for (long k = 0; k < (4l << 20); k++) {
            uint32_t a[4], b[4];
            for (int i = 0; i < 4; i++) {
                a[i] = rnd();
                b[i] = rnd();
                if (k & 1) b[i] = (b[i] & ~0x7Fu) | (uint32_t)((k >> 1) + 37 * i) % 128;
            }
            const u128 r = do_sh(op, mk(a[0], a[1], a[2], a[3]), mk(b[0], b[1], b[2], b[3]));
            for (int i = 0; i < 4; i++)
                if (r._u32[i] != ref_sh(op, a[i], b[i])) {
                    if (bad < 3)
                        printf("  %s mismatch a=%08x b=%08x got %08x want %08x\n", shn[op], a[i], b[i], r._u32[i],
                               ref_sh(op, a[i], b[i]));
                    bad++;
                }
        }
        printf("%-5s 16 M lanes: %ld mismatches\n", shn[op], bad);
        fail |= bad != 0;
    }
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
