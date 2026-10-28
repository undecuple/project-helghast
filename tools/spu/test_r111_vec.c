/* Patches 1132-1134 host test (SPU pass 111): the vector forms against copies of the per-lane loops they replaced.
 * 1. mpy (signed lo x lo) and mpyu (unsigned lo x lo): every pair of 16-bit halves (2^32), with random other halves.
 *    mpya, mpys, mpyi, mpyui, mpyh, mpyhh, mpyhha, mpyhhu, mpyhhau: 64 M random lanes plus the edge halves
 *    0, 1, 0x7FFF, 0x8000, 0xFFFF in every combination.
 * 2. clz: every 32-bit input. cntb: every byte value in every byte position, plus 16 M random vectors.
 * 3. and, or, xor, nand, nor, andc, orc, selb: 16 M random vectors.
 *    gb, gbh, gbb: every 16-bit LSB pattern (gbb), 16 M random vectors; fsm, fsmh: every low byte with random high bits.
 * 4. The quadword byte shifts/rotates (1133): shlqbyi, rotqbyi, shlqby, rotqby, shlqbybi, rotqbybi, rotqmby,
 *    rotqmbybi, rotqmbyi on 1 M random vectors with every count 0..127, with KZ3_SPU_FASTCG on (pshufb) and off
 *    (spu__byteperm_ref): both equal the old loops.
 * 5. fi (1134): spu__fi_int4 against spu__fi_lane on 64 M random (a, b) with every exponent 0..255 in turn; spu_fi with
 *    the recompute on against the 1131 loop (spu__fi_ref) on 64 M lanes, a quarter of them exponent 255.
 * Negative controls: clz without the x & ~(x >> 1) step, mpy without zeroing the other halfword (both must differ).
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_r111_vec.sh */
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
    return (uint32_t)(rs >> 16) ^ (uint32_t)(rs << 20);
}
static u128 mk(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a;
    r._u32[1] = b;
    r._u32[2] = c;
    r._u32[3] = d;
    return r;
}
static u128 rv(void)
{
    return mk(rnd(), rnd(), rnd(), rnd());
}
static int same(u128 x, u128 y)
{
    return memcmp(&x, &y, 16) == 0;
}

