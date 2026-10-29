/* kz3_native_lp.c -- Gfx.LightProbes (job_0116d380, ovl215; 1.00 twin job_010f0f00) kernels as native host code
 * (spu rounds 123-125: kd search, bake, blend, attenuation, dynamic-light SH). Default on for the 1.14 job (SPU pass 127 milestone), opt-in for the 1.00 twins: KZ3_NATIVE_JOB_LIGHTPROBES=0 turns all
 * off, _LP_KD / _LP_BAKE / _LP_BLEND / _LP_ATTEN / _LP_LIGHT=0 one kernel (a single =1 beats LIGHTPROBES=0).
 *
 * (Method note from KZ2 SPU pass 17, notes-for-kz3 2026-10-29: float ops through the helpers keep native code exact.)
 * Both kernels run the SPU's float ops through the runtime's own helpers (spu_fs/fm/fa/fma/fcgt on u128), so they
 * are bit-identical to the lifted code by construction of the arithmetic; what the native code replaces is the
 * lifted instruction stream (register file in memory, one LS access wrapper per quadword, branch hops).
 *
 *  - lp_kd_42D0 (spec docs/completed/spu-native-specs.md 1.4): 4-nearest-probe search in the implicit kd-tree.
 *    r3 = S (LS pointer; word 0 = probe count n, word 5 = probe array), r4 = out (quad of 4 node addresses, nearest
 *    first), r5 = query quad (a register, not a pointer). The lifted function also leaves its traversal stack in
 *    the LS (8 bytes per depth at sp-544+32: child index, squared plane distance) and a back-chain quad at sp-544:
 *    both are written here at the end, so the whole local store is identical.
 *  - lp_bake_45E8 (spec 1.8): r3 = S (word 7 = float scratch), r4 = 9 SH quads, r5 = direction table (52 B an
 *    entry, nine floats at +16), r6 = count (64 or 128), r7 = half-float output; evaluates the SH at every
 *    direction into the scratch (RGBA floats), then packs 4 float quads to 2 half quads (truncating).
 * Registers: both leave r2-r79 as they were (the lifted bodies clobber them; the only caller reads none after
 * the return) and keep r0, r1, r80-r127. Arguments outside what was checked (alignment, bounds, overlap, counts)
 * decline: the lifted body runs. */
#include "kz3_native_jobs.h"
#include "spu_helpers.h"

#include <smmintrin.h>
#include <string.h>

static inline uint32_t ld32(const uint8_t* ls, uint32_t a)
{
    uint32_t v;
    memcpy(&v, ls + (a & (SPU_LS_MASK)), 4);
    return __builtin_bswap32(v);
}

static inline void st32(uint8_t* ls, uint32_t a, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(ls + (a & (SPU_LS_MASK)), &v, 4);
}

/* a quadword as the runtime's u128 (host word k = SPU word k) */
static inline u128 ldq(const uint8_t* ls, uint32_t a)
{
    u128 r;
    __m128i v = _mm_loadu_si128((const __m128i*)(ls + (a & SPU_LS_MASK)));
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    memcpy(&r, &v, 16);
    return r;
}

static inline void stq(uint8_t* ls, uint32_t a, u128 r)
{
    __m128i v;
    memcpy(&v, &r, 16);
    v = _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
    _mm_storeu_si128((__m128i*)(ls + (a & SPU_LS_MASK)), v);
}

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

/* ---- kd-tree 4-nearest search ------------------------------------------------------------------------------- */

/* The kd search runs on scalar lane values: the host's own scalar SSE op (the same MXCSR, FTZ/DAZ and rounding the
 * runtime's vector ops use) and, when the result has biased exponent 255, the runtime's vector helper on splats (the
 * extended-range recompute of patch 1071 and its g_kz3_spu_xr switch stay the helper's). The same key compare as
 * fcgt (spu__sf_key) replaces the vector compare. No u128 round trips on the common path. Pass 125; the round-123
 * form (u128 helpers) gave the same bits: the equality runs are the proof (tools/spu/test_r125_native_lp.sh). */
