/* kz3_native_dof.c -- Gfx.Post.DepthOfField (job_011f9c80, ovl242; 1.00 twin job_0117d880) kernels as native host
 * code (SPU pass 122). Default off: KZ3_NATIVE_JOB_DOF=1 (or KZ3_NATIVE_JOB_POST=1).
 *
 * The unpack kernel @0x5780 is KZ2's DepthOfField unpack (KZ2 port/kz2/spu/native_dof.c dof_unpack_5F80, credited)
 * in KZ3's schedule: 4 pixels a step (one quadword of pixels and one of weights, four float4 out), r6 counts pixels.
 * Bit-exact with the lifted code by construction (every float op is a plain SSE mul of values far from exponent 255)
 * and checked by tools/spu/native_diff.py on synthetic and recorded states. The gather @0x51F0 is not native yet.
 * Registers: the kernel leaves r2-r79 as they were (call site 0x4C54 reads none: r4, r2-r6, r8, r9 are loaded again at
 * 0x4C58) and keeps r0, r1, r80-r127.
 */
#include "kz3_native_jobs.h"

#include <smmintrin.h>
#include <string.h>

#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

#define K_255 0x3F808081u   /* 256/255 (1.0039216): x/256 * 256/255 = x/255 */

static inline __m128 f4(uint32_t bits)
{
    return _mm_castsi128_ps(_mm_set1_epi32((int)bits));
}

static inline __m128 lane_splat(__m128 v, int p)
{
    switch (p & 3) {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

/* @0x5780: r3 = float4 destination, r4 = pixels (4 bytes each: M, c1, c2, c3), r5 = weights (the top byte of each
 * word, one per pixel), r6 = pixel count (a multiple of 4: the SPU loop steps 4 and ends at 0). Per pixel:
 *   x = {255, c1, c2, c3};  v = x + ((M * x) >> 4);  out = (v/256 * 256/255) * (w/256 * 256/255)
 * an RGBM-style decode times the pixel's weight, in the SPU's own order (mpyu, rotmi 4, a, cuflt 147, fm, fm). v <= 4334
 * and every product is a small finite positive value, so plain SSE equals the helpers. The loop is software-pipelined
 * and reads one quadword past each input (not needed here). */
static int dof_unpack_5780(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t dst = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], wts = ctx->gpr[5]._u32[0];
    const uint32_t n = ctx->gpr[6]._u32[0];
    if (n == 0 || n % 4 || n > 16384 || ((dst | src | wts) & 15) || dst + 16 * n > SPU_LS_SIZE ||
        src + 4 * n + 16 > SPU_LS_SIZE || wts + 4 * n + 16 > SPU_LS_SIZE ||
        overlaps(dst, 16 * n, src, 4 * n + 16) || overlaps(dst, 16 * n, wts, 4 * n + 16))
        return 0;
    const __m128 k = f4(K_255), s8 = f4(0x3B800000u);   /* 2^-8 */
    const __m128i c255 = _mm_set_epi32(0, 0, 0, 255);
    const __m128i bs = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    for (uint32_t q = 0; q < n / 4; q++, src += 16, wts += 16, dst += 64) {
        const __m128i a = _mm_loadu_si128((const __m128i*)(ls + src));
        const __m128i b = _mm_loadu_si128((const __m128i*)(ls + wts));
        const __m128i wb = _mm_shuffle_epi8(b, _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0));
        const __m128 w = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(wb), s8), k);
        for (int p = 0; p < 4; p++) {
            const char o = (char)(4 * p);
            const __m128i x = _mm_or_si128(c255, _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, (char)(o + 3), -1, -1, -1,
                                                                                   (char)(o + 2), -1, -1, -1, (char)(o + 1),
                                                                                   -1, -1, -1, -1)));
            const __m128i m = _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o));
            const __m128i v = _mm_add_epi32(x, _mm_srli_epi32(_mm_mullo_epi16(m, x), 4));
            const __m128 f = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(v), s8), k);
            _mm_storeu_si128((__m128i*)(ls + dst + 16 * p), _mm_shuffle_epi8(_mm_castps_si128(_mm_mul_ps(f, lane_splat(w, p))), bs));
        }
    }
    return 1;
}

#define DOF_ENV "KZ3_NATIVE_JOB_DOF"
#define POOL_UNPACK 0x5B60u, 0x6100u, 0x3a429ca7f6e8972eull

const kz3_native_entry kz3_native_dof[] = {
    { 6, DOF_ENV, "Gfx.Post.DepthOfField", "job_011f9c80", 0x5780u, dof_unpack_5780, "dof_unpack", POOL_UNPACK },
    { 7, DOF_ENV, "Gfx.Post.DepthOfField", "job_0117d880", 0x5780u, dof_unpack_5780, "dof_unpack", POOL_UNPACK },
};
const unsigned kz3_native_dof_n = sizeof kz3_native_dof / sizeof kz3_native_dof[0];