/* the loops as they were (spu_helpers.h before 1132) */
static u128 r_mpy(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._s32[i] = (int32_t)a._s16[i * 2] * (int32_t)b._s16[i * 2];
    return r;
}
static u128 r_mpya(u128 a, u128 b, u128 c)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = (uint32_t)((int32_t)a._s16[i * 2] * (int32_t)b._s16[i * 2]) + c._u32[i];
    return r;
}
static u128 r_mpyu(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = (uint32_t)a._u16[i * 2] * (uint32_t)b._u16[i * 2];
    return r;
}
static u128 r_mpyi(u128 a, int32_t imm)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._s32[i] = (int32_t)a._s16[i * 2] * (int16_t)imm;
    return r;
}
static u128 r_mpyhhu(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = (uint32_t)a._u16[2 * i + 1] * (uint32_t)b._u16[2 * i + 1];
    return r;
}
static u128 r_mpyhha(u128 a, u128 b, u128 t)
{
    u128 r;
    for (int i = 0; i < 4; i++)
        r._u32[i] = t._u32[i] + (uint32_t)((int32_t)a._s16[2 * i + 1] * (int32_t)b._s16[2 * i + 1]);
    return r;
}
static u128 r_mpyhhau(u128 a, u128 b, u128 t)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = t._u32[i] + (uint32_t)a._u16[2 * i + 1] * (uint32_t)b._u16[2 * i + 1];
    return r;
}
static u128 r_mpyh(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = (uint32_t)((int32_t)a._s16[2 * i + 1] * (int32_t)b._s16[2 * i]) << 16;
    return r;
}
static u128 r_mpyhh(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._s32[i] = (int32_t)a._s16[2 * i + 1] * (int32_t)b._s16[2 * i + 1];
    return r;
}
static u128 r_mpys(u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 4; i++) {
        int32_t p = (int32_t)a._s16[2 * i] * (int32_t)b._s16[2 * i];
        r._s32[i] = (int16_t)(p >> 16);
    }
    return r;
}
static u128 r_mpyui(u128 a, int32_t imm)
{
    u128 r;
    for (int i = 0; i < 4; i++) r._u32[i] = (uint32_t)a._u16[2 * i] * (uint32_t)(uint16_t)imm;
    return r;
}
static u128 r_selb(u128 a, u128 b, u128 c)
{
    u128 r;
    for (int i = 0; i < 2; i++) r._u64[i] = (a._u64[i] & ~c._u64[i]) | (b._u64[i] & c._u64[i]);
    return r;
}
static u128 r_log(int op, u128 a, u128 b)
{
    u128 r;
    for (int i = 0; i < 2; i++) {
        uint64_t x = a._u64[i], y = b._u64[i];
        r._u64[i] = op == 0   ? x & y
                    : op == 1 ? x | y
                    : op == 2 ? x ^ y
                    : op == 3 ? ~(x & y)
                    : op == 4 ? ~(x | y)
                    : op == 5 ? x & ~y
                              : x | ~y;
    }
    return r;
}
static uint32_t r_clz32(uint32_t x)
{
    return x ? (uint32_t)__builtin_clz(x) : 32u;
}
static u128 r_cntb(u128 a)
{
    u128 r;
    for (int i = 0; i < 16; i++) {
        uint8_t v = a._u8[i], c = 0;
        while (v) {
            c += v & 1;
            v >>= 1;
        }
        r._u8[i] = c;
    }
    return r;
}
static u128 r_gb(u128 a)
{
    u128 r = mk(0, 0, 0, 0);
    r._u32[0] = ((a._u32[0] & 1) << 3) | ((a._u32[1] & 1) << 2) | ((a._u32[2] & 1) << 1) | (a._u32[3] & 1);
    return r;
}
static u128 r_gbh(u128 a)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint32_t)(a._u16[i ^ 1] & 1) << (7 - i));
    u128 r = mk(0, 0, 0, 0);
    r._u32[0] = v;
    return r;
}
static u128 r_gbb(u128 a)
{
    uint32_t v = 0;
    for (int i = 0; i < 16; i++) v |= ((uint32_t)(a._u8[SPU_W(i)] & 1) << (15 - i));
    u128 r = mk(0, 0, 0, 0);
    r._u32[0] = v;
    return r;
}
static u128 r_fsm(u128 a)
{
    u128 r;
    uint32_t v = a._u32[0] & 0xF;
    for (int i = 0; i < 4; i++) r._u32[i] = ((v >> (3 - i)) & 1) ? 0xFFFFFFFFu : 0;
    return r;
}
static u128 r_fsmh(u128 a)
{
    u128 r;
    uint32_t v = a._u32[0] & 0xFF;
    for (int i = 0; i < 8; i++) r._u16[i ^ 1] = ((v >> (7 - i)) & 1) ? 0xFFFFu : 0;
    return r;
}
/* byte shift/rotate loops: left by sh with zero fill (shl), rotate (rot), right by sh with zero fill (rotqm) */
static u128 r_shl(u128 a, int sh)
{
    u128 r = mk(0, 0, 0, 0);
    for (int i = 0; i < 16; i++) {
        int s = i + sh;
        if (s < 16) r._u8[SPU_W(i)] = a._u8[SPU_W(s)];
    }
    return r;
}
static u128 r_rot(u128 a, int sh)
{
    u128 r;
    for (int i = 0; i < 16; i++) r._u8[SPU_W(i)] = a._u8[SPU_W((i + sh) & 0x0F)];
    return r;
}
static u128 r_rotm(u128 a, int sh)
{
    u128 r = mk(0, 0, 0, 0);
    for (int i = 0; i < 16; i++) {
        int s = i - sh;
        if (s >= 0) r._u8[SPU_W(i)] = a._u8[SPU_W(s)];
    }
    return r;
}