static inline uint32_t sf_bits(__m128 v)
{
    return (uint32_t)_mm_cvtsi128_si32(_mm_castps_si128(v));
}
static inline __m128 sf_ld(uint32_t a)
{
    return _mm_castsi128_ps(_mm_cvtsi32_si128((int)a));
}
static inline uint32_t sf_add(uint32_t a, uint32_t b)
{
    const uint32_t r = sf_bits(_mm_add_ss(sf_ld(a), sf_ld(b)));
    if (__builtin_expect((r & 0x7F800000u) == 0x7F800000u, 0)) return spu_fa(spu_splat_u32(a), spu_splat_u32(b))._u32[0];
    return r;
}
static inline uint32_t sf_sub(uint32_t a, uint32_t b)
{
    const uint32_t r = sf_bits(_mm_sub_ss(sf_ld(a), sf_ld(b)));
    if (__builtin_expect((r & 0x7F800000u) == 0x7F800000u, 0)) return spu_fs(spu_splat_u32(a), spu_splat_u32(b))._u32[0];
    return r;
}
static inline uint32_t sf_mul(uint32_t a, uint32_t b)
{
    const uint32_t r = sf_bits(_mm_mul_ss(sf_ld(a), sf_ld(b)));
    if (__builtin_expect((r & 0x7F800000u) == 0x7F800000u, 0)) return spu_fm(spu_splat_u32(a), spu_splat_u32(b))._u32[0];
    return r;
}
static inline uint32_t fsub_l(uint32_t a, uint32_t b)
{
    return sf_sub(a, b);
}
/* key of a value known to be non-negative (squares, the list entries): the word, or 0 when its exponent is 0 */
static inline uint32_t nnkey(uint32_t x)
{
    return (x & 0x7F800000u) ? x : 0u;
}
static inline int fgt_l(uint32_t a, uint32_t b)
{
    return spu__sf_key(a) > spu__sf_key(b);
}

/* Insert node `na` (an LS address) into the ascending list of the 4 nearest: distance = (x2 + y2) + z2 of
 * q - pos with the SPU's op order (lane 0 of the lifted code's rotated sums). */
static inline void kd_insert(const uint8_t* ls, uint32_t na, const uint32_t* q, uint32_t* lst, uint32_t* adr)
{
    /* The distance is a sum of three squares (never negative) and the list is ascending, so the new point enters only
     * if its distance is below entry 3; every partial sum is already >= its first terms (the SPU's sums of
     * non-negative values are monotone), so a point can be rejected after one or two coordinates with the same result
     * as the full computation. */
    const uint32_t kb = nnkey(lst[3]);
    const uint32_t d0 = fsub_l(q[0], ld32(ls, na)), s0 = sf_mul(d0, d0);
    if (nnkey(s0) >= kb) return;
    const uint32_t d1 = fsub_l(q[1], ld32(ls, na + 4)), t01 = sf_add(s0, sf_mul(d1, d1));
    if (nnkey(t01) >= kb) return;
    const uint32_t d2 = fsub_l(q[2], ld32(ls, na + 8)), dist = sf_add(t01, sf_mul(d2, d2));
    if (nnkey(dist) >= kb) return;
    const uint32_t g = ((uint32_t)fgt_l(lst[0], dist) << 3) | ((uint32_t)fgt_l(lst[1], dist) << 2) |
                       ((uint32_t)fgt_l(lst[2], dist) << 1) | 1u;
    const int k = g ? 32 - __builtin_clz(g) : 0; /* lanes whose entry is farther than the new one: a suffix */
    if (!k) return;
    const int j0 = 4 - k;
    for (int j = 3; j > j0; j--) {
        lst[j] = lst[j - 1];
        adr[j] = adr[j - 1];
    }
    lst[j0] = dist;
    adr[j0] = na;
}

