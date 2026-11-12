/* kz3_native_snd.c -- Snd.Mixer (ovl259 "synth_submix": 1.14 job_0125e300, 1.00 twin job_011e1180, byte-identical
 * kernels at the same LS addresses) kernels as native host code (SPU pass 159, plan: the RE notes "Native audio
 * plan" of pass 157). The 1.14 K7a gain ramp (slot 26) is on by default since 2026-11-11 (same-run toggle A/B: about
 * 14 % faster per call; native_diff on 24 real records: equal); K3 and the 1.00 twins stay opt-in. KZ3_NATIVE_JOB_SNDMIX=1/0
 * turns every kernel below on/off, _SND_K7A / _SND_K3 one each (a single switch beats the group one).
 *
 * Every float op is the runtime's SPU helper (spu_fa/fm/fma/cfltu/cuflt, FTZ, exact-zero sums, extended-range
 * recompute) applied to the same operands in the same order as the SPU's dataflow, on quads where the SPU works on quads
 * (no re-association, accumulated updates kept accumulated); constant tables are read from the local store at call time.
 * Bit-exact with the lifted code by construction and by tools/spu/native_diff.py (kinds "snd_k7a" and "snd_k3") and
 * tools/spu/test_r159_native_k7a.sh / test_r159_native_k3.sh. Guards (return 0, the lifted code runs): counts that
 * would make the SPU loop run 2^32 times, unaligned pointers, ranges past the local store or overlapping in a way that
 * changes the result of the SPU's read-ahead schedule.
 * Registers: the kernels leave r2-r79 as they were (SPU ABI volatile; ASSUMPTION: no caller reads them, as for the
 * Gfx.Post kernels), K3 returns r3 (the new phase) exactly as the SPU's quad. */
#include "kz3_native_jobs.h"
#include "spu_helpers.h"

#include <string.h>

/* local-store quad access without the runtime's debug probes (kz3_native_run already falls back to the lifted code when
 * a debug gate is armed); the address is masked to the quad like the SPU's lqx/stqx */
static inline u128 ldq(const spu_context* ctx, uint32_t a)
{
    const uint8_t* p = ctx->ls + (a & SPU_LS_MASK & ~0xFu);
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    return v;
}
static inline void stq(spu_context* ctx, uint32_t a, u128 v)
{
    uint8_t* p = ctx->ls + (a & SPU_LS_MASK & ~0xFu);
    for (int i = 0; i < 4; i++) {
        p[4 * i] = (uint8_t)(v._u32[i] >> 24); p[4 * i + 1] = (uint8_t)(v._u32[i] >> 16);
        p[4 * i + 2] = (uint8_t)(v._u32[i] >> 8); p[4 * i + 3] = (uint8_t)v._u32[i];
    }
}

