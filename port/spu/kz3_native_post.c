/* kz3_native_post.c -- Gfx.Post kernels as native host code (SPU pass 122, D88-). Default off:
 * KZ3_NATIVE_JOB_POSTMIX=1 (the shared blend), KZ3_NATIVE_JOB_POST=1 (every Gfx.Post gate).
 *
 * The blend is the KZ2 kernel's algorithm (KZ2 port/kz2/spu/native_post.c kz2_post_blend, credited); KZ3's copy has a
 * different schedule (software-pipelined over 4 pixels, one quadword a step) and was re-derived from the KZ3
 * disassembly, then checked against the lifted code by tools/spu/native_diff.py.
 *
 * Every kernel keeps r0, r1 and r80-r127 and leaves r2-r79 as they were at the gate: the call sites read none of
 * them afterwards (job_011f9c80/job_011f7d00 0x4998 -> br 0x432C, job_011d4680 0x4998 -> br 0x432C: r3, r5, r6 and
 * r2 are loaded again there; checked by reading the code at the return point).
 */
#include "kz3_native_jobs.h"

#include <smmintrin.h>
#include <string.h>

#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

/* KZ3 @0x4D00 in job_011f9c80 / job_011f7d00 (DepthOfField, MotionBlur; 1.00 twins job_0117d880 / job_0117b900) and
 * @0x4DD0 in job_011d4680 (Upsample; 1.00 job_01158280): r3 = RGBA8 pixels, blended in place; r4 = RGBA8 pixels
 * underneath; r5 = pixel count, a multiple of 4 (one quadword a step; any other count never reaches 0 in the SPU loop,
 * and 0 would run 2^30 steps). Per pixel, with a0 the first byte (coverage) of the r3 pixel:
 *   out_j = min(a_j + ((b_j * (256 - a0)) >> 8), 255)
 * (mpy signed 16x16, rotmai 8, a, cgt + selb: all values small and positive). The loop is software-pipelined: it reads
 * one quadword past each buffer (not stored, not needed here). SSE form: 16-bit lanes, packus saturates to 255. */
int kz3_post_blend(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t a = ctx->gpr[3]._u32[0], b = ctx->gpr[4]._u32[0];
    const uint32_t n = ctx->gpr[5]._u32[0];
    if (n == 0 || n % 4 || n > 16384 || ((a | b) & 15) || overlaps(a, 4 * n, b, 4 * n + 16) ||
        a + 4 * n > SPU_LS_SIZE || b + 4 * n + 16 > SPU_LS_SIZE)
        return 0;
    const __m128i z = _mm_setzero_si128(), k256 = _mm_set1_epi16(256);
    const __m128i a3lo = _mm_set_epi8(-1, 4, -1, 4, -1, 4, -1, 4, -1, 0, -1, 0, -1, 0, -1, 0);
    const __m128i a3hi = _mm_set_epi8(-1, 12, -1, 12, -1, 12, -1, 12, -1, 8, -1, 8, -1, 8, -1, 8);
    for (uint32_t q = 0; q < n / 4; q++, a += 16, b += 16) {
        uint8_t* pa = ls + a;
        const __m128i va = _mm_loadu_si128((const __m128i*)pa);
        const __m128i vb = _mm_loadu_si128((const __m128i*)(ls + b));
        const __m128i klo = _mm_sub_epi16(k256, _mm_shuffle_epi8(va, a3lo));
        const __m128i khi = _mm_sub_epi16(k256, _mm_shuffle_epi8(va, a3hi));
        const __m128i lo = _mm_add_epi16(_mm_unpacklo_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(vb, z), klo), 8));
        const __m128i hi = _mm_add_epi16(_mm_unpackhi_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(vb, z), khi), 8));
        _mm_storeu_si128((__m128i*)pa, _mm_packus_epi16(lo, hi));
    }
    return 1;
}

#define MIX_ENV "KZ3_NATIVE_JOB_POSTMIX"
#define POOL_BLEND_DOF 0x5AE0u, 0x5BB0u, 0xb65df4abc0d86d6full
#define POOL_BLEND_MB 0x5C50u, 0x5D20u, 0xb65df4abc0d86d6full
#define POOL_BLEND_UP 0x5A70u, 0x5B00u, 0x7ebb5d25c354924full

/* Gates (re/spu/native_gates.tsv; pools from tools/spu/native_pool.py): slot, flags, job, image, LS address, kernel,
 * family, constant pool [lo, hi) and its hash. 1.00 twins: the modules are byte-identical (the RE notes
 * s.184, spu_map_114.tsv). */
const kz3_native_entry kz3_native_post[] = {
    { 0, MIX_ENV, "Gfx.Post.DepthOfField", "job_011f9c80", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_DOF },
    { 1, MIX_ENV, "Gfx.Post.DepthOfField", "job_0117d880", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_DOF },
    { 2, MIX_ENV, "Gfx.Post.MotionBlur", "job_011f7d00", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_MB },
    { 3, MIX_ENV, "Gfx.Post.MotionBlur", "job_0117b900", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_MB },
    { 4, MIX_ENV, "Gfx.Post.Upsample", "job_011d4680", 0x4DD0u, kz3_post_blend, "blend", POOL_BLEND_UP },
    { 5, MIX_ENV, "Gfx.Post.Upsample", "job_01158280", 0x4DD0u, kz3_post_blend, "blend", POOL_BLEND_UP },
};
const unsigned kz3_native_post_n = sizeof kz3_native_post / sizeof kz3_native_post[0];