static int lp_kd_42D0(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t S = ctx->gpr[3]._u32[0], out = ctx->gpr[4]._u32[0], sp = ctx->gpr[1]._u32[0];
    const u128 q128 = ctx->gpr[5];
    if (((S | out | sp) & 15) || S + 32 > SPU_LS_SIZE || out + 16 > SPU_LS_SIZE || sp < 544 || sp > SPU_LS_SIZE)
        return 0;
    const uint32_t sp2 = sp - 544;
    const uint32_t n = ld32(ls, S), base = ld32(ls, S + 20);
    if (n > 0xFFFF || overlaps(sp2, 544, S, 32) || overlaps(sp2, 544, out, 16)) return 0;
    u128 addr = spu_fsmbi(0);
    if (n <= 4) {
        for (uint32_t i = 0; i < n; i++) addr._u32[i] = base + 96 * i; /* 0x453C: base + 96 i, the rest zero */
        stq(ls, sp2, ctx->gpr[1]);
        stq(ls, out, addr);
        return 1;
    }
    if (base > SPU_LS_SIZE || base + 96ull * n > SPU_LS_SIZE || (base & 15) || overlaps(sp2, 544, base, 96 * n))
        return 0;
    const uint32_t n2 = n >> 1;
    const uint32_t q[4] = {q128._u32[0], q128._u32[1], q128._u32[2], q128._u32[3]};
    uint32_t lst[4] = {0x7F7FFFFEu, 0x7F7FFFFEu, 0x7F7FFFFEu, 0x7F7FFFFEu}, adr[4] = {0, 0, 0, 0};
    uint32_t stk_c[64], stk_p[64];
    uint32_t wmask_lo = 0, wmask_hi = 0; /* depths that were written */
    uint32_t i = 1, depth = 0;
    uint32_t boundbits = 0x7F7FFFFEu;
    for (;;) {
        if (i <= n2) { /* 0x43D0: internal node, descend */
            if (depth >= 64) return 0;
            const uint32_t na = base + 96 * (i - 1);
            const uint32_t axis = ld32(ls, na + 12) & 3;
            const uint32_t dx = fsub_l(q[axis], ld32(ls, na + 4 * axis));
            stk_c[depth] = (!(dx & 0x80000000u) && (dx & 0x7F800000u)) ? 2 * i + 1 : 2 * i; /* key(dx) > 0 */
            stk_p[depth] = sf_mul(dx, dx);
            if (depth < 32)
                wmask_lo |= 1u << depth;
            else
                wmask_hi |= 1u << (depth - 32);
            i = stk_c[depth];
            depth++;
            continue;
        }
        if (i <= n) { /* 0x4450: leaf, evaluate the point */
            kd_insert(ls, base + 96 * (i - 1), q, lst, adr);
            boundbits = lst[3];
        }
        /* pop (0x4350/0x4370) */
        uint32_t top = depth - 1, cur = i, par = i >> 1;
        int again = 0;
        for (;;) {
            if (stk_c[top] == cur && fgt_l(boundbits, stk_p[top])) { /* the sibling subtree may hold a closer point */
                kd_insert(ls, base + 96 * (par - 1), q, lst, adr);
                boundbits = lst[3];
                i = (cur == 2 * par) ? cur + 1 : 2 * par;
                depth = top + 1;
                again = 1;
                break;
            }
            cur = par;
            if ((par >> 1) == 0) break;
            par >>= 1;
            top--;
        }
        if (!again) break;
    }
    for (int j = 0; j < 4; j++) addr._u32[j] = adr[j];
    stq(ls, sp2, ctx->gpr[1]);
    for (uint32_t d = 0; d < 64; d++)
        if ((d < 32 ? wmask_lo >> d : wmask_hi >> (d - 32)) & 1) {
            st32(ls, sp2 + 32 + 8 * d, stk_c[d]);
            st32(ls, sp2 + 32 + 8 * d + 4, stk_p[d]);
        }
    stq(ls, out, addr);
    return 1;
}

/* ---- SH bake and half packing -------------------------------------------------------------------------------- */

static int lp_bake_45E8(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t S = ctx->gpr[3]._u32[0], shp = ctx->gpr[4]._u32[0], tbl = ctx->gpr[5]._u32[0];
    const uint32_t cnt = ctx->gpr[6]._u32[0], oh = ctx->gpr[7]._u32[0];
    if (cnt == 0) return 1; /* the lifted body loads the SH quads and returns */
    if ((cnt & 3) || cnt > 256 || ((S | shp | oh) & 15) || (tbl & 3) || S + 32 > SPU_LS_SIZE || shp + 144 > SPU_LS_SIZE)
        return 0;
    const uint32_t scr = ld32(ls, S + 28);
    if ((scr & 15) || scr + 16 * cnt > SPU_LS_SIZE || oh + 8 * cnt > SPU_LS_SIZE || tbl + 52 * cnt + 16 > SPU_LS_SIZE ||
        overlaps(scr, 16 * cnt, S, 32))
        return 0;
    u128 sh[9];
    for (int k = 0; k < 9; k++) sh[k] = ldq(ls, shp + 16 * k);
    for (uint32_t i = 0; i < cnt; i++) {
        const uint32_t e = tbl + 52 * i + 16;
        u128 acc = spu_fm(sh[1], spu_splat_u32(ld32(ls, e + 4)));
        acc = spu_fma(sh[0], spu_splat_u32(ld32(ls, e)), acc);
        for (int k = 2; k < 9; k++) acc = spu_fma(sh[k], spu_splat_u32(ld32(ls, e + 4 * k)), acc);
        stq(ls, scr + 16 * i, acc);
    }
    for (uint32_t it = 0; it < cnt / 4; it++) {
        const uint32_t src = scr + 64 * it, dst = oh + 32 * it;
        uint32_t w[16];
        for (int k = 0; k < 16; k++) w[k] = ld32(ls, src + 4 * k);
        uint16_t h[16];
        for (int k = 0; k < 16; k++) {
            const uint32_t hi = w[k] >> 16, a = hi & 0x7FFF;
            uint32_t r = (0x4780u > a) ? ((w[k] - 0x38000000u) >> 13) & 0xFFFFu : 0x7BFFu;
            r = (r & 0x7FFFu) | (hi & 0x8000u);
            if (0x3800u > a) r = 0;
            h[k] = (uint16_t)r;
        }
        for (int k = 0; k < 16; k++) {
            ls[(dst + 2 * k) & SPU_LS_MASK] = (uint8_t)(h[k] >> 8);
            ls[(dst + 2 * k + 1) & SPU_LS_MASK] = (uint8_t)h[k];
        }
    }
    return 1;
}

