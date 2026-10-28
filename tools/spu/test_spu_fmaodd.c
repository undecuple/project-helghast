/* Patch 1106 host test: the -mfma fast path's 4-op result test (SPU_FMA_ODD4=1) against the 7-op form (=0).
 * test_spu_fmaodd_tu.c is compiled twice (old_* and new_* wrappers); this file compares them:
 * 1. every 32-bit word w as the fused result (a = w, b = 1.0, c = +0 / -0, all four lanes different words),
 *    in the runtime's default MXCSR (round to nearest) with FTZ+DAZ and without;
 * 2. random and edge operands for fma/fms/fnms under all 4 rounding modes x FTZ x DAZ.
 * Negative control: a test with the zero-lane clear dropped (ptest(nn, nn)) must differ (checked in-file on the
 * predicate). Built by tools/spu/test_spu_fmaodd.sh against work/ps3recomp-patched (no game data). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
typedef union { uint32_t _u32[4]; float _f32[4]; uint64_t _u64[2]; } v128;   /* same layout as u128 */
#define DECL(p) v128 p##_fma(v128, v128, v128); v128 p##_fms(v128, v128, v128); v128 p##_fnms(v128, v128, v128);
DECL(old) DECL(new)

static const uint32_t edge[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00800000, 0x80800000, 0x00FFFFFF, 0x01000000, 0x01000001,
    0x3F800000, 0xBF800000, 0x3F800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF,
    0xFFFFFFFF, 0x7FFFFC00, 0x1F800000, 0x20000000, 0x5F800000, 0x5F7FFFFF, 0x34000000, 0xB4000000, 0x00400000};
#define NE ((int)(sizeof edge / sizeof edge[0]))
static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint32_t word(void) {
    const uint32_t k = rnd() & 7, w = rnd();
    if (k < 2) return edge[w % NE];
    if (k < 4) { static const uint32_t ex[] = {0, 1, 2, 125, 126, 127, 253, 254, 255}; return (w & 0x807FFFFFu) | ex[w % 9] << 23; }
    return w;
}
static long bad;
static void cmp3(v128 a, v128 b, v128 c, const char* what) {
    v128 o[3] = {old_fma(a, b, c), old_fms(a, b, c), old_fnms(a, b, c)};
    v128 n[3] = {new_fma(a, b, c), new_fms(a, b, c), new_fnms(a, b, c)};
    for (int k = 0; k < 3; k++)
        if (memcmp(&o[k], &n[k], 16) && bad++ < 8)
            printf("FAIL %s op %d: a %08x b %08x c %08x: old %08x new %08x\n", what, k, a._u32[0], b._u32[0], c._u32[0],
                   o[k]._u32[0], n[k]._u32[0]);
}
int main(void) {
    const unsigned csr0 = _mm_getcsr();
    long n1 = 0, n2 = 0;
    /* 1. all result words (only fma: fms/fnms of (w, 1, 0) are w and -w, which the sweep covers too) */
    for (int ftz = 0; ftz < 2; ftz++) {
        _mm_setcsr((csr0 & ~0xE040u) | (ftz ? 0x8040u : 0));
        v128 one, zero; for (int l = 0; l < 4; l++) { one._f32[l] = 1.0f; zero._u32[l] = (l & 1) ? 0x80000000u : 0; }
        for (uint64_t w = 0; w < (1ull << 32); w += 4) {
            v128 a; for (int l = 0; l < 4; l++) a._u32[l] = (uint32_t)(w + (uint64_t)l * 0x40000001ull);
            v128 o = old_fma(a, one, zero), nn = new_fma(a, one, zero);
            if (memcmp(&o, &nn, 16) && bad++ < 8) printf("FAIL sweep w %08x: old %08x new %08x\n", a._u32[0], o._u32[0], nn._u32[0]);
            n1 += 4;
        }
    }
    /* 2. random operands, all MXCSR modes */
    for (int m = 0; m < 16; m++) {
        _mm_setcsr((csr0 & ~0xE040u) | (unsigned)(m & 3) << 13 | (m & 4 ? 0x8000u : 0) | (m & 8 ? 0x40u : 0));
        for (int i = 0; i < 300000; i++) {
            v128 a, b, c;
            for (int l = 0; l < 4; l++) { a._u32[l] = word(); b._u32[l] = word(); c._u32[l] = word(); }
            cmp3(a, b, c, "random"); n2 += 12;
        }
    }
    _mm_setcsr(csr0);
    /* negative control on the predicate itself: without the zero clear, |r| = 0 counts as odd */
    const __m128i ab = _mm_setzero_si128();
    const __m128i nn = _mm_cmpgt_epi32(_mm_add_epi32(ab, _mm_set1_epi32(0x7F000000)), _mm_set1_epi32(-0x01800001));
    const int neg_ok = !_mm_testz_si128(nn, nn) && _mm_testz_si128(nn, ab);
    printf("test_spu_fmaodd: sweep %ld result lanes (2 MXCSR modes), random %ld lanes (16 modes): %ld differ; "
           "negative control %s\n", n1, n2, bad, neg_ok ? "ok" : "FAILED");
    if (bad || !neg_ok) { puts("FAIL"); return 1; }
    puts("PASS");
    return 0;
}
