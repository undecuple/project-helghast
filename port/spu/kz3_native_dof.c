/* kz3_native_dof.c -- Gfx.Post.DepthOfField (job_011f9c80, ovl242; 1.00 twin job_0117d880) kernels as native host
 * code (SPU pass 122). Default off: KZ3_NATIVE_JOB_DOF=1 (or KZ3_NATIVE_JOB_POST=1).
 *
 * The unpack kernel @0x5780 is KZ2's DepthOfField unpack (KZ2 port/kz2/spu/native_dof.c dof_unpack_5F80, credited)
 * in KZ3's schedule: 4 pixels a step (one quadword of pixels and one of weights, four float4 out), r6 counts pixels.
 * Bit-exact with the lifted code by construction (every float op is a plain SSE mul of values far from exponent 255)
 * and checked by tools/spu/native_diff.py on synthetic and recorded states. The gather @0x51F0 (pass 135) follows
 * below; KZ3_NATIVE_JOB_DOFGATHER=1 (or KZ3_NATIVE_JOB_POST=1) turns it on, default off until its toggle A/B.
 * Registers: the kernel leaves r2-r79 as they were (call site 0x4C54 reads none: r4, r2-r6, r8, r9 are loaded again at
 * 0x4C58) and keeps r0, r1, r80-r127.
 */
#include "kz3_native_jobs.h"
#include "spu_helpers.h"

#include <smmintrin.h>
#include <string.h>

#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

#define K_255 0x3F808081u /* 256/255 (1.0039216): x/256 * 256/255 = x/255 */

static inline __m128 f4(uint32_t bits)
{
    return _mm_castsi128_ps(_mm_set1_epi32((int)bits));
}

static inline __m128 lane_splat(__m128 v, int p)
{
    switch (p & 3) {
    case 0:
        return _mm_shuffle_ps(v, v, 0x00);
    case 1:
        return _mm_shuffle_ps(v, v, 0x55);
    case 2:
        return _mm_shuffle_ps(v, v, 0xAA);
    default:
        return _mm_shuffle_ps(v, v, 0xFF);
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
        src + 4 * n + 16 > SPU_LS_SIZE || wts + 4 * n + 16 > SPU_LS_SIZE || overlaps(dst, 16 * n, src, 4 * n + 16) ||
        overlaps(dst, 16 * n, wts, 4 * n + 16))
        return 0;
    const __m128 k = f4(K_255), s8 = f4(0x3B800000u); /* 2^-8 */
    const __m128i c255 = _mm_set_epi32(0, 0, 0, 255);
    const __m128i bs = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    for (uint32_t q = 0; q < n / 4; q++, src += 16, wts += 16, dst += 64) {
        const __m128i a = _mm_loadu_si128((const __m128i*)(ls + src));
        const __m128i b = _mm_loadu_si128((const __m128i*)(ls + wts));
        const __m128i wb =
            _mm_shuffle_epi8(b, _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0));
        const __m128 w = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(wb), s8), k);
        for (int p = 0; p < 4; p++) {
            const char o = (char)(4 * p);
            const __m128i x = _mm_or_si128(
                c255, _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, (char)(o + 3), -1, -1, -1, (char)(o + 2), -1, -1, -1,
                                                       (char)(o + 1), -1, -1, -1, -1)));
            const __m128i m =
                _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o));
            const __m128i v = _mm_add_epi32(x, _mm_srli_epi32(_mm_mullo_epi16(m, x), 4));
            const __m128 f = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(v), s8), k);
            _mm_storeu_si128((__m128i*)(ls + dst + 16 * p),
                             _mm_shuffle_epi8(_mm_castps_si128(_mm_mul_ps(f, lane_splat(w, p))), bs));
        }
    }
    return 1;
}

