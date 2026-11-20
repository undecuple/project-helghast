/* kz3_native_post.c -- Gfx.Post kernels as native host code (SPU pass 122). Default off:
 * KZ3_NATIVE_JOB_POSTMIX=1 (the shared blend), KZ3_NATIVE_JOB_UPSAMPLE=1 (the Upsample RGBA8 row kernel),
 * KZ3_NATIVE_JOB_MOTIONBLUR=1 (the MotionBlur blur row kernel, SPU pass 136), KZ3_NATIVE_JOB_POST=1 (every Gfx.Post
 * gate).
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
#include "spu_helpers.h"

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
        const __m128i lo =
            _mm_add_epi16(_mm_unpacklo_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(vb, z), klo), 8));
        const __m128i hi =
            _mm_add_epi16(_mm_unpackhi_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(vb, z), khi), 8));
        _mm_storeu_si128((__m128i*)pa, _mm_packus_epi16(lo, hi));
    }
    return 1;
}

/* KZ3 @0x52C0 in job_011d4680 (Upsample; 1.00 twin job_01158280): the RGBA8 2x bilinear row kernel.
 * r3 = dst, r4 = src (input row 0, first pixel), r5 = source pitch, r6 = destination pitch (bytes), r7 = Wn, the output
 * width in pixels (a multiple of 8, at least 8). Two output rows per call (dst, dst + r6), Wn * 4 bytes each. Reads the
 * source rows -1, 0, +1 at pixel columns -1 .. Wn/2: the framework has already filled the border rows and columns, so
 * nothing is clamped here. Per channel byte, with q = o >> 1, nb = q - 1 (o even) or q + 1 (o odd), ov = -1 (first
 * output row) or +1 (second):
 *   h(y) = 3 * X(y, q) + X(y, nb);   out = (3 * h(0) + h(ov)) >> 4
 * Weights 9/3/3/1 over 16, one floor at the end; every value stays below 4096, so 16-bit lanes are exact. The SPU loop
 * also reads up to 32 bytes past the right end (never used). Refused (the lifted code runs): count not a multiple of 8,
 * zero, unaligned pointers or pitches, regions leaving the local store, a destination overlapping its source. */
int kz3_post_upsample8(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t dst = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0];
    const uint32_t sp = ctx->gpr[5]._u32[0], dp = ctx->gpr[6]._u32[0], wn = ctx->gpr[7]._u32[0];
    if (wn == 0 || wn % 8 || wn > 2048 || ((dst | src | sp | dp) & 15) || sp == 0 || sp > 0x10000 || dp < 4 * wn ||
        dp > 0x10000)
        return 0;
    const uint32_t half = wn / 2;
    if (src < sp + 16) return 0;
    const uint32_t s_lo = src - sp - 16, s_hi = src + sp + 4 * half + 16, d_hi = dst + dp + 4 * wn;
    if (s_hi > SPU_LS_SIZE || d_hi > SPU_LS_SIZE || overlaps(dst, d_hi - dst, s_lo, s_hi - s_lo)) return 0;
    const __m128i z = _mm_setzero_si128(), k3 = _mm_set1_epi16(3);
    const uint8_t* r[3] = {ls + src - sp, ls + src, ls + src + sp};   /* source rows -1, 0, +1 */
    for (uint32_t q = 0; q < half; q++) {
        __m128i he[3], ho[3];   /* h for the even / odd output pixel of this source pixel, per source row */
        for (int i = 0; i < 3; i++) {
            int32_t m, c, n;
            memcpy(&m, r[i] + 4 * q - 4, 4);
            memcpy(&c, r[i] + 4 * q, 4);
            memcpy(&n, r[i] + 4 * q + 4, 4);
            const __m128i x3 = _mm_mullo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(c), z), k3);
            he[i] = _mm_add_epi16(x3, _mm_unpacklo_epi8(_mm_cvtsi32_si128(m), z));
            ho[i] = _mm_add_epi16(x3, _mm_unpacklo_epi8(_mm_cvtsi32_si128(n), z));
        }
        for (int oy = 0; oy < 2; oy++) {
            const int v = oy ? 2 : 0;   /* the vertical neighbour: row -1 for the first output row, +1 for the second */
            const __m128i e = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(he[1], k3), he[v]), 4);
            const __m128i o = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(ho[1], k3), ho[v]), 4);
            _mm_storel_epi64((__m128i*)(ls + dst + oy * dp + 8 * q), _mm_packus_epi16(_mm_unpacklo_epi64(e, o), z));
        }
    }
    return 1;
}

