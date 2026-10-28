/* Patch 1105 host test: spu_fa/spu_fs/spu_fm take their exponent-255 lane mask from the result alone
 * (SPU__XRM_AR = spu__xr_mask(r)) instead of spu__xr_mask3(a, b, r). Checks, for every MXCSR rounding mode with
 * FTZ and DAZ on and off, that the two masks are equal and that the new ops give the same bits as the old form
 * (the 1079/1089 body with spu__xr_mask3, rebuilt here) on edge words (zero, denormals, the smallest normal binade,
 * Smax, infinity/NaN encodings, the K4 sentinel words) and random words. Negative control: the operand-only mask
 * (mask(a) | mask(b)) must miss lanes (overflowing products). Built by tools/spu/test_spu_rmask.sh against
 * work/ps3recomp-patched (no game data). */
#include <stdint.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

int g_kz3_spu_slowcg = 0;
int g_kz3_fma_stats;
int g_kz3_xr_stats;
int g_kz3_spu_xrq = 1;
int g_kz3_spu_xr = 1;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra) { (void)op; (void)m; (void)a; (void)b; (void)c; (void)r; (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s; (void)ftz; }

#ifndef SPU__XRM_AR
int main(void) { puts("SKIP: patch 1105 not in the patched toolkit (no SPU__XRM_AR)"); return 0; }
#else
/* The pre-1105 bodies (patch 1079 + 1089). */
static __attribute__((noinline)) u128 old_fa(u128 a, u128 b) { u128 r; SPU__F2(r, a, b, _mm_add_ps, a._f32[i] + b._f32[i]); SPU_XR_RET_Q(spu__xr_mask3(a, b, r), r, a, b, SPU_XR_FA); }
static __attribute__((noinline)) u128 old_fs(u128 a, u128 b) { u128 r; SPU__F2(r, a, b, _mm_sub_ps, a._f32[i] - b._f32[i]); SPU_XR_RET_Q(spu__xr_mask3(a, b, r), r, a, b, SPU_XR_FS); }
static __attribute__((noinline)) u128 old_fm(u128 a, u128 b) { u128 r; SPU__F2(r, a, b, _mm_mul_ps, a._f32[i] * b._f32[i]); SPU_XR_RET_Q(spu__xr_mask3(a, b, r), r, a, b, SPU_XR_FM); }
static __attribute__((noinline)) u128 new_fa(u128 a, u128 b) { return spu_fa(a, b); }
static __attribute__((noinline)) u128 new_fs(u128 a, u128 b) { return spu_fs(a, b); }
static __attribute__((noinline)) u128 new_fm(u128 a, u128 b) { return spu_fm(a, b); }

static const uint32_t edge[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x00FFFFFF,
    0x01000000, 0x3F800000, 0xBF800000, 0x3F800001, 0x4B800000, 0x7F000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000,
    0xFF800000, 0x7F800001, 0x7FC00000, 0xFFC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFC00, 0x7FB50000, 0x7FB17215,
    0x5F800000, 0x5F7FFFFF, 0x20000000, 0x1F800000, 0x1FFFFFFF, 0xA0000000, 0x3F000000, 0x34000000, 0xB4000000};
#define NE ((int)(sizeof edge / sizeof edge[0]))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
/* Mostly uniform words; one lane in four drawn from the edge list, one in eight with exponent 254/255/0/1. */
static uint32_t word(void) {
    const uint32_t k = rnd() & 7, w = rnd();
    if (k < 2) return edge[w % NE];
    if (k == 2) { static const uint32_t ex[] = {0, 1, 254, 255, 127, 128}; return (w & 0x807FFFFFu) | ex[w % 6] << 23; }
    return w;
}

static long bad_mask, bad_val, neg_miss, lanes;
static void check(u128 a, u128 b, const char* mode) {
    const u128 rs3[3] = {old_fa(a, b), old_fs(a, b), old_fm(a, b)};
    const u128 rn3[3] = {new_fa(a, b), new_fs(a, b), new_fm(a, b)};
    for (int op = 0; op < 3; op++) {
        u128 r;
        __m128 A, B; memcpy(&A, &a, 16); memcpy(&B, &b, 16);
        const __m128 R = op == 0 ? _mm_add_ps(A, B) : op == 1 ? _mm_sub_ps(A, B) : _mm_mul_ps(A, B);
        memcpy(&r, &R, 16);
        const unsigned m3 = spu__xr_mask3(a, b, r), mr = SPU__XRM_AR(a, b, r), mab = spu__xr_mask(a) | spu__xr_mask(b);
        if (m3 != mr && bad_mask++ < 5)
            printf("FAIL mask op %d %s: %08x %08x %08x %08x / %08x %08x %08x %08x: %x vs %x\n", op, mode, a._u32[0],
                   a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], m3, mr);
        if (mab != m3) neg_miss++;
        if (memcmp(&rs3[op], &rn3[op], 16) && bad_val++ < 5)
            printf("FAIL value op %d %s: %08x x %08x -> old %08x new %08x\n", op, mode, a._u32[0], b._u32[0],
                   rs3[op]._u32[0], rn3[op]._u32[0]);
    }
    lanes += 12;
}

int main(void) {
    const unsigned csr0 = _mm_getcsr();
    static const char* rname[] = {"RN", "RD", "RU", "RZ"};
    for (int rm = 0; rm < 4; rm++)
        for (int fd = 0; fd < 4; fd++) {
            char mode[32]; snprintf(mode, sizeof mode, "%s%s%s", rname[rm], fd & 1 ? "+FTZ" : "", fd & 2 ? "+DAZ" : "");
            _mm_setcsr((csr0 & ~0xE040u) | (unsigned)rm << 13 | (fd & 1 ? 0x8000u : 0) | (fd & 2 ? 0x40u : 0));
            for (int i = 0; i < NE; i++)          /* every edge pair, lane 0; lanes 1-3 rotate through the list */
                for (int j = 0; j < NE; j++) {
                    u128 a, b;
                    for (int l = 0; l < 4; l++) { a._u32[l] = edge[(i + 7 * l) % NE]; b._u32[l] = edge[(j + 11 * l) % NE]; }
                    check(a, b, mode);
                }
            for (int n = 0; n < 250000; n++) {
                u128 a, b;
                for (int l = 0; l < 4; l++) { a._u32[l] = word(); b._u32[l] = word(); }
                check(a, b, mode);
            }
        }
    _mm_setcsr(csr0);
    printf("test_spu_rmask: %ld lanes (16 MXCSR modes), mask differs %ld, value differs %ld; negative control "
           "(operand-only mask) differs in %ld vectors\n", lanes, bad_mask, bad_val, neg_miss);
    if (neg_miss == 0) { puts("FAIL: negative control found no difference (test does not exercise overflow)"); return 1; }
    if (bad_mask || bad_val) { puts("FAIL"); return 1; }
    puts("PASS");
    return 0;
}
#endif