/* ---- gather @0x51F0 (SPU pass 135; plan: the RE notes s.2) --------------------------------------
 * dof_gather_51F0(r3 = dst RGBA8 row, r4 = float4 scratch of this row's pixel 0, r5 = CoC stream (byte 0 of each
 * word), r6 = float4 row buffer (written by phase 1, read back by phase 2), r7 = scratch row pitch in bytes (the SPU
 * uses its low halfword as a signed 16-bit multiplier), r8 = n pixels, a multiple of 4). The driver 0x4BA0 calls it
 * once per output row (0x4C90: r6 = 0x62C0, r7 = 16*Wp, r8 = W).
 * Phase 1 (0x5248..0x5570, loop 0x5390), per group of 4 pixels: c_p = stream word p >> 24; when all four are 0 the
 * centre quads pass through fma(x, 0x24-as-float, x) (0x5558; the denormal factor flushes to zero, so x) and are
 * stored; else each pixel's accumulator starts at its centre float4 and adds (fa, truncating) the 36 taps of the
 * disc in the SPU's order: groups of 6 (table rows 6g..6g+5), descending inside a group. Tap k of pixel p reads the
 * quad at (r4 + 64 g + 16 p + 16 dx + dy * pitch) & ~15 with dx = (XT[k] * c_p) >> 16 and dy = (YT[k] * c_p) >> 16
 * (mpys: signed 16-bit halves, arithmetic shift), then mpya with 16 and the lane quad 0x6060 = 0,16,32,48. The
 * tables live at LS 0x5BD0 (X) and 0x5E10 (Y) as splat quads; the pool hash pins them.
 * Phase 2 (0x5570..0x5668, loop 0x5668), per pixel: a = min(1, fm(fm(cuflt(c, 147), 256/255), 16)) (fcgt + selb),
 * rec = fi(S0, frest(S0)) with S0 = channel 0 of the row-buffer quad S, t = fm(fm(fm(S, rec), a), 255/256),
 * u = cfltu(t, 165), u = (signed) u > 255 ? 255 : u, dst byte 4 p + c = low byte of u.word c (shufb 0x5BA0/0x5AE0 +
 * or). Every float op goes through the runtime's own helpers (the lifted code's), so the bits are the SPU's by
 * construction; tools/spu/native_diff.py (kind dof_gather) is the proof. Registers: the lifted 0x51F0..0x5668 write
 * r2..r79 only (every `gpr[N] =` in the lift); the gate's return restores nothing else. Reads past the ends (the
 * software pipeline prefetches one stream quad and one row-buffer group) are reads only and are not repeated here. */
static inline u128 ldq(const uint8_t* ls, uint32_t a)
{
    u128 r;
    __m128i v = _mm_loadu_si128((const __m128i*)(ls + LSQ(a)));
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    memcpy(&r, &v, 16);
    return r;
}

static inline void stq(uint8_t* ls, uint32_t a, u128 r)
{
    __m128i v;
    memcpy(&v, &r, 16);
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    _mm_storeu_si128((__m128i*)(ls + LSQ(a)), v);
}

/* LS 0x5BD0 / 0x5E10: the 37-point disc minus its centre, 1/512 pixel units (c = 255 reaches dx, dy in -6..+5) */
static const int16_t dof_xt[36] = {-1536, -1536, -1536, -1024, -1024, -1024, -1024, -1024, -512, -512, -512, -512,
                                   -512,  -512,  -512,  0,     0,     0,     0,     0,     0,    512,  512,  512,
                                   512,   512,   512,   512,   1024,  1024,  1024,  1024,  1024, 1536, 1536, 1536};
static const int16_t dof_yt[36] = {-512,  0,     512,   -1024, -512,  0,     512,   1024,  -1536, -1024, -512, 0,
                                   512,   1024,  1536,  -1536, -1024, -512,  512,   1024,  1536,  -1536, -1024, -512,
                                   0,     512,   1024,  1536,  -1024, -512,  0,     512,   1024,  -512,  0,    512};