/* ---- band attenuation 0x4180 (spec 1.7) ------------------------------------------------------------------- */

static inline u128 fbits(uint32_t b)
{
    return spu_splat_u32(b);
}

/* r4 = F (flags; k = (F & 60) >> 2), r5 = 9 SH quads, scaled in place: quads 1-3 by f1, 4-8 by f2. The lifted code
 * runs its float ops on full vectors that start as splats; the same helper ops on splats give lane 0. */
static int lp_atten_4180(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t buf = ctx->gpr[5]._u32[0];
    if ((buf & 15) || buf + 144 > SPU_LS_SIZE) return 0;
    const int32_t k = (int32_t)(ctx->gpr[4]._u32[0] & 60u) >> 2;
    const u128 one = fbits(0x3F800000u), m3 = fbits(0xC0400000u), zero = spu_fsmbi(0);
    u128 f1, f2;
    if (k == 0) {
        f2 = one;
        const u128 r37 = spu_fa(zero, zero), r38 = spu_fm(zero, m3), r36 = spu_fm(r37, zero);
        const u128 r35 = spu_fma(zero, r38, one);
        f1 = spu_fma(r36, zero, r35);
    } else {
        const u128 fif = fbits(0x41700000u); /* 15.0 */
        const u128 r19 = spu_csflt(spu_splat_u32((uint32_t)k), 155);
        const u128 r20 = spu_frest(fif), r18 = spu_fi(fif, r20);
        const u128 r17 = spu_fm(r19, r18), r16 = spu_fnms(r17, fif, r19), r15 = spu_fma(r16, r18, r17);
        const u128 t = spu_fs(one, r15);
        const u128 t2 = spu_fm(t, t), t4 = spu_fm(t2, t2);
        const u128 r8 = spu_fma(t4, fif, one);
        const u128 r9 = spu_frest(r8), r3 = spu_fi(r8, r9), r5 = spu_fm(one, r3), r7 = spu_fnms(r5, r8, one);
        const u128 s = spu_fma(r7, r3, r5);
        const u128 sp = spu_fm(s, one);
        const u128 twos = spu_fa(s, s);
        const u128 gt = spu_fcgt(twos, one);
        const u128 m = spu_selb(twos, one, gt);
        const u128 a1 = spu_fm(m, m3), a2 = spu_fa(m, m), a3 = spu_fma(m, a1, one), a4 = spu_fm(a2, m);
        f2 = spu_fma(a4, m, a3);
        if (spu_fcgt(one, sp)._u32[0] != 0) { /* 0x4218 then 0x423C: f1 from s */
            const u128 b1 = spu_fa(sp, sp), b2 = spu_fm(sp, m3), b3 = spu_fm(b1, sp), b4 = spu_fma(sp, b2, one);
            f1 = spu_fma(b3, sp, b4);
        } else
            f1 = zero; /* s >= 1.0: the lifted code keeps r14 = 0 */
    }
    for (int q = 1; q < 9; q++) {
        const u128 v = ldq(ls, buf + 16 * q);
        stq(ls, buf + 16 * q, spu_fm(v, q < 4 ? f1 : f2));
    }
    return 1;
}

/* ---- probe blend 0x47F0 (spec 1.5) -------------------------------------------------------------------------- */

/* Sign-extend and scale the 36 int16 SH coefficients of a probe: quad k lane m = int16 at p + 16 + 8k + 2m, / 4096. */
static inline void probe_decode(const uint8_t* ls, uint32_t p, u128* o)
{
    for (int k = 0; k < 9; k++) {
        u128 iv;
        for (int m = 0; m < 4; m++) {
            const uint32_t a = (p + 16 + 8 * k + 2 * m) & SPU_LS_MASK;
            iv._u32[m] = (uint32_t)(int32_t)(int16_t)(uint16_t)((ls[a] << 8) | ls[a + 1]);
        }
        o[k] = spu_csflt(iv, 143);
    }
}

/* r3 = S, r4 = query quad (a register), r5 = 9 output quads. The lifted function has a 1968-byte frame; apart from
 * its back-chain, the saved r80/r81/r0 and the kd search's result quad (and the kd stack below the frame), what it
 * writes there is register-spill scratch that is dead after the return (tools/spu/native_diff.py masks it for this
 * kind and says so). The result is written to the output quads only. */