static inline int ovl(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

/* ---- K7a: mono -> mono gain ramp (DD80 prologue + DE40/DE48 loop, 16 samples per iteration) ------------------------
 * r3 src, r4 dst (LS, quad aligned float buffers), r5 count (floats, multiple of 16), r6 g0, r7 step (word 0 of each).
 * Early return (nothing written) if src == 0, dst == 0 or g0 == 0.0 && step == 0.0 (fcmeq: -0 counts). Gains
 * G_j = fma(step, tab_j, g0), tab = the constants 0..15 at LS 0x16B20 (four quads), per 16 samples G_j = fa(step16, G_j),
 * step16 = fm(16.0f, step) (accumulated); per quad j: dst = fma(src, G, dst) (operand order of the SPU's schedule:
 * j0 (s, G, d), j1-j2 (G, s, d), j3 (s, G, d)). The SPU loop loads the next 64 bytes of src and dst before it stores
 * the current 64: src and dst must not overlap (src == dst exactly is fine). */
static int mix_mono_ramp_DD80(spu_context* ctx)
{
    const uint32_t src = ctx->gpr[3]._u32[0], dst = ctx->gpr[4]._u32[0], cnt = ctx->gpr[5]._u32[0];
    const u128 g0 = spu_splat_u32(ctx->gpr[6]._u32[0]), step = spu_splat_u32(ctx->gpr[7]._u32[0]);
    const u128 zero = spu_il(0);
    if (src == 0 || dst == 0 || (spu_fcmeq(g0, zero)._u32[0] && spu_fcmeq(zero, step)._u32[0])) return 1;
    if (cnt == 0 || (cnt & 15u) || cnt > 0x10000u || ((src | dst) & 15u)) return 0;
    const uint32_t bytes = cnt * 4u;
    if ((uint64_t)src + bytes > SPU_LS_SIZE || (uint64_t)dst + bytes > SPU_LS_SIZE) return 0;
    if (src != dst && ovl(src, (uint64_t)bytes + 64, dst, (uint64_t)bytes + 64)) return 0;
    if (ovl(dst, bytes, 0x16B20u, 0x40u)) return 0;
    u128 G[4];
    for (int j = 0; j < 4; j++) G[j] = spu_fma(step, ldq(ctx, 0x16B20u + 16u * (uint32_t)j), g0);
    const u128 step16 = spu_fm(spu_splat_u32(0x41800000u), step);
    for (uint32_t b = 0; b < bytes; b += 64) {
        u128 o[4];
        for (int j = 0; j < 4; j++) {
            const u128 s = ldq(ctx, src + b + 16u * (uint32_t)j), d = ldq(ctx, dst + b + 16u * (uint32_t)j);
#ifdef KZ3_NATIVE_NEGCTL /* negative control: separate multiply and add instead of the single-rounded fma */
            o[j] = spu_fa(spu_fm(s, G[j]), d);
#else
            o[j] = (j == 0 || j == 3) ? spu_fma(s, G[j], d) : spu_fma(G[j], s, d);
#endif
        }
        for (int j = 0; j < 4; j++) {
            stq(ctx, dst + b + 16u * (uint32_t)j, o[j]);
            G[j] = spu_fa(step16, G[j]);
        }
    }
    return 1;
}

/* ---- K3: pitch resampler, 4-tap polyphase (CF80 prologue + D100/D2DC loop, 8 outputs per iteration) ----------------
 * r3 step (float, source samples per output), r4 phase0, r5 src (LS float block; the kernel works on src - 12: three
 * samples of history), r6 dst (LS floats), r7 n (outputs, multiple of 8; word 0 counts). Lane k of a group of four
 * outputs: p_k = fma(step, tab_k, phase0), tab = {0,1,2,3} at LS 0x16740; per group p += step4 (accumulated), step4 =
 * fm(4.0f, step). Tap row: LS[0x15600 + (cfltu(p,161) & 4095)] (the load masks the low four bits: 256 rows of four
 * floats); window: the four floats starting at byte (cfltu(p,171) + src - 12), built from two quads (A, A + 16) with
 * the pattern row sh = ((cfltu(p,169) + 4 ((src - 12) & 15)) & 63) >> 4 (LS 0x16670..0x166B0: row sh = words sh..sh+3
 * of the pair; the pool hash covers the rows). Per output the products m_j = fm(window_j, tap_j); the SPU transposes
 * four outputs' products with shufb/selb (LS 0x16620..0x16660, 0x16730) and sums tap-wise, **(t1 + t3) + (t2 + t0)**
 * (the RE notes "Pass 159": the pairing is (0,2),(1,3), the specs' (0+1)+(2+3) ASSUMPTION was wrong; it is read off
 * the patterns and the sum tree D100 r10 = fa(r10, r34), r34 = fa(r39, r36), r10 = fa(r34, r10)). Group output quad ->
 * dst + 16 g. Return r3 = fs(pend, cuflt(cfltu(pend, 173), 155)), pend = fma(step, cuflt(r7, 155), phase0) over the full
 * r7 quad (the SPU's other lanes), i.e. the fractional part of the end phase.
 * The SPU loads the windows of a group before the previous groups are stored: dst must not overlap any window read
 * (checked by a position pre-pass) nor the constants and the tap table (LS 0x15600..0x16760). */
static int resample_CF80(spu_context* ctx)
{
    const uint32_t src = ctx->gpr[5]._u32[0], dst = ctx->gpr[6]._u32[0], n = ctx->gpr[7]._u32[0];
    if (n == 0 || (n & 7u) || n > 0x10000u || (dst & 15u) || (src & 3u) || src < 16u) return 0;
    if ((uint64_t)dst + 4ull * n > SPU_LS_SIZE || ovl(dst, 4ull * n, 0x15600u, 0x1160u)) return 0;
    const u128 step = spu_splat_u32(ctx->gpr[3]._u32[0]), phase0 = spu_splat_u32(ctx->gpr[4]._u32[0]);
    const uint32_t base = src - 12u, off4 = 4u * (base & 15u);
    const u128 tab = ldq(ctx, 0x16740u);
    const u128 step4 = spu_fm(spu_splat_u32(0x40800000u), step);
    const u128 p0 = spu_fma(tab, step, phase0);
    /* pre-pass: the byte range of every window read (addresses cfltu(p,171) + base, two quads each), by the
     * monotone-or-not accumulation itself (min and max over all groups and lanes) */
    {
        uint64_t lo = ~0ull, hi = 0;
        u128 p = p0;
        for (uint32_t g = 0; g < n / 4; g++) {
            const u128 a = spu_cfltu(p, 171);
            for (int k = 0; k < 4; k++) {
                if ((uint64_t)a._u32[k] + base + 32 > SPU_LS_SIZE) return 0; /* a window would wrap round the local store: the lifted code */
                const uint64_t A = (uint64_t)((a._u32[k] + base) & ~0xFu);
                if (A < lo) lo = A;
                if (A + 32 > hi) hi = A + 32;
            }
            p = spu_fa(step4, p);
        }
        if (ovl(dst, 4ull * n, lo, hi - lo)) return 0;
    }
    u128 p = p0;
    for (uint32_t g = 0; g < n / 4; g++) {
        const u128 f161 = spu_cfltu(p, 161), f169 = spu_cfltu(p, 169), f171 = spu_cfltu(p, 171);
        u128 m[4];
        for (int k = 0; k < 4; k++) {
            const u128 taps = ldq(ctx, 0x15600u + (f161._u32[k] & 4095u));
            const uint32_t A = f171._u32[k] + base;
            const u128 q0 = ldq(ctx, A), q1 = ldq(ctx, A + 16u);
            const uint32_t sh = ((f169._u32[k] + off4) & 63u) >> 4;
            u128 w;
            for (uint32_t i = 0; i < 4; i++) w._u32[i] = (i + sh < 4) ? q0._u32[i + sh] : q1._u32[i + sh - 4];
            m[k] = spu_fm(w, taps);
        }
        u128 t[4];
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) t[j]._u32[k] = m[k]._u32[j];
#ifdef KZ3_NATIVE_NEGCTL /* negative control: the spec's (t0 + t1) + (t2 + t3) pairing */
        const u128 out = spu_fa(spu_fa(t[0], t[1]), spu_fa(t[2], t[3]));
#else
        const u128 out = spu_fa(spu_fa(t[1], t[3]), spu_fa(t[2], t[0]));
#endif
        stq(ctx, dst + 16u * g, out);
        p = spu_fa(step4, p);
    }
    const u128 pend = spu_fma(step, spu_cuflt(ctx->gpr[7], 155), phase0);
    ctx->gpr[3] = spu_fs(pend, spu_cuflt(spu_cfltu(pend, 173), 155));
    return 1;
}

