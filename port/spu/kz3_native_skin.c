/* kz3_native_skin.c -- Anim.Skinning (SPUSkinning, ovl255: 1.14 job_01242c80, 1.00 twin job_011c5b00, byte-identical
 * modules) 3-bone vertex skinning kernel @0x5670 + loop 0x5868 as native host code (SPU pass 141). The 1.14 slot 24 is
 * default on (SPU pass 142, same-run toggle A/B on K4: native lower in 10/10 paired phases, median 0.91x); the 1.00 twin
 * slot 25 is unmeasured and stays off. KZ3_NATIVE_JOB_SKIN=0 turns slot 24 off, =1 turns on both (KZ3_NATIVE_TOGGLE=24 for the A/B). The module is the biggest SPU job on K4
 * (pass 97: 621 calls per frame, 100 us per call, 18 % of SPU time; this function ~50 % of the module).
 *
 * Call (0x5008..0x501C in the job's driver 0x4FE8; the SPU ABI's argument registers):
 *   r3 output (32 bytes per vertex), r4 stream A (float4 per vertex, LS 0x6600 in the job), r5 stream B (float4 per
 *   vertex, LS 0x7600), r6 record stream (20-byte records, base unaligned; the function reads 16 bytes at rec + 6),
 *   r7 / r8 two bone palettes (64-byte 4x4 matrices: rows +0x00/+0x10/+0x20/+0x30), r9 vertex count, r10 record
 *   stride (20 at the call site). Only word 0 of each register counts (the SPU's preferred slot).
 * Per vertex k, Q = the 16 bytes at r6 + 6 + k * r10 (bytes 0-1 weights, 5-7 bone indices, 8-10 and 11-13 two
 * signed-byte vectors), A = stream A[k], B = stream B[k]:
 *   w0, w1 = Q[0] / 255, Q[1] / 255 (cuflt, fm by 0x3B808081);  w2 = 1 - (w0 + w1)
 *   M_b = r7 + 64 Q[5+b], N_b = r8 + 64 Q[5+b] (b = 0, 1, 2; mpya)
 *   P = sum_b w_b (M_b.r0 Ax + M_b.r1 Ay + M_b.r2 Az + M_b.r3)            position, palette r7
 *   P' = the same with N_b                                              position, palette r8
 *   n = normalise(sum_b w_b M_b.(3x3) B)                                normal, palette r7
 *   t1, t2 = normalise(sum_b w_b M_b.(3x3) (Q[8..10] / 127)), the same for Q[11..13]
 *   out[k] = { P.x, P.y, P.z, h(d.x):h(d.y) }, { h(d.z):h(n.x), h(n.y):h(n.z), p(t1), p(t2) }   d = P - P'
 * where h is the SPU's float-to-half (x - 0x38000000 bits 28..13, |x| >= 65536 -> 0x7BFF, |x| < 2^-15 -> 0, the
 * sign kept), p the 11:11:10 pack (x 11 bits, y 11, z 10 of cflts(t * (2047/2048, 2047/2048, 1023/1024), 2^31)) and
 * normalise v * fi(s, frsqest(s)) with s = (vx^2 + vy^2) + vz^2. ASSUMPTION (pass 98): P' is the previous frame's
 * pose (d is the motion vector), n the normal, t1/t2 tangent and binormal.
 *
 * Bit-exact with the lifted code by construction: every float op is the runtime's SPU helper (spu_fm/fma/fa/fs, the
 * conversions, frsqest, fi) on the same operands in the same order as the SPU's dataflow (recovered by symbolic
 * execution of the software-pipelined loop, the RE notes "Pass 141"); the integer packing is per-lane C of the
 * same bit operations. tools/spu/native_diff.py (kind "skin") and tools/spu/test_r141_native_skin.sh are the proof.
 * Guards (-> 0, the lifted code runs): n == 0 (the SPU loop would run 2^32 times) or 32 n past the local store;
 * unaligned r3, r4, r5, r7 or r8; any input or the output past the local store; the output overlapping the records,
 * either stream or the bones the records index (the SPU loop reads a vertex ahead of its stores, so in-place work
 * would see other values). Registers: the kernel leaves r2-r79 as they were (SPU ABI volatile; the caller's
 * continuation 0x4D10/0x4D30/0x4D60 reads only r6 after writing it, and r80+) and keeps r0, r1, r80-r127. */