static int lp_blend_47F0(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t sp0 = ctx->gpr[1]._u32[0], S = ctx->gpr[3]._u32[0], out9 = ctx->gpr[5]._u32[0];
    const u128 q = ctx->gpr[4];
    if (((sp0 | S | out9) & 15) || sp0 < 1968 + 544 || sp0 + 0x20 > SPU_LS_SIZE || S + 32 > SPU_LS_SIZE ||
        out9 + 144 > SPU_LS_SIZE)
        return 0;
    const uint32_t spn = sp0 - 1968;
    const uint32_t n = ld32(ls, S), base = ld32(ls, S + 20);
    if (overlaps(out9, 144, spn - 544, 1968 + 544 + 0x20) || overlaps(out9, 144, S, 32) ||
        (n > 4 && overlaps(out9, 144, base, 96 * n)))
        return 0;
    /* the kd search (it writes its result quad at spn + 32 and its stack below spn) */
    const u128 sv1 = ctx->gpr[1], sv3 = ctx->gpr[3], sv4 = ctx->gpr[4], sv5 = ctx->gpr[5];
    ctx->gpr[1] = spu_a(sv1, spu_il(-1968)); /* every lane, as the lifted prologue */
    ctx->gpr[4] = spu_splat_u32(spn + 32);
    ctx->gpr[5] = q;
    const int ok = lp_kd_42D0(ctx);
    ctx->gpr[1] = sv1;
    ctx->gpr[3] = sv3;
    ctx->gpr[4] = sv4;
    ctx->gpr[5] = sv5;
    if (!ok) return 0;
    const u128 ptrs = ldq(ls, spn + 32);
    int count = 4;
    while (count > 0 && ptrs._u32[count - 1] == 0) count--;
    if (count == 0) return 0; /* never happens (n >= 1); the lifted body runs its own path */
    u128 dec[4][9];
    uint32_t pt[4];
    for (int i = 0; i < count; i++) {
        pt[i] = ptrs._u32[i];
        if ((pt[i] & 15) || pt[i] + 96 > SPU_LS_SIZE || overlaps(out9, 144, pt[i], 96)) return 0;
    }
    const u128 c_pi = fbits(0x3F800001u), c_half = fbits(0x3F000000u), c_inv = fbits(0x3C000000u),
               one = fbits(0x3F800000u), c_min = fbits(0x38800000u);
    u128 w[4], sum = spu_il(0);
    for (int i = 0; i < count; i++) {
        const u128 pp = ldq(ls, pt[i]);
        const u128 d = spu_fs(q, pp), s = spu_fm(d, d);
        __m128 sv;
        memcpy(&sv, &s, 16);
        const __m128 yv = _mm_shuffle_ps(sv, sv, _MM_SHUFFLE(0, 3, 2, 1));
        const __m128 zv = _mm_shuffle_ps(sv, sv, _MM_SHUFFLE(1, 0, 3, 2));
        u128 y, z;
        memcpy(&y, &yv, 16);
        memcpy(&z, &zv, 16);
        const u128 t = spu_fa(spu_fa(s, y), z);
        const u128 d2 = spu_splat_u32(t._u32[0]);
        if (spu_fcgt(d2, c_min)._u32[0] == 0) { /* an exact hit: this probe's coefficients, unchanged (0x4C9C) */
            u128 o[9];
            probe_decode(ls, pt[i], o);
            for (int k = 0; k < 9; k++) stq(ls, out9 + 16 * k, o[k]);
            goto frame;
        }
        const u128 r55 = spu_frsqest(d2), r53 = spu_fi(d2, r55), r51 = spu_fm(r53, d2), r50 = spu_fnms(r53, r51, c_pi);
        const u128 r52 = spu_fm(r51, c_half), r49 = spu_fma(r50, r52, r51), r47 = spu_fs(r49, c_inv);
        const u128 r48 = spu_frest(r47), r46 = spu_fi(r47, r48), r45 = spu_fm(one, r46), r44 = spu_fnms(r45, r47, one);
        w[i] = spu_fma(r44, r46, r45);
        sum = spu_fa(sum, w[i]);
    }
    {
        const u128 rs = spu_frest(sum), r20 = spu_fi(sum, rs);
        u128 acc[9];
        for (int i = 0; i < count; i++) {
            const u128 r17 = spu_fm(w[i], r20), r16 = spu_fnms(r17, sum, w[i]), wn = spu_fma(r16, r20, r17);
            probe_decode(ls, pt[i], dec[i]);
            if (i == 0)
                for (int k = 0; k < 9; k++) acc[k] = spu_fm(dec[0][k], wn);
            else
                for (int k = 0; k < 9; k++) acc[k] = spu_fma(dec[i][k], wn, acc[k]);
        }
        for (int k = 0; k < 9; k++) stq(ls, out9 + 16 * k, acc[k]);
    }
frame:
    stq(ls, spn, ctx->gpr[1]); /* back-chain: the caller's r1 */
    stq(ls, sp0 - 0x20, ctx->gpr[81]);
    stq(ls, sp0 - 0x10, ctx->gpr[80]);
    stq(ls, sp0 + 0x10, ctx->gpr[0]);
    return 1;
}