static long nbad;
static void chk(const char* what, u128 got, u128 want, long* bad)
{
    if (!same(got, want)) {
        if (*bad < 3) printf("  %s mismatch\n", what);
        (*bad)++;
    }
}

int main(void)
{
    int fail = 0;
    /* 1. multiplies */
    long bm = 0, bu = 0, neg = 0;
    for (uint64_t x = 0; x < (1ull << 32); x += 4) {
        const uint32_t ah = (uint32_t)(x >> 16), bl = (uint32_t)x & 0xFFFF; /* a half fixed per 65536, b halves vary */
        const uint32_t j = rnd();
        const u128 a = mk(ah | (j << 16), ah | (j & 0xFFFF0000u), ah, ah | 0xFFFF0000u);
        const u128 b = mk(bl | (j << 16), (bl + 1) | 0x80000000u, (bl + 2) | (j & 0xFFFF0000u), bl + 3);
        chk("mpy", spu_mpy(a, b), r_mpy(a, b), &bm);
        chk("mpyu", spu_mpyu(a, b), r_mpyu(a, b), &bu);
        u128 n = r_mpy(a, b); /* negative control: no masking of a's other halfword */
        for (int i = 0; i < 4; i++)
            n._s32[i] = (int32_t)a._s16[i * 2] * (int32_t)b._s16[i * 2] +
                        (int32_t)a._s16[i * 2 + 1] * (int32_t)b._s16[i * 2 + 1];
        if (!same(n, r_mpy(a, b))) neg++;
    }
    printf("mpy, mpyu: every 16-bit pair: %ld / %ld mismatches; negative control (no halfword mask) %ld differ (> 0)\n",
           bm, bu, neg);
    fail |= bm || bu || !neg;
    long bo = 0;
    static const uint32_t edge[5] = {0, 1, 0x7FFF, 0x8000, 0xFFFF};
    for (long k = 0; k < (16l << 20) + 5 * 5 * 5 * 5; k++) {
        u128 a = rv(), b = rv(), c = rv();
        if (k >= (16l << 20)) { /* edge halves: a.lo, a.hi, b.lo, b.hi */
            const long e = k - (16l << 20);
            const uint32_t w0 = edge[e % 5] | edge[e / 5 % 5] << 16, w1 = edge[e / 25 % 5] | edge[e / 125 % 5] << 16;
            a = mk(w0, w1, w0, w1);
            b = mk(w1, w0, w1, w0);
        }
        const int32_t imm = (int32_t)rnd();
        chk("mpya", spu_mpya(a, b, c), r_mpya(a, b, c), &bo);
        chk("mpys", spu_mpys(a, b), r_mpys(a, b), &bo);
        chk("mpyi", spu_mpyi(a, imm), r_mpyi(a, imm), &bo);
        chk("mpyui", spu_mpyui(a, imm), r_mpyui(a, imm), &bo);
        chk("mpyh", spu_mpyh(a, b), r_mpyh(a, b), &bo);
        chk("mpyhh", spu_mpyhh(a, b), r_mpyhh(a, b), &bo);
        chk("mpyhha", spu_mpyhha(a, b, c), r_mpyhha(a, b, c), &bo);
        chk("mpyhhu", spu_mpyhhu(a, b), r_mpyhhu(a, b), &bo);
        chk("mpyhhau", spu_mpyhhau(a, b, c), r_mpyhhau(a, b, c), &bo);
    }
    printf(
        "mpya mpys mpyi mpyui mpyh mpyhh mpyhha mpyhhu mpyhhau: 16 M random + 625 edge vectors each: %ld mismatches\n",
        bo);
    fail |= bo != 0;

    /* 2. clz, cntb */
    long bc = 0, nc = 0;
    for (uint64_t x = 0; x < (1ull << 32); x += 4) {
        const uint32_t w = (uint32_t)x;
        const u128 r = spu_clz(mk(w, w + 1, w + 2, w + 3));
        for (int i = 0; i < 4; i++) {
            const uint32_t v = w + (uint32_t)i;
            if (r._u32[i] != r_clz32(v)) bc++;
            /* negative control: the float trick without x & ~(x >> 1) (rounds up for x >= 2^24) */
            float f = (float)(int32_t)v;
            uint32_t fb;
            memcpy(&fb, &f, 4);
            uint32_t c = 158u - ((fb >> 23) & 0xFF);
            if (c > 32) c = 32;
            if (v >> 31) c = 0;
            if (c != r_clz32(v)) nc++;
        }
    }
    printf("clz all 2^32 inputs: %ld mismatches; negative control (no top-bit isolation) %ld differ (> 0)\n", bc, nc);
    fail |= bc || !nc;
    long bn = 0;
    for (int v = 0; v < 256; v++) {
        u128 a;
        memset(&a, v, 16);
        chk("cntb", spu_cntb(a), r_cntb(a), &bn);
    }
    for (long k = 0; k < (16l << 20); k++) {
        const u128 a = rv();
        chk("cntb", spu_cntb(a), r_cntb(a), &bn);
    }
    printf("cntb every byte value + 16 M random: %ld mismatches\n", bn);
    fail |= bn != 0;

    /* 3. gather bits, form select masks */
    long bg = 0;
    for (uint32_t m = 0; m < 65536; m++) { /* every LSB pattern over the 16 bytes / 8 halfwords / 4 words */
        u128 a = rv();
        for (int i = 0; i < 16; i++) a._u8[i] = (uint8_t)((a._u8[i] & 0xFE) | ((m >> i) & 1));
        chk("gb", spu_gb(a), r_gb(a), &bg);
        chk("gbh", spu_gbh(a), r_gbh(a), &bg);
        chk("gbb", spu_gbb(a), r_gbb(a), &bg);
        u128 f = rv();
        f._u32[0] = (f._u32[0] & ~0xFFFFu) | m;
        chk("fsm", spu_fsm(f), r_fsm(f), &bg);
        chk("fsmh", spu_fsmh(f), r_fsmh(f), &bg);
    }
    for (long k = 0; k < (16l << 20); k++) {
        const u128 a = rv();
        chk("gb", spu_gb(a), r_gb(a), &bg);
        chk("gbh", spu_gbh(a), r_gbh(a), &bg);
        chk("gbb", spu_gbb(a), r_gbb(a), &bg);
        chk("fsm", spu_fsm(a), r_fsm(a), &bg);
        chk("fsmh", spu_fsmh(a), r_fsmh(a), &bg);
    }
    long bl = 0;
    for (long k = 0; k < (16l << 20); k++) {
        const u128 a = rv(), b = rv(), c = rv();
        chk("and", spu_and(a, b), r_log(0, a, b), &bl);
        chk("or", spu_or(a, b), r_log(1, a, b), &bl);
        chk("xor", spu_xor(a, b), r_log(2, a, b), &bl);
        chk("nand", spu_nand(a, b), r_log(3, a, b), &bl);
        chk("nor", spu_nor(a, b), r_log(4, a, b), &bl);
        chk("andc", spu_andc(a, b), r_log(5, a, b), &bl);
        chk("orc", spu_orc(a, b), r_log(6, a, b), &bl);
        chk("selb", spu_selb(a, b, c), r_selb(a, b, c), &bl);
    }
    printf("and or xor nand nor andc orc selb: 16 M random: %ld mismatches\n", bl);
    fail |= bl != 0;
    printf("gb gbh gbb fsm fsmh: every 16-bit pattern + 16 M random: %ld mismatches\n", bg);
    fail |= bg != 0;

    /* 4. byte shifts / rotates, fast and KZ3_SPU_FASTCG=0 */
    long bq = 0;
    for (int slow = 0; slow <= 1; slow++) {
        g_kz3_spu_slowcg = slow;
        for (long k = 0; k < (1l << 20); k++) {
            const u128 a = rv();
            for (int c = 0; c < 128; c++) {
                u128 b = rv();
                b._u32[0] = (b._u32[0] & ~0x7Fu) | (uint32_t)c; /* shift count in the preferred word */
                u128 b3 = rv();
                b3._u32[0] = (b3._u32[0] & ~0x3F8u) | ((uint32_t)c << 3 & 0x3F8u);
                const int ms = (0 - c) & 0x1F, ms3 = (0 - ((int)b3._u32[0] >> 3)) & 0x1F;
                chk("shlqbyi", spu_shlqbyi(a, c), (c & 0x1F) >= 16 ? mk(0, 0, 0, 0) : r_shl(a, c & 0x1F), &bq);
                chk("rotqbyi", spu_rotqbyi(a, c), r_rot(a, c & 0x0F), &bq);
                chk("shlqby", spu_shlqby(a, b), (c & 0x1F) >= 16 ? mk(0, 0, 0, 0) : r_shl(a, c & 0x1F), &bq);
                chk("rotqby", spu_rotqby(a, b), r_rot(a, c & 0x0F), &bq);
                chk("shlqbybi", spu_shlqbybi(a, b3), (c & 0x1F) >= 16 ? mk(0, 0, 0, 0) : r_shl(a, c & 0x1F), &bq);
                chk("rotqbybi", spu_rotqbybi(a, b3), r_rot(a, c & 0x0F), &bq);
                chk("rotqmby", spu_rotqmby(a, b), ms >= 16 ? mk(0, 0, 0, 0) : r_rotm(a, ms), &bq);
                chk("rotqmbybi", spu_rotqmbybi(a, b3), ms3 >= 16 ? mk(0, 0, 0, 0) : r_rotm(a, ms3), &bq);
                chk("rotqmbyi", spu_rotqmbyi(a, c), ms >= 16 ? mk(0, 0, 0, 0) : r_rotm(a, ms), &bq);
            }
            if (k == (1l << 14) && slow) break; /* the slow loop: 16 K vectors are enough */
        }
    }
    g_kz3_spu_slowcg = 0;
    printf("quadword byte shifts/rotates, every count 0..127, fast (1 M vectors) and FASTCG=0: %ld mismatches\n", bq);
    fail |= bq != 0;

    /* 5. fi */
    long bf = 0, bfi = 0;
#if defined(__SSE4_1__)
    for (long k = 0; k < (64l << 20); k++) {
        const uint32_t e = (uint32_t)(k & 0xFF);
        u128 a = rv(), b = rv();
        for (int i = 0; i < 4; i++) b._u32[i] = (b._u32[i] & 0x807FFFFFu) | (((e + (uint32_t)i) & 0xFF) << 23);
        if (k % 7 == 0) b._u32[k & 3] |= 0x3FFu;
        __m128i A, B;
        memcpy(&A, &a, 16);
        memcpy(&B, &b, 16);
        const __m128i E = _mm_and_si128(_mm_srli_epi32(B, 23), _mm_set1_epi32(0xFF));
        const __m128i R = spu__fi_int4(A, B, E);
        u128 r;
        memcpy(&r, &R, 16);
        for (int i = 0; i < 4; i++)
            if (r._u32[i] != spu__fi_lane(a._u32[i], b._u32[i])) bfi++;
    }
    printf("fi integer path (spu__fi_int4 vs spu__fi_lane), 64 M vectors, every exponent: %ld mismatches\n", bfi);
    for (int xr = 0; xr <= 1; xr++) {
        g_kz3_spu_xr = xr;
        for (long k = 0; k < (16l << 20); k++) {
            u128 a = rv(), b = rv();
            const int l = (int)(k & 3);
            if (k & 4) b._u32[l] |= 0x7F800000u; /* exponent 255 in one lane of half the vectors */
            chk("fi", spu_fi(a, b), spu__fi_ref(a, b), &bf);
        }
    }
    g_kz3_spu_xr = 0;
    printf(
        "spu_fi vs the 1131 loop (recompute off and on, half the vectors with an exponent-255 lane): %ld mismatches\n",
        bf);
#else
    printf("fi: no SSE4.1 vector path in this build (the loop is spu_fi itself)\n");
#endif
    fail |= bf || bfi;
    (void)nbad;
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