#define SND_ENV_GROUP "KZ3_NATIVE_JOB_SNDMIX"
#define SND_ENV_K7A SND_ENV_GROUP "|KZ3_NATIVE_JOB_SND_K7A"
#define SND_ENV_K3 SND_ENV_GROUP "|KZ3_NATIVE_JOB_SND_K3"
#define POOL_K7A 0x16B20u, 0x16B60u, 0x1cc013bcff943729ull /* tab 0..15 (native_gates: hash over LS words, SPU pass 159) */
#define POOL_K3 0x16620u, 0x16750u, 0x01a2e8dcf4ca6b28ull  /* transpose/select patterns, window rows, tab 0..3 */

/* Gates (re/spu/native_gates.tsv): slot, flags, job, image, LS address, kernel, family, constant pool, default. */
const kz3_native_entry kz3_native_snd[] = {
    {26, SND_ENV_K7A, "Snd.Mixer", "job_0125e300", 0xDD80u, mix_mono_ramp_DD80, "snd_k7a", POOL_K7A, 1},
    {27, SND_ENV_K7A, "Snd.Mixer", "job_011e1180", 0xDD80u, mix_mono_ramp_DD80, "snd_k7a", POOL_K7A, 0},
    {28, SND_ENV_K3, "Snd.Mixer", "job_0125e300", 0xCF80u, resample_CF80, "snd_k3", POOL_K3, 0},
    {29, SND_ENV_K3, "Snd.Mixer", "job_011e1180", 0xCF80u, resample_CF80, "snd_k3", POOL_K3, 0},
};
const unsigned kz3_native_snd_n = sizeof kz3_native_snd / sizeof kz3_native_snd[0];