/* Switch names (spelled out for tools/envsw.py, which does not parse the '|' strings): KZ3_NATIVE_JOB_LIGHTPROBES,
 * KZ3_NATIVE_JOB_LP_KD, KZ3_NATIVE_JOB_LP_BAKE, KZ3_NATIVE_JOB_LP_BLEND, KZ3_NATIVE_JOB_LP_ATTEN, KZ3_NATIVE_JOB_LP_LIGHT. */
#define LP_ENV_KD "KZ3_NATIVE_JOB_LIGHTPROBES|KZ3_NATIVE_JOB_LP_KD"
#define LP_ENV_BAKE "KZ3_NATIVE_JOB_LIGHTPROBES|KZ3_NATIVE_JOB_LP_BAKE"
#define POOL_LP                                                                                                        \
    0x69A0u, 0x6B50u, 0x19cbe0ffe412482full /* constants of both kernels: patterns, direction masks, insertion tables */

/* ---- dynamic-light SH 0x4DD0 (spec 1.6, decoded from the lifted code in pass 125) ----------------------------------
 * r3 = L (64-byte light element), r4 = query quad (a register), r5 = out9 (9 quads); returns r3 = 1 (contributes) or 0.
 * The light's kind is the halfword at L+0x3C (bytes 12-13 of quad L+0x30; the spec's "0x3A" is the rotqbyi 10 source
 * of the preferred halfword, which is bytes 2-3 of the result = L+0x3C): 0 point (code 0x4F90..0x52E8, tail 0x5380),
 * 1 spot (0x5678..0x58B8, tail 0x58B8), any other value directional (0x4DD0 straight on, tail 0x4F20). The three copies
 * share one tail: n = (x, y, z), F[k] = fm(Y_k(n), bw[k]) for k = 0..8 (Y_k as the jump table 0x4F6C / 0x53CC / 0x5904
 * entries compute it, bw[] = LS[0x6A60 + 4k]), out9[k] = fm(splat F[k], colour) lanewise (colour = LQ(L+0x10), scaled
 * first for point and spot). The lifted function spills its work into a 1760-byte frame (dead after the return); this
 * kernel writes only the back-chain at its bottom and out9, and native_diff_harness.c masks the rest of the frame for
 * kind lp_light, as for lp_blend.
 *   directional: v = (L+0x30, +0x34, +0x38), n = (-v) * rsq(|v|^2), rsq = frsqest, fi, one Newton step (below).
 *   point: d = fs(LQ(L), q); d2 = |d|^2; dist = (d2 > 0x00800000) ? d2 * rsq(d2) : 0; t = fma(dist, W0, W1) (W0, W1 =
 *     L+0x20, +0x24); t = max(min(t, 1), 0); sm = t*t*(3 - 2t); sm <= 0 -> return 0; n = d * recip(dist) (fma(refined
 *     recip, d, 0)); colour = fm(colour, sm).
 *   spot: as point up to d, no d2 threshold: rs = rsq(d2); dn = d * rs; dist = recip(rs); cos = -(dn . a) with a = (L+0x30,
 *     +0x34, +0x38), summed (x+y)+z; td = fma(dist, W2, W3) (L+0x28, +0x2C), tc = fma(cos, W0, W1); both clamped and
 *     smoothstepped as above; att = sum over the first three lanes of table[L+0x3E] * (1, rs, rs^2, rs^2), table = LS
 *     0x6A10 / 0x6A00 / 0x69F0 for 0 / 1 / 2 (constant, 1/dist, 1/dist^2 falloff); total = (sm_d * att) * sm_c; total <=
 *     0 -> return 0; n = dn; colour = fm(colour, total). A value above 2 in L+0x3E reads uninitialised frame words in the
 *     lifted code: declined. */
static inline u128 sp(uint32_t b)
{
    return spu_splat_u32(b);
}
static inline u128 mk4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a;
    r._u32[1] = b;
    r._u32[2] = c;
    r._u32[3] = d;
    return r;
}

/* (x2 + y2) + z2 of lanes 0..2 as splat, with the lifted code's op order */
static inline u128 dot3_splat(u128 s)
{
    __m128 sv;
    memcpy(&sv, &s, 16);
    const __m128 yv = _mm_shuffle_ps(sv, sv, _MM_SHUFFLE(0, 3, 2, 1));
    const __m128 zv = _mm_shuffle_ps(sv, sv, _MM_SHUFFLE(1, 0, 3, 2));
    u128 y, z;
    memcpy(&y, &yv, 16);
    memcpy(&z, &zv, 16);
    return sp(spu_fa(spu_fa(s, y), z)._u32[0]);
}