#include "kz3_native_jobs.h"
#include "spu_helpers.h"

#include <smmintrin.h>
#include <string.h>

/* the helpers below take and return u128 by value: inlined, or every call passes them through memory */
#define SK_INLINE static inline __attribute__((always_inline))
#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

SK_INLINE u128 sk_ldq(const uint8_t* ls, uint32_t a)
{
    u128 r;
    __m128i v = _mm_loadu_si128((const __m128i*)(ls + LSQ(a)));
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    memcpy(&r, &v, 16);
    return r;
}

SK_INLINE void sk_stq(uint8_t* ls, uint32_t a, const uint32_t w[4])
{
    __m128i v = _mm_loadu_si128((const __m128i*)w);
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    _mm_storeu_si128((__m128i*)(ls + LSQ(a)), v);
}

SK_INLINE u128 sk_spl(u128 v, int k)
{
    return spu_splat_u32(v._u32[k]);
}

/* r0 x + r1 y + r2 z in the SPU's order: fm, then fma, fma */
SK_INLINE u128 sk_xf3(u128 r0, u128 r1, u128 r2, u128 x, u128 y, u128 z)
{
    return spu_fma(r2, z, spu_fma(r1, y, spu_fm(r0, x)));
}

/* sum_b w_b v_b, bone 0 first (the SPU's fm, fma, fma) */
SK_INLINE u128 sk_blend(u128 v0, u128 v1, u128 v2, u128 w0, u128 w1, u128 w2)
{
#ifdef KZ3_NATIVE_NEGCTL
    return spu_fma(v0, w0, spu_fma(v1, w1, spu_fm(v2, w2))); /* negative control for the tests: bone 2 first */
#else
    return spu_fma(v2, w2, spu_fma(v1, w1, spu_fm(v0, w0)));
#endif
}

/* v * rsqrt((vx^2 + vy^2) + vz^2): the lane sum through shlqbyi 4 / 8 (word 0 is the sum), splat, frsqest + fi */
SK_INLINE u128 sk_norm(u128 v)
{
    const u128 sq = spu_fm(v, v);
    u128 s4 = sq, s8 = sq;
    s4._u32[0] = sq._u32[1], s4._u32[1] = sq._u32[2], s4._u32[2] = sq._u32[3], s4._u32[3] = 0;
    s8._u32[0] = sq._u32[2], s8._u32[1] = sq._u32[3], s8._u32[2] = 0, s8._u32[3] = 0;
#ifdef KZ3_NATIVE_NEGCTL
    const u128 s = spu_fa(spu_fa(s4, s8), sq); /* negative control for the tests: (y^2 + z^2) + x^2 */
#else
    const u128 s = spu_fa(spu_fa(sq, s4), s8);
#endif
    const u128 s0 = sk_spl(s, 0);
    return spu_fm(v, spu_fi(s0, spu_frsqest(s0)));
}

/* sf 0x38000000, rotqbii 3, top halfword; clamp 0x7BFF at |x| >= 0x4780xxxx, 0 below 0x3800xxxx, sign of x */
SK_INLINE uint32_t sk_half(uint32_t x)
{
    const uint32_t t = x >> 16, a = t & 0x7FFFu;
    if (a < 0x3800u) return 0;
    const uint32_t h = a < 0x4780u ? ((x - 0x38000000u) >> 13) & 0x7FFFu : 0x7BFFu;
    return h | (t & 0x8000u);
}

/* rotm by 21 / 10 / 0, shufb bytes 8,9,2,3, selb with y's bits 0x003FF800 */
SK_INLINE uint32_t sk_pack(u128 c)
{
    const uint32_t u0 = c._u32[0] >> 21, u1 = c._u32[1] >> 10, u2 = c._u32[2];
    const uint32_t w = (u2 & 0xFFFF0000u) | (u0 & 0xFFFFu);
    return (w & ~0x003FF800u) | (u1 & 0x003FF800u);
}

SK_INLINE int sk_overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}

/* One vertex through the runtime's helpers (the reference semantics; also the fallback of the fast path). q = the
 * vertex's 16 record bytes, A/B its stream quadwords, o = the 8 output words. */