/* ---- MotionBlur blur row @0x5460 (SPU pass 136; plan the RE notes s.1, spec s.11.3) -------------
 * mb_blur_row_5460(r3 = dst RGBA8 row, r4 = float4 pixel 0 of this row (the K-M1 scratch, A border rows above and
 * below), r5 = velocity stream (4 bytes per pixel: b0 strength, b1 x, b2 y, b3 unused), r6 = float row pitch in bytes
 * (the SPU uses its low halfword as a signed 16-bit multiplier in mpya), r7 = n pixels, a multiple of 4). The driver
 * 0x4B88 calls it once per output row from 0x4C84 (link 0x4C88) with (dst + y*Wout*bpp, scratch + y*16*Wp,
 * vel + y*vpitch, 16*Wp, W); the lifted 0x5460 is the prologue and hops into the loop 0x5700..0x5A74 (one function
 * in the SPU: entered by the bisl at 0x4C84, returns by the bi $r0 at 0x5A78), so this gate at 0x5460 covers the
 * whole kernel. Per pixel (P = r4 + 16 p): vx = 16*b1 - 2048 (mpya b1, 16, -2048), vy = b2 - 128, the other side
 * sg = -1 as 0x10000 - v (sf), then 12 taps in this order: k = 0..5 the minus side with c = 3020 2520 1960 1400 840
 * 280, k = 6..11 the plus side, dx = (s16(sg*vx) * c) >> 16, dy = (s16(sg*vy) * c) >> 16 (mpys: low halves, arithmetic
 * shift), tap quad at (P + dy*pitch + dx) & ~15 (lqx). Sum per channel, inside out: S = T11 + T10, + T9, + T8, + T7,
 * S = fma(T6, 0.5, S), + T5 .. + T1, S = fma(T0, 0.5, S) (fa/fma truncating, the runtime's helpers); rec =
 * fi(S0, frest(S0)) (no Newton step); out byte c = low byte of cfltu(fm(fm(S, rec), cuflt(b0, 147)), 165), no clamp
 * (the lift has no cgt/selb). One 16-byte store per 4 pixels (stqd -16(r3) after r3 += 16). The lifted code writes
 * r2..r79 only (every `gpr[N] =` in the lift of 0x5460 and 0x5700); its velocity prefetch (one quad past the end)
 * and the tap reads outside the padded rows are reads only. Pool 0x5C50..0x5DF0 (native_pool.py 011f7d00 0x5460:
 * the shufb masks 0x5DB0/0x5DD0/0x5DE0 = bytes 0,4,8,12 / 2,6,10,14 / 1,5,9,13 zero-extended, 0x5CD0..0x5D00 =
 * splat word 3/2/1/0, 0x5D10/0x5C50 = low bytes of pixels 0,1 / 2,3). Bit-exact by construction through the
 * helpers; tools/spu/native_diff.py (kind mb_blur) is the proof. Guards (-> 0, the lifted code runs): n == 0 or not
 * a multiple of 4 (the SPU loop would not end), unaligned r3-r5, dst or the velocity stream past the LS, the tap
 * window (rows -6..+5, pixels -6..+5 of the row) outside the LS or under the destination, the stream under the
 * destination. */
static inline u128 mb_ldq(const uint8_t* ls, uint32_t a)
{
    u128 r;
    __m128i v = _mm_loadu_si128((const __m128i*)(ls + LSQ(a)));
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    memcpy(&r, &v, 16);
    return r;
}