/* rsq(d2) = fma(fnms(d2, r*r, 1) , r*0.5, r) with r = fi(d2, frsqest(d2)) */
static inline u128 rsq_nr(u128 d2)
{
    const u128 one = sp(0x3F800000u), half = sp(0x3F000000u);
    const u128 r5 = spu_fi(d2, spu_frsqest(d2));
    const u128 r23 = spu_fm(r5, r5), r4 = spu_fm(r5, half), r28 = spu_fnms(d2, r23, one);
    return spu_fma(r28, r4, r5);
}

/* refined reciprocal: fma(fnms(r, a, 1), r, r) with r = fi(a, frest(a)) */
static inline u128 recip_nr(u128 a)
{
    const u128 one = sp(0x3F800000u);
    const u128 r = spu_fi(a, spu_frest(a));
    return spu_fma(spu_fnms(r, a, one), r, r);
}

/* clamp to [0, 1], then t*t*(3 - 2t) */
static inline u128 smooth01(u128 t)
{
    const u128 one = sp(0x3F800000u), zero = spu_il(0);
    const u128 hi = spu_selb(t, one, spu_fcgt(t, one));
    const u128 c = spu_selb(zero, hi, spu_fcgt(hi, zero));
    const u128 w = spu_fnms(c, sp(0x40000000u), sp(0x40400000u));
    return spu_fm(spu_fm(c, c), w);
}

static int lp_light_4DD0(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t L = ctx->gpr[3]._u32[0], out9 = ctx->gpr[5]._u32[0], sp0 = ctx->gpr[1]._u32[0];
    if (((L | out9 | sp0) & 15) || L + 64 > SPU_LS_SIZE || out9 + 144 > SPU_LS_SIZE || sp0 < 1760 || sp0 > SPU_LS_SIZE)
        return 0;
    const uint32_t spn = sp0 - 1760;
    if (overlaps(out9, 144, spn, 1760) || overlaps(out9, 144, L, 64) || overlaps(L, 64, spn, 1760)) return 0;
    const u128 q = ctx->gpr[4];
    const u128 one = sp(0x3F800000u), zero = spu_il(0);
    const uint32_t w3c = ld32(ls, L + 0x3C), kind = w3c >> 16;
    u128 color = ldq(ls, L + 0x10);
    u128 nx, ny, nz;
    if (kind == 0) { /* point */
        const u128 d = spu_fs(ldq(ls, L), q);
        const u128 d2 = dot3_splat(spu_fm(d, d));
        const u128 dist = spu_selb(zero, spu_fm(rsq_nr(d2), d2), spu_fcgt(d2, sp(0x00800000u)));
        const u128 w = ldq(ls, L + 0x20);
        const u128 sm = smooth01(spu_fma(dist, sp(w._u32[0]), sp(w._u32[1])));
        if (spu_fcgt(sm, zero)._u32[0] == 0) goto zero_ret;
        const u128 nv = spu_fma(recip_nr(dist), d, zero);
        nx = sp(nv._u32[0]);
        ny = sp(nv._u32[1]);
        nz = sp(nv._u32[2]);
        color = spu_fm(color, sm);
    } else if (kind == 1) { /* spot */
        const uint32_t idx = w3c & 0xFFFFu;
        if (idx > 2) return 0;
        const u128 d = spu_fs(ldq(ls, L), q);
        const u128 d2 = dot3_splat(spu_fm(d, d));
        const u128 rs = rsq_nr(d2);
        const u128 dn = spu_fm(d, rs);
        const u128 dist = recip_nr(rs);
        const u128 a = ldq(ls, L + 0x30);
        const u128 neg = sp(0x80000000u);
        const u128 av = spu_xor(neg, mk4(a._u32[0], a._u32[1], a._u32[2], a._u32[2]));
        const u128 cosv = dot3_splat(spu_fm(dn, av));
        const u128 w = ldq(ls, L + 0x20);
        const u128 td = spu_fma(dist, sp(w._u32[2]), sp(w._u32[3]));
        const u128 tc = spu_fma(cosv, sp(w._u32[0]), sp(w._u32[1]));
        const u128 rs2 = spu_fm(rs, rs);
        const u128 pw = mk4(0x3F800000u, rs._u32[0], rs2._u32[0], rs2._u32[0]);
        const u128 tab = ldq(ls, idx == 0 ? 0x6A10 : idx == 1 ? 0x6A00 : 0x69F0);
        const u128 att = dot3_splat(spu_fm(tab, pw));
        const u128 sm_d = smooth01(td), sm_c = smooth01(tc);
        const u128 total = spu_fm(spu_fm(sm_d, att), sm_c);
        if (spu_fcgt(total, zero)._u32[0] == 0) goto zero_ret;
        nx = sp(dn._u32[0]);
        ny = sp(dn._u32[1]);
        nz = sp(dn._u32[2]);
        color = spu_fm(color, total);
    } else { /* directional */
        const u128 a = ldq(ls, L + 0x30);
        const u128 neg = sp(0x80000000u);
        const u128 nvv = spu_xor(neg, mk4(a._u32[0], a._u32[1], a._u32[2], a._u32[2]));
        const u128 d2 = dot3_splat(spu_fm(nvv, nvv));
        const u128 nv = spu_fm(nvv, rsq_nr(d2));
        nx = sp(nv._u32[0]);
        ny = sp(nv._u32[1]);
        nz = sp(nv._u32[2]);
    }
    {
        /* real SH basis y,z,x order of the jump table entries; products left to right as the lifted code does */
        const u128 c1 = sp(0x3EFA2A1Bu), c1m = sp(0xBEFA2A1Bu), c2 = sp(0x3F8BD8A0u), c2m = sp(0xBF8BD8A0u);
        u128 Y[9];
        Y[0] = sp(0x3E906EBAu);
        Y[1] = spu_fm(ny, c1m);
        Y[2] = spu_fm(nz, c1);
        Y[3] = spu_fm(nx, c1m);
        Y[4] = spu_fm(ny, spu_fm(nx, c2));
        Y[5] = spu_fm(nz, spu_fm(ny, c2m));
        Y[6] = spu_fm(spu_fms(nz, spu_fm(nz, sp(0x40400000u)), one), sp(0x3EA17B00u));
        Y[7] = spu_fm(nz, spu_fm(nx, c2m));
        Y[8] = spu_fm(spu_fms(nx, nx, spu_fm(ny, ny)), sp(0x3F0BD8A0u));
        for (int k = 0; k < 9; k++) {
            const u128 F = spu_fm(Y[k], sp(ld32(ls, 0x6A60 + 4 * k)));
            stq(ls, out9 + 16 * k, spu_fm(sp(F._u32[0]), color));
        }
    }
    stq(ls, spn, ctx->gpr[1]); /* back-chain */
    ctx->gpr[3] = spu_il(1);
    return 1;
zero_ret:
    stq(ls, spn, ctx->gpr[1]);
    ctx->gpr[3] = spu_il(0);
    return 1;
}