SK_INLINE void sk_vertex_ref(const uint8_t* ls, const uint8_t* q, uint32_t aa, uint32_t ba, uint32_t p1, uint32_t p2,
                             uint32_t o[8])
{
    const u128 k255 = spu_splat_u32(0x3B808081u), k127 = spu_splat_u32(0x3C010204u), one = spu_splat_u32(0x3F800000u);
    u128 kpk, wv, v1, v2;
    kpk._u32[0] = 0x3F7FE000u, kpk._u32[1] = 0x3F7FE000u, kpk._u32[2] = 0x3F7FC000u, kpk._u32[3] = 0;
    wv._u32[0] = q[0], wv._u32[1] = q[1], wv._u32[2] = 0, wv._u32[3] = 0;
    v1._u32[0] = (uint32_t)(int32_t)(int8_t)q[8], v1._u32[1] = (uint32_t)(int32_t)(int8_t)q[9];
    v1._u32[2] = (uint32_t)(int32_t)(int8_t)q[10], v1._u32[3] = 0;
    v2._u32[0] = (uint32_t)(int32_t)(int8_t)q[11], v2._u32[1] = (uint32_t)(int32_t)(int8_t)q[12];
    v2._u32[2] = (uint32_t)(int32_t)(int8_t)q[13], v2._u32[3] = 0;
    u128 M[3][4], N[3][4];
    for (int b = 0; b < 3; b++)
        for (int r = 0; r < 4; r++) {
            M[b][r] = sk_ldq(ls, p1 + 64u * q[5 + b] + 16u * r);
            N[b][r] = sk_ldq(ls, p2 + 64u * q[5 + b] + 16u * r);
        }
    const u128 A = sk_ldq(ls, aa), B = sk_ldq(ls, ba);
    const u128 W = spu_fm(spu_cuflt(wv, 155), k255);
    const u128 w0 = sk_spl(W, 0), w1 = sk_spl(W, 1), w2 = spu_fs(one, spu_fa(w0, w1));
    const u128 V1 = spu_fm(spu_csflt(v1, 155), k127), V2 = spu_fm(spu_csflt(v2, 155), k127);
    const u128 ax = sk_spl(A, 0), ay = sk_spl(A, 1), az = sk_spl(A, 2);
    const u128 bx = sk_spl(B, 0), by = sk_spl(B, 1), bz = sk_spl(B, 2);
    const u128 cx = sk_spl(V1, 0), cy = sk_spl(V1, 1), cz = sk_spl(V1, 2);
    const u128 ex = sk_spl(V2, 0), ey = sk_spl(V2, 1), ez = sk_spl(V2, 2);
    u128 PA[3], PB[3], NB[3], T1[3], T2[3];
    for (int b = 0; b < 3; b++) {
        PA[b] = spu_fa(sk_xf3(M[b][0], M[b][1], M[b][2], ax, ay, az), M[b][3]);
        PB[b] = spu_fa(sk_xf3(N[b][0], N[b][1], N[b][2], ax, ay, az), N[b][3]);
        NB[b] = sk_xf3(M[b][0], M[b][1], M[b][2], bx, by, bz);
        T1[b] = sk_xf3(M[b][0], M[b][1], M[b][2], cx, cy, cz);
        T2[b] = sk_xf3(M[b][0], M[b][1], M[b][2], ex, ey, ez);
    }
    const u128 P = sk_blend(PA[0], PA[1], PA[2], w0, w1, w2), P2 = sk_blend(PB[0], PB[1], PB[2], w0, w1, w2);
    const u128 d = spu_fs(P, P2);
    const u128 nn = sk_norm(sk_blend(NB[0], NB[1], NB[2], w0, w1, w2));
    const u128 t1 = sk_norm(sk_blend(T1[0], T1[1], T1[2], w0, w1, w2));
    const u128 t2 = sk_norm(sk_blend(T2[0], T2[1], T2[2], w0, w1, w2));
    const u128 c1 = spu_cflts(spu_fm(t1, kpk), 142), c2 = spu_cflts(spu_fm(t2, kpk), 142);
    o[0] = P._u32[0], o[1] = P._u32[1], o[2] = P._u32[2], o[3] = sk_half(d._u32[0]) << 16 | sk_half(d._u32[1]);
    o[4] = sk_half(d._u32[2]) << 16 | sk_half(nn._u32[0]), o[5] = sk_half(nn._u32[1]) << 16 | sk_half(nn._u32[2]);
    o[6] = sk_pack(c1), o[7] = sk_pack(c2);
}