/* SPU pass 161: the reciprocal estimate pair of one pixel on a scalar. rec = fi(S0, frest(S0)) is the same value in all
 * four lanes (S0 is splatted), so the four-lane helpers (spu_frest + spu_fi: two table gathers, two double conversions
 * per lane, ~9 ns/px of 33 in the host timing) did one lane's work four times. frest: sign | (253 - e) << 23 | the 5-bit
 * table fraction, e == 0 -> sign | 0x7FFFFFFF; fi: the SPU's base - step * y in integers (spu__fi_lane's math; patch 1134
 * proved it equals the float form for every exponent). When the estimate has exponent 255 (S0 zero or denormal) the
 * helpers run as before. Equal to spu_fi(s0, spu_frest(s0)) lane 0 for 10^8 random and the edge inputs
 * (tools/spu/test_r161_mb_fast.sh). */
static inline uint32_t mb_rec_scalar(uint32_t ra, int* slow)
{
    const uint32_t sg = ra & 0x80000000u, e0 = (ra >> 23) & 0xFFu;
    if (e0 == 0u) { *slow = 1; return 0; }
    const uint32_t re = e0 >= 253u ? 0u : 253u - e0;
    const uint32_t rb = sg | (re << 23) | (spu_frest_fraction_lut[(ra >> 18) & 0x1Fu] & 0x7FFFFFu);
    const uint32_t e = (rb >> 23) & 0xFFu;
    if (e == 255u) { *slow = 1; return 0; }
    /* fi on one lane, the helper's float form (patch 1131/1035): base = rb & ~0x3FF, step = the 10-bit field times
     * 2^(e - 140) in double rounded once to float (negated when base < 0), y = (ra & 0x7FFFF) * 2^-19, base - step * y
     * with separate multiply and subtract (no contraction: intrinsics, as the helper). */
    const uint32_t base_bits = rb & 0xFFFFFC00u;
    float base;
    memcpy(&base, &base_bits, 4);
    const uint64_t sc_bits = (uint64_t)(1023 + (int)e - 140) << 52;
    double sc;
    memcpy(&sc, &sc_bits, 8);
    float step = (float)((double)(rb & 0x3FFu) * sc);
    if (base < 0.0f) step = -step;
    const float y = (float)(ra & 0x7FFFFu) * 0x1p-19f;
    const __m128 r = _mm_sub_ss(_mm_set_ss(base), _mm_mul_ss(_mm_set_ss(step), _mm_set_ss(y)));
    float rf = _mm_cvtss_f32(r);
    uint32_t rw;
    memcpy(&rw, &rf, 4);
    return rw;
}

/* SPU pass 161: byte 0 of cfltu(q, 165) (u32 of q * 256, saturating at 0 below and 2^32 - 1 above) for four lanes when
 * no lane has exponent 255 and every q * 256 is below 2^31: q * 256 is an exact power-of-two scaling, cvttps truncates
 * toward zero like the helper's floor of the clamped double, negatives clamp to 0. Returns 0 (caller runs the helper)
 * otherwise. */
static inline int mb_cflt_bytes(__m128 q, __m128i* bytes)
{
    const __m128i qi = _mm_castps_si128(q);
    const __m128i x255 = _mm_cmpeq_epi32(_mm_and_si128(_mm_srli_epi32(qi, 23), _mm_set1_epi32(0xFF)), _mm_set1_epi32(0xFF));
    const __m128 t = _mm_max_ps(_mm_mul_ps(q, _mm_set1_ps(256.0f)), _mm_setzero_ps());
    const __m128i big = _mm_castps_si128(_mm_cmpge_ps(t, _mm_set1_ps(2147483648.0f)));
    if (__builtin_expect(!_mm_testz_si128(_mm_or_si128(x255, big), _mm_or_si128(x255, big)), 0)) return 0;
    *bytes = _mm_cvttps_epi32(t);
    return 1;
}

static const int32_t mb_c[6] = {3020, 2520, 1960, 1400, 840, 280}; /* il immediates at 0x5460.. */