#define LP_ENV_LIGHT "KZ3_NATIVE_JOB_LIGHTPROBES|KZ3_NATIVE_JOB_LP_LIGHT"
#define LP_ENV_BLEND "KZ3_NATIVE_JOB_LIGHTPROBES|KZ3_NATIVE_JOB_LP_BLEND"
#define LP_ENV_ATTEN "KZ3_NATIVE_JOB_LIGHTPROBES|KZ3_NATIVE_JOB_LP_ATTEN"

const kz3_native_entry kz3_native_lp[] = {
    {8, LP_ENV_KD, "Gfx.LightProbes", "job_0116d380", 0x42D0u, lp_kd_42D0, "lp_kd", POOL_LP, 1},
    {9, LP_ENV_BAKE, "Gfx.LightProbes", "job_0116d380", 0x45E8u, lp_bake_45E8, "lp_bake", POOL_LP, 1},
    {10, LP_ENV_KD, "Gfx.LightProbes", "job_010f0f00", 0x42D0u, lp_kd_42D0, "lp_kd", POOL_LP},
    {11, LP_ENV_BAKE, "Gfx.LightProbes", "job_010f0f00", 0x45E8u, lp_bake_45E8, "lp_bake", POOL_LP},
    {12, LP_ENV_BLEND, "Gfx.LightProbes", "job_0116d380", 0x47F0u, lp_blend_47F0, "lp_blend", POOL_LP, 1},
    {13, LP_ENV_ATTEN, "Gfx.LightProbes", "job_0116d380", 0x4180u, lp_atten_4180, "lp_atten", POOL_LP, 1},
    {14, LP_ENV_BLEND, "Gfx.LightProbes", "job_010f0f00", 0x47F0u, lp_blend_47F0, "lp_blend", POOL_LP},
    {16, LP_ENV_LIGHT, "Gfx.LightProbes", "job_0116d380", 0x4DD0u, lp_light_4DD0, "lp_light", POOL_LP, 1},
    {17, LP_ENV_LIGHT, "Gfx.LightProbes", "job_010f0f00", 0x4DD0u, lp_light_4DD0, "lp_light", POOL_LP},
    {15, LP_ENV_ATTEN, "Gfx.LightProbes", "job_010f0f00", 0x4180u, lp_atten_4180, "lp_atten", POOL_LP},
};
const unsigned kz3_native_lp_n = sizeof kz3_native_lp / sizeof kz3_native_lp[0];