/* The same vertex as plain SSE/FMA operations, the helpers' own fast paths (fm/fa/fs: _mm_mul/add/sub_ps, taken by
 * the helper when no result lane has exponent 255, patch 1105; fma: _mm_fmadd_ps, taken when no result lane is
 * exponent 0 non-zero, 1 or 255, patch 1106, under -mfma with the default float mode g_kz3_spu_slowcg == 0). Each
 * op's result feeds the same test as its helper (accumulated over the vertex); a vertex where any op would have left
 * the fast path returns 0 before it stores anything and is redone by sk_vertex_ref, so the bits are the helpers'
 * in every case. The three square sums share one frsqest + fi (lanes 0, 1, 2: the helpers are per lane). */
#if defined(__FMA__) && defined(__SSE4_1__) && SPU_FMA_ODD4 && SPU_XR_RMASK
#define SK_FAST 1
#include <immintrin.h>
typedef struct {
    __m128i e; /* max of fa/fm/fs results' exponent fields: 0x7F800000 = some lane was exponent 255 */
    __m128i f; /* or of the fma results' odd-lane bits */
    int chk;   /* 0: sk_bounded() proved that no fma result can be odd, the fma test is left out (a constant) */
} sk_acc;

SK_INLINE __m128 sk_m(sk_acc* c, __m128 a, __m128 b)
{
    const __m128 r = _mm_mul_ps(a, b);
    c->e = _mm_max_epu32(c->e, _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7F800000)));
    return r;
}
SK_INLINE __m128 sk_a(sk_acc* c, __m128 a, __m128 b)
{
    const __m128 r = _mm_add_ps(a, b);
    c->e = _mm_max_epu32(c->e, _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7F800000)));
    return r;
}
SK_INLINE __m128 sk_s(sk_acc* c, __m128 a, __m128 b)
{
    const __m128 r = _mm_sub_ps(a, b);
    c->e = _mm_max_epu32(c->e, _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7F800000)));
    return r;
}
SK_INLINE __m128 sk_ma(sk_acc* c, __m128 a, __m128 b, __m128 d)
{
    const __m128 r = _mm_fmadd_ps(a, b, d);
#ifndef SK_VERIFY_BOUND
    if (!c->chk) return r;
#endif
    const __m128i ab = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nn = _mm_cmpgt_epi32(_mm_add_epi32(ab, _mm_set1_epi32(0x7F000000)), _mm_set1_epi32(-0x01800001));
    c->f = _mm_or_si128(c->f, _mm_and_si128(nn, ab));
    return r;
}
SK_INLINE __m128 sk_ld(const uint8_t* ls, uint32_t a)
{
    const __m128i v = _mm_loadu_si128((const __m128i*)(ls + LSQ(a)));
    return _mm_castsi128_ps(_mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3)));
}
#define SK_SPL(v, k) _mm_shuffle_ps((v), (v), (k) * 0x55)
SK_INLINE __m128 sk_fxf3(sk_acc* c, const __m128* R, __m128 x, __m128 y, __m128 z)
{
    return sk_ma(c, R[2], z, sk_ma(c, R[1], y, sk_m(c, R[0], x)));
}
SK_INLINE __m128 sk_fblend(sk_acc* c, const __m128* v, __m128 w0, __m128 w1, __m128 w2)
{
#ifdef KZ3_NATIVE_NEGCTL
    return sk_ma(c, v[0], w0, sk_ma(c, v[1], w1, sk_m(c, v[2], w2)));
#else
    return sk_ma(c, v[2], w2, sk_ma(c, v[1], w1, sk_m(c, v[0], w0)));
#endif
}
/* Can any fma of this call leave the helper's fast path? Its test (patch 1106) fires on a result lane that is
 * exponent 255 or below 2^-125 and not zero. Every fma here is a step of r0 x + r1 y + r2 z or of the weight blend,
 * whose operands are the bone rows (M), the stream x/y/z (A, B), the byte vectors i/127 and the weights. A non-zero
 * exact a b + c is a multiple of 2^min(G(a) + G(b), G(c)) when a, b, c are multiples of 2^G(.), and its rounded value
 * (any rounding mode) is a non-zero multiple of the same power, so it is at least that large. With e(.) the smallest
 * exponent of a non-zero value, G = e - 23 for M, A and B, -30 for i/127 (|i/127| >= 2^-7) and -31 for the weights
 * (b/255 >= 2^-8; w2 = 1 - (w0 + w1) is a multiple of 2^-31 too): the positions are multiples of 2^(eM + eA - 77),
 * the normals of 2^(eM + eB - 77), the byte vectors of 2^(eM - 84). All at least 2^-125 when eM + eA >= -48,
 * eM + eB >= -48 and eM >= -41. No result reaches 2^128 when the largest exponents satisfy eM + max(eA, eB, 0) <= 110
 * (|w| <= 1, three terms of three products). Exponents here are biased (127 = 2^0); a denormal (exponent 0, non-zero)
 * fails the test. The fa/fm/fs tests stay in either case (the normalisation can reach exponent 255: a zero vector). */