static int mb_blur_row_5460(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t dst = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], vel = ctx->gpr[5]._u32[0],
                   n = ctx->gpr[7]._u32[0];
    const int32_t pitch = (int16_t)ctx->gpr[6]._u32[0];
    const int32_t ap = pitch < 0 ? -pitch : pitch;
    const int64_t rlo = (int64_t)src - 6 * ap - 96, rhi = (int64_t)src + 16 * (int64_t)n + 6 * ap + 96;
    if (n == 0 || n % 4 || n > 16384 || ((dst | src | vel) & 15) || dst + 4 * n > SPU_LS_SIZE ||
        vel + 4 * n + 32 > SPU_LS_SIZE || rlo < 0 || rhi > SPU_LS_SIZE ||
        overlaps(dst, 4 * n, (uint32_t)rlo, (uint32_t)(rhi - rlo)) || overlaps(dst, 4 * n, vel, 4 * n + 32))
        return 0;
    const u128 half = spu_splat_u32(0x3F000000u);
    /* SPU pass 161: the 16-byte output quad is assembled with pshufb (byte 0 of each lane of the 4 pixels' results) and
     * stored once; b0 -> b0/256 and the S0 splat stay in vector registers (the 4-lane loops and the union round trips
     * through memory of pass 140 are gone). Same arithmetic, so the same bits (native_diff slots 22/23, test_r136). */
    /* pkq[p]: low byte of each channel word -> bytes 4p..4p+3 (a table, not _mm_slli_si128(x, 4 * p): clang -O1
       does not unroll the p loop and needs that shift count to be a constant) */
    const __m128i pkq[4] = {
        _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 8, 4, 0),
        _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 8, 4, 0, -1, -1, -1, -1),
        _mm_set_epi8(-1, -1, -1, -1, 12, 8, 4, 0, -1, -1, -1, -1, -1, -1, -1, -1),
        _mm_set_epi8(12, 8, 4, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)};
    for (uint32_t g = 0; g < n / 4; g++) {
        const u128 vq = mb_ldq(ls, vel + 16 * g);
        __m128i vqi;
        memcpy(&vqi, &vq, 16);
        const __m128i b0i = _mm_srli_epi32(vqi, 24);
        u128 b0q;
        memcpy(&b0q, &b0i, 16);
        const u128 sq = spu_cuflt(b0q, 147); /* b0 / 256 per pixel */
        float sqa[4];
        memcpy(sqa, &sq, 16);
        u128 TT[4][12];
#pragma GCC unroll 4
        for (int p = 0; p < 4; p++) {
            const uint32_t w = vq._u32[p];
            const int32_t vx = 16 * (int32_t)((w >> 16) & 0xFF) - 2048, vy = (int32_t)((w >> 8) & 0xFF) - 128;
            const int32_t P = (int32_t)(src + 64 * g + 16 * p);
#pragma GCC unroll 12
            for (int k = 0; k < 12; k++) {
#ifdef KZ3_NATIVE_NEGCTL
                const int sg = k < 6 ? 1 : -1; /* negative control for the tests: the sides summed the other way round */
#else
                const int sg = k < 6 ? -1 : 1;
#endif
                const int32_t c = mb_c[k % 6];
                const int32_t dx = ((int32_t)(int16_t)(sg * vx) * c) >> 16, dy = ((int32_t)(int16_t)(sg * vy) * c) >> 16;
                TT[p][k] = mb_ldq(ls, (uint32_t)(P + dy * pitch + dx));
            }
        }
        __m128i outq = _mm_setzero_si128();
#pragma GCC unroll 4
        for (int p = 0; p < 4; p++) {
            const u128* T = TT[p];
            u128 S = spu_fa(T[11], T[10]);
            S = spu_fa(T[9], S);
            S = spu_fa(T[8], S);
            S = spu_fa(T[7], S);
            S = spu_fma(T[6], half, S);
            S = spu_fa(T[5], S);
            S = spu_fa(T[4], S);
            S = spu_fa(T[3], S);
            S = spu_fa(T[2], S);
            S = spu_fa(T[1], S);
            S = spu_fma(T[0], half, S);
            uint32_t s0w;
            memcpy(&s0w, &S, 4);   /* lane 0 of the sum: the estimate pair's input */
            int slow = 0;
            const uint32_t rw = mb_rec_scalar(s0w, &slow);
            u128 rec;
            if (__builtin_expect(slow, 0)) {
                const u128 s0 = spu_splat_u32(s0w);
                rec = spu_fi(s0, spu_frest(s0));
            } else rec = spu_splat_u32(rw);
            u128 spl;
            { const __m128 sp = _mm_set1_ps(sqa[p]); memcpy(&spl, &sp, 16); }
            const u128 qd = spu_fm(spu_fm(S, rec), spl);
            __m128i ui;
            { __m128 qv; memcpy(&qv, &qd, 16);
              if (!mb_cflt_bytes(qv, &ui)) { const u128 u = spu_cfltu(qd, 165); memcpy(&ui, &u, 16); } }
            /* low byte of each of the 4 channel words -> bytes 4p..4p+3 */
            outq = _mm_or_si128(outq, _mm_shuffle_epi8(ui, pkq[p]));
        }
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), outq);
    }
    return 1;
}