static int dof_gather_51F0(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t dst = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], strm = ctx->gpr[5]._u32[0],
                   row = ctx->gpr[6]._u32[0], n = ctx->gpr[8]._u32[0];
    const int32_t pitch = (int16_t)ctx->gpr[7]._u32[0];
    if (n == 0 || n % 4 || n > 16384 || ((dst | src | strm | row) & 15) || dst + 4 * n > SPU_LS_SIZE ||
        row + 16 * n > SPU_LS_SIZE || strm + 4 * n > SPU_LS_SIZE || overlaps(dst, 4 * n, row, 16 * n + 64) ||
        overlaps(dst, 4 * n, strm, 4 * n + 32))
        return 0;
    const u128 k24 = spu_splat_u32(0x24u);
    for (uint32_t g = 0; g < n / 4; g++) {
        const uint32_t base = src + 64 * g;
        const u128 sw = ldq(ls, strm + 16 * g);
        u128 acc[4];
        for (int p = 0; p < 4; p++) acc[p] = ldq(ls, base + 16 * p);
        if (((sw._u32[0] | sw._u32[1] | sw._u32[2] | sw._u32[3]) >> 24) == 0) {
            for (int p = 0; p < 4; p++) acc[p] = spu_fma(acc[p], k24, acc[p]);
        } else {
            /* SPU pass 140: tap-major, the 4 pixels' independent fa chains interleaved (each chain is 36 dependent adds;
             * pixel-major left the core waiting on one chain at a time, as the SPU's software pipeline does not).
             * Per pixel the adds are in the same order, so the bits are unchanged. */
            int32_t cp[4];
            for (int p = 0; p < 4; p++) cp[p] = (int32_t)(sw._u32[p] >> 24);
#pragma GCC unroll 36
            for (int k = 0; k < 36; k++) {
#ifdef KZ3_NATIVE_NEGCTL
                const int kk = k;   /* negative control for the tests: ascending tap order (fa is not associative) */
#else
                const int kk = (k / 6) * 6 + 5 - k % 6;
#endif
#pragma GCC unroll 4
                for (int p = 0; p < 4; p++) {
                    const int32_t dx = (dof_xt[kk] * cp[p]) >> 16, dy = (dof_yt[kk] * cp[p]) >> 16;
                    const uint32_t off = (uint32_t)(dx * 16 + 16 * p) + (uint32_t)(dy * pitch);
                    acc[p] = spu_fa(acc[p], ldq(ls, base + off));
                }
            }
        }
        for (int p = 0; p < 4; p++) stq(ls, row + 64 * g + 16 * p, acc[p]);
    }
    const u128 k255 = spu_splat_u32(K_255), k16 = spu_splat_u32(0x41800000u), one = spu_splat_u32(0x3F800000u),
               k255_256 = spu_splat_u32(0x3F7F0000u), i255 = spu_splat_u32(255);
    for (uint32_t g = 0; g < n / 4; g++) {
        const u128 sw = ldq(ls, strm + 16 * g);
        u128 cq;
        for (int p = 0; p < 4; p++) cq._u32[p] = sw._u32[p] >> 24;
        u128 al = spu_fm(spu_fm(spu_cuflt(cq, 147), k255), k16);
        al = spu_selb(al, one, spu_fcgt(al, one));
        for (int p = 0; p < 4; p++) {
            const u128 S = ldq(ls, row + 64 * g + 16 * p);
            const u128 s0 = spu_splat_u32(S._u32[0]);
            const u128 rec = spu_fi(s0, spu_frest(s0));
            const u128 t = spu_fm(spu_fm(spu_fm(S, rec), spu_splat_u32(al._u32[p])), k255_256);
            u128 u = spu_cfltu(t, 165);
            u = spu_selb(u, i255, spu_cgt(u, i255));
            for (int c = 0; c < 4; c++) ls[dst + 16 * g + 4 * p + c] = (uint8_t)u._u32[c];
        }
    }
    return 1;
}

#define DOF_ENV "KZ3_NATIVE_JOB_DOF"
#define GATHER_ENV "KZ3_NATIVE_JOB_DOFGATHER"
#define POOL_UNPACK 0x5B60u, 0x6100u, 0x3a429ca7f6e8972eull
#define POOL_GATHER 0x5AE0u, 0x6100u, 0xa9f4e7c54f4f4083ull /* the plan's widened range; native_pool.py, pass 135 */

const kz3_native_entry kz3_native_dof[] = {
    {6, DOF_ENV, "Gfx.Post.DepthOfField", "job_011f9c80", 0x5780u, dof_unpack_5780, "dof_unpack", POOL_UNPACK, 1},
    {7, DOF_ENV, "Gfx.Post.DepthOfField", "job_0117d880", 0x5780u, dof_unpack_5780, "dof_unpack", POOL_UNPACK},
    {20, GATHER_ENV, "Gfx.Post.DepthOfField", "job_011f9c80", 0x51F0u, dof_gather_51F0, "dof_gather", POOL_GATHER},
    {21, GATHER_ENV, "Gfx.Post.DepthOfField", "job_0117d880", 0x51F0u, dof_gather_51F0, "dof_gather", POOL_GATHER},
};
const unsigned kz3_native_dof_n = sizeof kz3_native_dof / sizeof kz3_native_dof[0];