SK_INLINE void sk_exp_range(__m128i v, __m128i lane3, __m128i* lo, __m128i* hi)
{
    const __m128i ab = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i ex = _mm_srli_epi32(ab, 23), z = _mm_or_si128(_mm_cmpeq_epi32(ab, _mm_setzero_si128()), lane3);
    *lo = _mm_min_epu32(*lo, _mm_or_si128(ex, z));
    *hi = _mm_max_epu32(*hi, _mm_andnot_si128(z, ex));
}
SK_INLINE uint32_t sk_hmin(__m128i v)
{
    v = _mm_min_epu32(v, _mm_shuffle_epi32(v, 0x4E));
    return (uint32_t)_mm_cvtsi128_si32(_mm_min_epu32(v, _mm_shuffle_epi32(v, 0xB1)));
}
SK_INLINE uint32_t sk_hmax(__m128i v)
{
    v = _mm_max_epu32(v, _mm_shuffle_epi32(v, 0x4E));
    return (uint32_t)_mm_cvtsi128_si32(_mm_max_epu32(v, _mm_shuffle_epi32(v, 0xB1)));
}
static int sk_bounded(const uint8_t* ls, uint32_t p1, uint32_t p2, uint32_t mn, uint32_t mx, uint32_t sa, uint32_t sb,
                      uint32_t n)
{
    const __m128i none = _mm_setzero_si128(), w3 = _mm_setr_epi32(0, 0, 0, -1);
    __m128i lm = _mm_set1_epi32(-1), hm = none, la = lm, ha = none, lb = lm, hb = none;
    for (uint32_t a = 64 * mn; a < 64 * mx + 64; a += 16) {
        sk_exp_range(_mm_castps_si128(sk_ld(ls, p1 + a)), none, &lm, &hm);
        sk_exp_range(_mm_castps_si128(sk_ld(ls, p2 + a)), none, &lm, &hm);
    }
    for (uint32_t k = 0; k < n; k++) {
        sk_exp_range(_mm_castps_si128(sk_ld(ls, sa + 16 * k)), w3, &la, &ha);
        sk_exp_range(_mm_castps_si128(sk_ld(ls, sb + 16 * k)), w3, &lb, &hb);
    }
    /* an all-zero set has lo = 0xFFFFFFFF (no constraint) and hi = 0 */
    const int64_t eml = (int64_t)sk_hmin(lm) - 127, eal = (int64_t)sk_hmin(la) - 127, ebl = (int64_t)sk_hmin(lb) - 127;
    const int64_t emh = (int64_t)sk_hmax(hm) - 127, eah = (int64_t)sk_hmax(ha) - 127, ebh = (int64_t)sk_hmax(hb) - 127;
    const int64_t eabh = eah > ebh ? eah : ebh;
    return eml + eal >= -48 && eml + ebl >= -48 && eml >= -41 && emh + (eabh > 0 ? eabh : 0) <= 110;
}

/* (x^2 + y^2) + z^2 in word 0 (the shlqbyi 4 / 8 sums of sk_norm) */
SK_INLINE __m128 sk_fsum(sk_acc* c, __m128 v)
{
    const __m128 sq = sk_m(c, v, v);
    const __m128 s4 = _mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(sq), 4)),
                 s8 = _mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(sq), 8));
#ifdef KZ3_NATIVE_NEGCTL
    return sk_a(c, sk_a(c, s4, s8), sq);