#define UP_ENV "KZ3_NATIVE_JOB_UPSAMPLE"
#define MIX_ENV "KZ3_NATIVE_JOB_POSTMIX"
#define MB_ENV "KZ3_NATIVE_JOB_MOTIONBLUR"
#define POOL_MB 0x5C50u, 0x5DF0u, 0x52a18409a3b8087full /* native_pool.py 011f7d00 0x5460 (pass 136) */
#define POOL_BLEND_DOF 0x5AE0u, 0x5BB0u, 0xb65df4abc0d86d6full
#define POOL_BLEND_MB 0x5C50u, 0x5D20u, 0xb65df4abc0d86d6full
#define POOL_BLEND_UP 0x5A70u, 0x5B00u, 0x7ebb5d25c354924full

/* Gates (re/spu/native_gates.tsv; pools from tools/spu/native_pool.py): slot, flags, job, image, LS address, kernel,
 * family, constant pool [lo, hi) and its hash. 1.00 twins: the modules are byte-identical (the RE notes
 * s.184, spu_map_114.tsv). */
const kz3_native_entry kz3_native_post[] = {
    {0, MIX_ENV, "Gfx.Post.DepthOfField", "job_011f9c80", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_DOF, 1},
    {1, MIX_ENV, "Gfx.Post.DepthOfField", "job_0117d880", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_DOF},
    {2, MIX_ENV, "Gfx.Post.MotionBlur", "job_011f7d00", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_MB},
    {3, MIX_ENV, "Gfx.Post.MotionBlur", "job_0117b900", 0x4D00u, kz3_post_blend, "blend", POOL_BLEND_MB},
    {4, MIX_ENV, "Gfx.Post.Upsample", "job_011d4680", 0x4DD0u, kz3_post_blend, "blend", POOL_BLEND_UP, 1},
    {5, MIX_ENV, "Gfx.Post.Upsample", "job_01158280", 0x4DD0u, kz3_post_blend, "blend", POOL_BLEND_UP},
    {18, UP_ENV, "Gfx.Post.Upsample", "job_011d4680", 0x52C0u, kz3_post_upsample8, "upsample8", POOL_BLEND_UP, 1},
    {19, UP_ENV, "Gfx.Post.Upsample", "job_01158280", 0x52C0u, kz3_post_upsample8, "upsample8", POOL_BLEND_UP},
    {22, MB_ENV, "Gfx.Post.MotionBlur", "job_011f7d00", 0x5460u, mb_blur_row_5460, "mb_blur", POOL_MB},
    {23, MB_ENV, "Gfx.Post.MotionBlur", "job_0117b900", 0x5460u, mb_blur_row_5460, "mb_blur", POOL_MB},
};
const unsigned kz3_native_post_n = sizeof kz3_native_post / sizeof kz3_native_post[0];