#else
    return sk_a(c, sk_a(c, sq, s4), s8);
#endif
}

SK_INLINE int sk_vertex_fast(const uint8_t* ls, const uint8_t* q, uint32_t aa, uint32_t ba, uint32_t p1, uint32_t p2,
                             uint32_t o[8], const int chk)
{
    sk_acc c = {_mm_setzero_si128(), _mm_setzero_si128(), chk};
    __m128 M[3][4], N[3][4];
    for (int b = 0; b < 3; b++)
        for (int r = 0; r < 4; r++) {
            M[b][r] = sk_ld(ls, p1 + 64u * q[5 + b] + 16u * r);
            N[b][r] = sk_ld(ls, p2 + 64u * q[5 + b] + 16u * r);
        }
    const __m128 A = sk_ld(ls, aa), B = sk_ld(ls, ba);
    /* cuflt / csflt of bytes: small integers, exact in either conversion (the helpers' exponent-255 test cannot fire) */
    const __m128 W =
        sk_m(&c, _mm_cvtepi32_ps(_mm_setr_epi32(q[0], q[1], 0, 0)), _mm_castsi128_ps(_mm_set1_epi32(0x3B808081)));
    const __m128 V1 = sk_m(&c, _mm_cvtepi32_ps(_mm_setr_epi32((int8_t)q[8], (int8_t)q[9], (int8_t)q[10], 0)),
                           _mm_castsi128_ps(_mm_set1_epi32(0x3C010204)));
    const __m128 V2 = sk_m(&c, _mm_cvtepi32_ps(_mm_setr_epi32((int8_t)q[11], (int8_t)q[12], (int8_t)q[13], 0)),
                           _mm_castsi128_ps(_mm_set1_epi32(0x3C010204)));
    const __m128 w0 = SK_SPL(W, 0), w1 = SK_SPL(W, 1), w2 = sk_s(&c, _mm_set1_ps(1.0f), sk_a(&c, w0, w1));
    const __m128 ax = SK_SPL(A, 0), ay = SK_SPL(A, 1), az = SK_SPL(A, 2), bx = SK_SPL(B, 0), by = SK_SPL(B, 1),
                 bz = SK_SPL(B, 2), cx = SK_SPL(V1, 0), cy = SK_SPL(V1, 1), cz = SK_SPL(V1, 2), ex = SK_SPL(V2, 0),
                 ey = SK_SPL(V2, 1), ez = SK_SPL(V2, 2);
    __m128 PA[3], PB[3], NB[3], T1[3], T2[3];
    for (int b = 0; b < 3; b++) {
        PA[b] = sk_a(&c, sk_fxf3(&c, M[b], ax, ay, az), M[b][3]);
        PB[b] = sk_a(&c, sk_fxf3(&c, N[b], ax, ay, az), N[b][3]);
        NB[b] = sk_fxf3(&c, M[b], bx, by, bz);
        T1[b] = sk_fxf3(&c, M[b], cx, cy, cz);
        T2[b] = sk_fxf3(&c, M[b], ex, ey, ez);
    }
    const __m128 P = sk_fblend(&c, PA, w0, w1, w2), P2 = sk_fblend(&c, PB, w0, w1, w2);
    const __m128 d = sk_s(&c, P, P2);
    const __m128 nv = sk_fblend(&c, NB, w0, w1, w2), t1v = sk_fblend(&c, T1, w0, w1, w2),
                 t2v = sk_fblend(&c, T2, w0, w1, w2);
    const __m128 sn = sk_fsum(&c, nv), s1 = sk_fsum(&c, t1v), s2 = sk_fsum(&c, t2v);
    /* lanes 0/1/2 = the three sums (word 0 of each), lane 3 a copy of lane 0 */
    const __m128 S = _mm_shuffle_ps(_mm_unpacklo_ps(sn, s1), _mm_unpacklo_ps(s2, sn), 0x44);
    u128 Su, Ru;
    memcpy(&Su, &S, 16);
    Ru = spu_fi(Su, spu_frsqest(Su));
    __m128 R;
    memcpy(&R, &Ru, 16);
    const __m128 nn = sk_m(&c, nv, SK_SPL(R, 0)), t1 = sk_m(&c, t1v, SK_SPL(R, 1)), t2 = sk_m(&c, t2v, SK_SPL(R, 2));
    const __m128 kpk = _mm_castsi128_ps(_mm_setr_epi32(0x3F7FE000, 0x3F7FE000, 0x3F7FC000, 0));
    const __m128 k1 = sk_m(&c, t1, kpk), k2 = sk_m(&c, t2, kpk);
#ifdef SK_VERIFY_BOUND /* test builds: an fma the bound said could not be odd was odd */
    if (!chk && !_mm_testz_si128(c.f, c.f)) __builtin_trap();
#endif
    if (!_mm_testz_si128(c.f, c.f) ||
        !_mm_testz_si128(_mm_cmpeq_epi32(c.e, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(-1)))
        return 0;
    u128 K1, K2, Pu, Du, Nu;
    memcpy(&K1, &k1, 16), memcpy(&K2, &k2, 16), memcpy(&Pu, &P, 16), memcpy(&Du, &d, 16), memcpy(&Nu, &nn, 16);
    const u128 c1 = spu_cflts(K1, 142), c2 = spu_cflts(K2, 142);
    o[0] = Pu._u32[0], o[1] = Pu._u32[1], o[2] = Pu._u32[2], o[3] = sk_half(Du._u32[0]) << 16 | sk_half(Du._u32[1]);
    o[4] = sk_half(Du._u32[2]) << 16 | sk_half(Nu._u32[0]), o[5] = sk_half(Nu._u32[1]) << 16 | sk_half(Nu._u32[2]);
    o[6] = sk_pack(c1), o[7] = sk_pack(c2);
    return 1;
}
#else
#define SK_FAST 0
SK_INLINE int sk_vertex_fast(const uint8_t* ls, const uint8_t* q, uint32_t aa, uint32_t ba, uint32_t p1, uint32_t p2,
                             uint32_t o[8], const int chk)
{
    (void)ls, (void)q, (void)aa, (void)ba, (void)p1, (void)p2, (void)o, (void)chk;
    return 0;
}
static int sk_bounded(const uint8_t* ls, uint32_t p1, uint32_t p2, uint32_t mn, uint32_t mx, uint32_t sa, uint32_t sb,
                      uint32_t n)
{
    (void)ls, (void)p1, (void)p2, (void)mn, (void)mx, (void)sa, (void)sb, (void)n;
    return 0;
}
#endif

#ifdef SK_STATS /* test builds: which paths the states took */
#include <stdio.h>
static long sk_st[4]; /* calls bounded, calls checked, vertices redone by the helpers, vertices */
__attribute__((destructor)) static void sk_st_print(void)
{
    printf("S skin paths: calls bounded=%ld checked=%ld, vertices=%ld redone=%ld\n", sk_st[0], sk_st[1], sk_st[3],
           sk_st[2]);
}
#define SK_ST(i, v) (sk_st[i] += (v))
#else
#define SK_ST(i, v) ((void)0)
#endif

static int skin3_5670(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t out = ctx->gpr[3]._u32[0], sa = ctx->gpr[4]._u32[0], sb = ctx->gpr[5]._u32[0],
                   rec = ctx->gpr[6]._u32[0] + 6u, p1 = ctx->gpr[7]._u32[0], p2 = ctx->gpr[8]._u32[0],
                   n = ctx->gpr[9]._u32[0], st = ctx->gpr[10]._u32[0];
    if (n == 0 || n > SPU_LS_SIZE / 32 || ((out | sa | sb | p1 | p2) & 15) || st >= SPU_LS_SIZE) return 0;
    const uint64_t rlen = (uint64_t)(n - 1) * st + 16;
    if ((uint64_t)out + 32ull * n > SPU_LS_SIZE || (uint64_t)sa + 16ull * n > SPU_LS_SIZE ||
        (uint64_t)sb + 16ull * n > SPU_LS_SIZE || (uint64_t)rec + rlen > SPU_LS_SIZE)
        return 0;
    if (sk_overlaps(out, 32ull * n, rec, rlen) || sk_overlaps(out, 32ull * n, sa, 16ull * n) ||
        sk_overlaps(out, 32ull * n, sb, 16ull * n))
        return 0;
    uint32_t mn = 255, mx = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t* q = ls + rec + k * st;
        for (int b = 5; b < 8; b++) {
            mn = q[b] < mn ? q[b] : mn;
            mx = q[b] > mx ? q[b] : mx;
        }
    }
    const uint64_t blo = 64ull * mn, blen = 64ull * (mx - mn) + 64;
    if ((uint64_t)p1 + blo + blen > SPU_LS_SIZE || (uint64_t)p2 + blo + blen > SPU_LS_SIZE ||
        sk_overlaps(out, 32ull * n, (uint64_t)p1 + blo, blen) || sk_overlaps(out, 32ull * n, (uint64_t)p2 + blo, blen))
        return 0;

    const int fast = SK_FAST && g_kz3_spu_slowcg == 0;
    uint32_t k = 0;
    const int bnd = fast && sk_bounded(ls, p1, p2, mn, mx, sa, sb, n);
    SK_ST(0, bnd), SK_ST(1, fast && !bnd), SK_ST(3, n);
    if (bnd) /* two vertices a step: two independent dependency chains for the core to overlap */
        for (; k + 1 < n; k += 2) {
            const uint8_t *q0 = ls + rec + k * st, *q1 = q0 + st;
            uint32_t o[16];
            const int ok0 = sk_vertex_fast(ls, q0, sa + 16u * k, sb + 16u * k, p1, p2, o, 0);
            const int ok1 = sk_vertex_fast(ls, q1, sa + 16u * k + 16u, sb + 16u * k + 16u, p1, p2, o + 8, 0);
            SK_ST(2, !ok0 + !ok1);
            if (__builtin_expect(!ok0, 0)) sk_vertex_ref(ls, q0, sa + 16u * k, sb + 16u * k, p1, p2, o);
            if (__builtin_expect(!ok1, 0)) sk_vertex_ref(ls, q1, sa + 16u * k + 16u, sb + 16u * k + 16u, p1, p2, o + 8);
            for (int j = 0; j < 4; j++) sk_stq(ls, out + 32u * k + 16u * j, o + 4 * j);
        }
    else if (fast)
        for (; k + 1 < n; k += 2) {
            const uint8_t *q0 = ls + rec + k * st, *q1 = q0 + st;
            uint32_t o[16];
            const int ok0 = sk_vertex_fast(ls, q0, sa + 16u * k, sb + 16u * k, p1, p2, o, 1);
            const int ok1 = sk_vertex_fast(ls, q1, sa + 16u * k + 16u, sb + 16u * k + 16u, p1, p2, o + 8, 1);
            SK_ST(2, !ok0 + !ok1);
            if (__builtin_expect(!ok0, 0)) sk_vertex_ref(ls, q0, sa + 16u * k, sb + 16u * k, p1, p2, o);
            if (__builtin_expect(!ok1, 0)) sk_vertex_ref(ls, q1, sa + 16u * k + 16u, sb + 16u * k + 16u, p1, p2, o + 8);
            for (int j = 0; j < 4; j++) sk_stq(ls, out + 32u * k + 16u * j, o + 4 * j);
        }
    for (; k < n; k++) {
        const uint8_t* q = ls + rec + k * st;
        uint32_t o[8];
        if (!fast || !sk_vertex_fast(ls, q, sa + 16u * k, sb + 16u * k, p1, p2, o, 1)) {
            SK_ST(2, 1);
            sk_vertex_ref(ls, q, sa + 16u * k, sb + 16u * k, p1, p2, o);
        }
        sk_stq(ls, out + 32u * k, o);
        sk_stq(ls, out + 32u * k + 16u, o + 4);
    }
    return 1;
}

#define SKIN_ENV "KZ3_NATIVE_JOB_SKIN"
#define POOL_SKIN 0x6350u, 0x6480u, 0xf1f2c5a5400a3ef0ull /* native_pool.py 01242c80 0x5670 (pass 141) */

/* Gates (re/spu/native_gates.tsv): slot, flags, job, image, LS address, kernel, family, constant pool, default. */
const kz3_native_entry kz3_native_skin[] = {
    {24, SKIN_ENV, "Anim.Skinning", "job_01242c80", 0x5670u, skin3_5670, "skin", POOL_SKIN, 1},
    {25, SKIN_ENV, "Anim.Skinning", "job_011c5b00", 0x5670u, skin3_5670, "skin", POOL_SKIN, 0},
};
const unsigned kz3_native_skin_n = sizeof kz3_native_skin / sizeof kz3_native_skin[0];
