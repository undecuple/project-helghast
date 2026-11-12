/* native_diff_harness.c -- native gate vs lifted function (SPU pass 122; KZ2's native_diff_harness design).
 * Built and driven by tools/spu/native_diff.py. Reuses job_diff_harness.c's runtime contract and trial setup
 * (main renamed), links the image's lifted spu_recomp.c and port/spu/kz3_native_post.c + kz3_native_dof.c.
 *
 * Usage: harness <image.bin> <base> <slot> <seed> <trials> [record.kz3nj ...]
 *   For the slot's gate entry (image, LS address, kernel, kind, constant pool): the pool hash must match the image;
 *   then per state - synthetic (random LS and registers, kind-specific argument registers; <trials> of them) or a
 *   record written by KZ3_NATIVE_REC - run the lifted function (gate array empty) on one copy and the native kernel
 *   on another, and compare the whole local store, r0, r1 and r80..r127 (the registers a caller may keep; the lifted
 *   body also clobbers r2..r79, which the native kernel leaves untouched: call sites read none, kz3_native_post.c).
 *   Prints `R <what> equal=<n> differ=<n> declined=<n> lifted_nr=<n>` and the first difference.
 */
#define main jdh_main
#include <math.h>
#include <time.h>
#include "job_diff_harness.c"
#undef main
#include "kz3_native_jobs.h"

int (*g_kz3_native_gate[64])(spu_context*);

static const kz3_native_entry* find_entry(int slot)
{
    for (unsigned i = 0; i < kz3_native_post_n; i++)
        if (kz3_native_post[i].slot == slot) return &kz3_native_post[i];
    for (unsigned i = 0; i < kz3_native_dof_n; i++)
        if (kz3_native_dof[i].slot == slot) return &kz3_native_dof[i];
    for (unsigned i = 0; i < kz3_native_lp_n; i++)
        if (kz3_native_lp[i].slot == slot) return &kz3_native_lp[i];
    for (unsigned i = 0; i < kz3_native_skin_n; i++)
        if (kz3_native_skin[i].slot == slot) return &kz3_native_skin[i];
    for (unsigned i = 0; i < kz3_native_snd_n; i++)
        if (kz3_native_snd[i].slot == slot) return &kz3_native_snd[i];
    return NULL;
}

static uint64_t fnv_pool(const uint8_t* ls, uint32_t lo_, uint32_t hi_)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t a = lo_; a < hi_; a += 8) {
        uint64_t v;
        memcpy(&v, ls + a, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

/* Kind-specific synthetic arguments and data on top of setup()'s random LS and registers. */
static void synth_lp(spu_context* c, const char* kind, int variant);
static void wbe32(uint8_t* ls, uint32_t a, uint32_t v);
static void synth(spu_context* c, const char* kind, int variant)
{
    uint8_t* ls = c->ls;
    const uint32_t area = lo, span = 0x3E000 - lo;
    if (!strncmp(kind, "lp_", 3)) { synth_lp(c, kind, variant); return; }
    if (!strcmp(kind, "blend")) {
        const uint32_t n = 4 * (1 + rnd() % (variant % 7 == 0 ? 3 : 200));
        const uint32_t a = area + ((rnd() % (span / 2 - 4 * 800)) & ~15u);
        const uint32_t b = area + span / 2 + ((rnd() % (span / 2 - 4 * 800)) & ~15u);
        c->gpr[3] = spu_splat_u32(a);
        c->gpr[4] = spu_splat_u32(b);
        c->gpr[5] = spu_splat_u32(n);
        /* extreme pixel values now and then: coverage 0 / 255, channels 0 / 255 */
        if (variant % 3 == 1)
            for (uint32_t i = 0; i < 4 * n + 16; i++) {
                ls[a + i] = (rnd() & 1) ? 0 : 0xFF;
                ls[b + i] = (rnd() & 1) ? 0 : 0xFF;
            }
        if (variant % 3 == 2)
            for (uint32_t i = 0; i < n; i++) ls[a + 4 * i] = (rnd() & 1) ? 0 : (rnd() & 1) ? 0xFF : rnd() & 0xFF;
    }
    if (!strcmp(kind, "upsample8")) {
        const uint32_t wn = 8 * (1 + rnd() % (variant % 7 == 0 ? 2 : 24));
        const uint32_t sp = 16 * (((wn / 2 + 2) * 4 + 15) / 16 + rnd() % 8), dp = 4 * wn + 16 * (rnd() % 4);
        const uint32_t src = area + sp + 16 + ((rnd() % (span / 4)) & ~15u);
        const uint32_t dst = area + span / 2 + ((rnd() % (span / 4 - 2 * dp)) & ~15u);
        c->gpr[3] = spu_splat_u32(dst);
        c->gpr[4] = spu_splat_u32(src);
        c->gpr[5] = spu_splat_u32(sp);
        c->gpr[6] = spu_splat_u32(dp);
        c->gpr[7] = spu_splat_u32(wn);
        /* extreme values now and then: every byte 0 or 255 */
        if (variant % 3 == 1)
            for (int y = -1; y <= 1; y++)
                for (uint32_t i = 0; i < 4 * (wn / 2 + 2) + 32; i++)
                    ls[src + y * (int)sp - 4 + i] = (rnd() & 1) ? 0 : 0xFF;
    }
    if (!strcmp(kind, "dof_unpack")) {
        const uint32_t n = 4 * (1 + rnd() % (variant % 7 == 0 ? 3 : 150));
        const uint32_t d = area + ((rnd() % (span / 2 - 16 * 700)) & ~15u);
        const uint32_t s = area + span / 2 + ((rnd() % (span / 4 - 4 * 700)) & ~15u);
        const uint32_t w = area + span / 2 + span / 4 + ((rnd() % (span / 4 - 4 * 700)) & ~15u);
        c->gpr[3] = spu_splat_u32(d);
        c->gpr[4] = spu_splat_u32(s);
        c->gpr[5] = spu_splat_u32(w);
        c->gpr[6] = spu_splat_u32(n);
        if (variant % 3 == 1)
            for (uint32_t i = 0; i < 4 * n + 16; i++) {
                ls[s + i] = (rnd() & 1) ? 0 : 0xFF;
                ls[w + i] = (rnd() & 1) ? 0 : 0xFF;
            }
    }
    if (!strcmp(kind, "dof_gather")) {
        /* r3 dst, r4 scratch (this row's pixel 0), r5 CoC stream, r6 row buffer, r7 pitch (16 Wp; low halfword), r8 n.
         * Scratch: 14 rows of Wp float4 (6 rows above, 7 below, 6 + 6 pixels of slack), realistic floats (weight 0..2,
         * colours 0..8, some exact-zero weights) or setup()'s random bits (variant % 3 == 0: full-range patterns through
         * the helpers). CoC modes: all zero (skip path only), all 255, random, {0, 1, 37, 255}, alternating zero
         * groups, random with a third of the groups zero. variant % 4 == 3 puts garbage in r7's high halfword. */
        static const uint32_t wps[] = {40, 128, 256, 64, 20};
        const uint32_t Wp = wps[variant % 5];
        const uint32_t n = variant % 7 == 0 ? 4 * (1 + rnd() % 3) : 4 * (1 + rnd() % ((Wp - 12) / 4));
        const uint32_t scratch = area, src = scratch + 16 * (6 * Wp + 6), rowb = area + 0xE400, strm = area + 0xF800,
                       dst = area + 0xFC00;
        if (variant % 3) {
            for (uint32_t i = 0; i < 14 * Wp; i++)
                for (int k = 0; k < 4; k++) {
                    float f = k == 0 ? (float)(rnd() % 20001) / 10000.0f : (float)(rnd() % 80001) / 10000.0f;
                    if (k == 0 && variant % 11 == 0 && rnd() % 8 == 0) f = 0.0f;
                    uint32_t v;
                    memcpy(&v, &f, 4);
                    wbe32(ls, scratch + 16 * i + 4 * k, v);
                }
        }
        static const uint8_t few[4] = {0, 1, 37, 255};
        for (uint32_t i = 0; i < n; i++) {
            const int mode = variant % 6;
            uint8_t c = (uint8_t)rnd();
            if (mode == 0) c = 0;
            else if (mode == 1) c = 255;
            else if (mode == 3) c = few[rnd() % 4];
            else if (mode == 4 && (i / 4) % 2 == 0) c = 0;
            else if (mode == 5 && (i / 4) % 3 == 0) c = 0;
            ls[strm + 4 * i] = c;
        }
        uint32_t pitch = 16 * Wp;
        if (variant % 4 == 3) pitch |= rnd() << 16;
        c->gpr[3] = spu_splat_u32(dst);
        c->gpr[4] = spu_splat_u32(src);
        c->gpr[5] = spu_splat_u32(strm);
        c->gpr[6] = spu_splat_u32(rowb);
        c->gpr[7] = spu_splat_u32(pitch);
        c->gpr[8] = spu_splat_u32(n);
    }
    if (!strcmp(kind, "snd_k7a") || !strcmp(kind, "snd_k3")) {
        /* pass 159: Snd.Mixer K7a (r3 src, r4 dst, r5 count, r6 g0, r7 step; word 0 only, other lanes stay random) and K3
         * (r3 step, r4 phase0, r5 src, r6 dst, r7 n). Floats: realistic audio [-1, 1] and gains/steps, silent (exact
         * zero and -0) buffers, raw random bits, denormals; modes by variant: in place, overlapping (the kernel declines),
         * src or dst 0, g0 = step = +-0 (early return). */
        const int k7 = !strcmp(kind, "snd_k7a");
        const uint32_t half = span / 2;
        const uint32_t cnt = k7 ? 16 * (1 + rnd() % (variant % 5 == 0 ? 2 : 32)) : 8 * (1 + rnd() % (variant % 5 == 0 ? 2 : 48));
        const uint32_t sa = area + 0x100 + ((rnd() % (half - 0x4000)) & ~15u), da = area + half + ((rnd() % (half - 0x4000)) & ~15u);
        const int mode = variant % 11, raw = variant % 7 == 3, silent = variant % 6 == 0, den = variant % 13 == 5;
        const uint32_t len = k7 ? cnt : 4 * cnt + 64;  /* floats to fill at the source (K3: well past the last window) */
        uint32_t src = sa, dst = da;
        if (k7 && mode == 0) dst = src;
        if (mode == 1) dst = src + 16 * (1 + rnd() % 8);
        for (uint32_t i = 0; i < len + 16 && !raw; i++) {
            float f = ((float)(rnd() & 0xFFFFFF) / 8388608.0f) - 1.0f;
            uint32_t b; memcpy(&b, &f, 4);
            if (den) b = (b & 0x80000000u) | (rnd() & 0x007FFFFFu);
            if (silent) b = (rnd() & 3) ? 0 : 0x80000000u;
            wbe32(ls, src - (k7 ? 0 : 16) + 4 * i, b);
            if (k7 && dst != src) {
                float g = ((float)(rnd() & 0xFFFFFF) / 8388608.0f) - 1.0f;
                memcpy(&b, &g, 4);
                if (silent) b = (variant % 12 == 0) ? 0x80000000u : 0;
                wbe32(ls, dst + 4 * i, b);
            }
        }
        float f0, f1;
        if (k7) {
            f0 = (float)(rnd() & 0xFFFF) / 65536.0f;
            f1 = ((float)(rnd() & 0xFFFF) / 65536.0f - 0.5f) / 128.0f;
            if (mode == 4) f0 = f1 = 0.0f;
            if (mode == 5) { f0 = -0.0f; f1 = 0.0f; }
            if (mode == 6) f1 = 0.0f;
            if (mode == 7) f0 = 0.0f;
            if (mode == 2) src = 0;
            if (mode == 3) dst = 0;
        } else {
            f0 = (float)(rnd() & 0xFFFF) / 65536.0f * (variant % 4 == 1 ? 64.0f : 1.0f);   /* phase0 */
            f1 = 0.05f + (float)(rnd() & 0xFFFF) / 65536.0f * (variant % 3 == 0 ? 1.0f : 4.0f);   /* step */
            if (mode == 4) f1 = -f1;
            if (mode == 5) f1 = 0.0f;
            if (mode == 2) dst = src - 16 + 16 * (rnd() % 8);
            if (mode == 3) src += 1 + rnd() % 3;   /* unaligned: declined */
        }
        uint32_t b0, b1; memcpy(&b0, &f0, 4); memcpy(&b1, &f1, 4);
        if (raw) { b0 = rnd() ^ rnd() << 12; b1 = rnd() ^ rnd() << 12; }
        if (k7) {
            c->gpr[3]._u32[0] = src; c->gpr[4]._u32[0] = dst; c->gpr[5]._u32[0] = cnt; c->gpr[6]._u32[0] = b0; c->gpr[7]._u32[0] = b1;
        } else {
            c->gpr[3]._u32[0] = b1; c->gpr[4]._u32[0] = b0; c->gpr[5]._u32[0] = src; c->gpr[6]._u32[0] = dst; c->gpr[7]._u32[0] = cnt;
        }
    }
    if (!strcmp(kind, "skin")) {
        /* pass 141: r3 out (32 B per vertex), r4/r5 float4 streams, r6 record base (unaligned, records at r6 + 6 +
         * k r10), r7/r8 bone palettes (64 B per bone), r9 n, r10 stride. variant % 4 == 0 keeps setup()'s random bits
         * (NaN/Inf/denormal patterns through the helpers), else realistic data: rotations-ish 3x3 in [-1, 1],
         * translations in [-50, 50], positions [-2, 2], normals [-1, 1], weights with b0 + b1 <= 255 (some 255/0),
         * palette 2 = palette 1 (motion 0), a small offset (half under/normal range) or a large one (half clamp).
         * variant % 7 == 3: another stride; % 9 == 0: n 1..4; NATIVE_DIFF_N=<n> fixes n and realistic data (timing at a real size);
         * % 23 == 11: out over stream B (the kernel declines, the lifted code runs). */
        const uint32_t n = getenv("NATIVE_DIFF_N") ? (uint32_t)atoi(getenv("NATIVE_DIFF_N")) : variant % 9 == 0 ? 1 + rnd() % 4 : 1 + rnd() % 256;
        static const uint32_t strides[] = {16, 24, 32, 20};
        const uint32_t st = variant % 7 == 3 ? strides[rnd() % 4] : 20, nb = 1 + rnd() % 256;
        const uint32_t out = area, sa = area + 0x4000, sb = area + 0x6000, recb = area + 0x8000 + (rnd() & 15),
                       p1 = area + 0xC000, p2 = area + 0x10000;
        const int real = getenv("NATIVE_DIFF_N") || variant % 4; /* timing: realistic data only */
        if (real) {
            const int pm = variant % 3; /* palette 2: 0 same, 1 small offset, 2 large */
            /* variant % 5 == 4: bones and streams scaled by 2^-0..2^-33 each (around the kernel's exponent bound) */
            const float sM = variant % 5 == 4 ? ldexpf(1.0f, -(int)(rnd() % 34)) : 1.0f, sA = variant % 5 == 4 ? ldexpf(1.0f, -(int)(rnd() % 34)) : 1.0f;
            for (uint32_t b = 0; b < nb; b++)
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        float f = sM * (r < 3 ? ((float)(rnd() % 20001) / 10000.0f - 1.0f) : c < 3 ? ((float)(rnd() % 100001) / 1000.0f - 50.0f) : 1.0f);
                        if (r < 3 && c == 3) f = 0.0f;
                        uint32_t v;
                        memcpy(&v, &f, 4);
                        wbe32(ls, p1 + 64 * b + 16 * r + 4 * c, v);
                        float g = f;
                        if (pm == 1) g = f + (float)((int)(rnd() % 2001) - 1000) * 1e-7f;
                        if (pm == 2 && r == 3 && c < 3) g = f + (float)((int)(rnd() % 2001) - 1000) * 100.0f;
                        memcpy(&v, &g, 4);
                        wbe32(ls, p2 + 64 * b + 16 * r + 4 * c, v);
                    }
            for (uint32_t k = 0; k < n + 1; k++)
                for (int c = 0; c < 4; c++) {
                    float a = sA * (c < 3 ? (float)(rnd() % 40001) / 10000.0f - 2.0f : 1.0f), b = sA * (c < 3 ? (float)(rnd() % 20001) / 10000.0f - 1.0f : 0.0f);
                    uint32_t v;
                    memcpy(&v, &a, 4);
                    wbe32(ls, sa + 16 * k + 4 * c, v);
                    memcpy(&v, &b, 4);
                    wbe32(ls, sb + 16 * k + 4 * c, v);
                }
        }
        for (uint32_t k = 0; k < n + 1; k++) {
            uint8_t* q = ls + recb + 6 + k * st;
            if (real) {
                const uint32_t m = rnd() % 8;
                q[0] = m == 0 ? 255 : (uint8_t)rnd();
                q[1] = m == 0 ? 0 : (uint8_t)(rnd() % (256 - q[0]));
            }
            for (int b = 5; b < 8; b++) q[b] = (uint8_t)(rnd() % nb);
        }
        c->gpr[3] = spu_splat_u32(variant % 23 == 11 ? sb : out);
        c->gpr[4] = spu_splat_u32(sa);
        c->gpr[5] = spu_splat_u32(sb);
        c->gpr[6] = spu_splat_u32(recb);
        c->gpr[7] = spu_splat_u32(p1);
        c->gpr[8] = spu_splat_u32(p2);
        c->gpr[9] = spu_splat_u32(n);
        c->gpr[10] = spu_splat_u32(st);
        return;
    }
    if (!strcmp(kind, "mb_blur")) {
        /* r3 dst RGBA8, r4 float4 pixel 0 of the row (interior of a 14-row padded scratch: 6 rows above, 7 below, 6 + 6
         * pixels of slack), r5 velocity stream (4 B per pixel, one quad of prefetch past the end), r6 pitch (16 Wp; low
         * halfword), r7 n (pass 136). Floats: realistic (weight 0..17 with some exact zeros, colours 0..17) or setup()'s
         * random bits (variant % 3 == 0: full-range patterns, NaN/Inf/denormal bits through the helpers). Velocity modes:
         * zero vector (b1 = b2 = 128), bytes 0/255 only, random, b0 = 0, {0, 127, 128, 255} corners, random.
         * variant % 4 == 3 puts garbage in r6's high halfword. */
        static const uint32_t wps[] = {40, 128, 256, 64, 20};
        const uint32_t Wp = getenv("NATIVE_DIFF_WP") ? (uint32_t)atoi(getenv("NATIVE_DIFF_WP")) : wps[variant % 5];   /* pass 140: a real width for the timing */
        const uint32_t n = getenv("NATIVE_DIFF_WP") ? Wp - 12 : variant % 7 == 0 ? 4 * (1 + rnd() % 3) : 4 * (1 + rnd() % ((Wp - 12) / 4));
        const uint32_t scratch = area, src = scratch + 16 * (6 * Wp + 6), vel = getenv("NATIVE_DIFF_WP") ? ((area + 16 * 14 * Wp + 0x40) & ~15u) : area + 0xF800,
                       dst = getenv("NATIVE_DIFF_WP") ? ((vel + 4 * Wp + 0x80) & ~15u) : area + 0xFC00;
        if (variant % 3) {
            for (uint32_t i = 0; i < 14 * Wp; i++)
                for (int k = 0; k < 4; k++) {
                    float f = (float)(rnd() % 170001) / 10000.0f;
                    if (k == 0 && variant % 11 == 0 && rnd() % 8 == 0) f = 0.0f;
                    uint32_t v;
                    memcpy(&v, &f, 4);
                    wbe32(ls, scratch + 16 * i + 4 * k, v);
                }
        }
        static const uint8_t corner[4] = {0, 127, 128, 255};
        for (uint32_t i = 0; i < n + 4; i++) {
            const int mode = variant % 6;
            uint8_t b0 = (uint8_t)rnd(), b1 = (uint8_t)rnd(), b2 = (uint8_t)rnd(), b3 = (uint8_t)rnd();
            if (mode == 0) b1 = b2 = 128;
            else if (mode == 1) { b0 = (rnd() & 1) ? 0 : 255; b1 = (rnd() & 1) ? 0 : 255; b2 = (rnd() & 1) ? 0 : 255; }
            else if (mode == 3) b0 = 0;
            else if (mode == 4) { b1 = corner[rnd() % 4]; b2 = corner[rnd() % 4]; }
            ls[vel + 4 * i] = b0;
            ls[vel + 4 * i + 1] = b1;
            ls[vel + 4 * i + 2] = b2;
            ls[vel + 4 * i + 3] = b3;
        }
        uint32_t pitch = 16 * Wp;
        if (variant % 4 == 3) pitch |= rnd() << 16;
        c->gpr[3] = spu_splat_u32(dst);
        c->gpr[4] = spu_splat_u32(src);
        c->gpr[5] = spu_splat_u32(vel);
        c->gpr[6] = spu_splat_u32(pitch);
        c->gpr[7] = spu_splat_u32(n);
    }
}


static int g_cmp_r3;   /* snd_k3 returns r3 (the new phase): compared too */
static uint32_t g_dead_lo, g_dead_hi;   /* LS bytes of the kernel's own dead scratch frame (compare skips them) */

static void wbe32(uint8_t* ls, uint32_t a, uint32_t v)
{
    ls[a] = v >> 24; ls[a + 1] = v >> 16; ls[a + 2] = v >> 8; ls[a + 3] = (uint8_t)v;
}

/* a float with a random exponent in a useful range (some zeros, exponent-255 values and signs) */
static uint32_t rnd_float(int grid)
{
    const uint32_t r = rnd_word();
    if (grid) return ((r >> 8) % 17 > 8 ? 0x3F800000u : 0xBF800000u) + ((r & 7) << 22) * 0 + (((r >> 4) & 7) << 20);
    switch (r % 29) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return 0x7F800000u | (r & 0x80000000u) | (rnd_word() & 0x7FFFFF);
    case 3: return (rnd_word() & 0x807FFFFFu) | ((10 + rnd_word() % 10) << 23);
    default: return (rnd_word() & 0x807FFFFFu) | ((100 + rnd_word() % 50) << 23);
    }
}

static void synth_lp(spu_context* c, const char* kind, int variant)
{
    uint8_t* ls = c->ls;
    const uint32_t area = (lo + 15) & ~15u;
    if (!strcmp(kind, "lp_kd")) {
        const uint32_t n = variant % 9 == 0 ? rnd() % 7 : 5 + rnd() % (variant % 5 == 0 ? 600 : 90);
        const uint32_t S = area, out = area + 64, base = area + 0x100 + 16 * (rnd() % 64);
        const int grid = variant % 3 == 0;   /* coarse positions: many equal distances */
        wbe32(ls, S, n);
        wbe32(ls, S + 20, base);
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t v = grid ? (0x3F800000u + ((rnd() % 5) << 21)) | ((rnd() & 1) << 31) : rnd_float(0);
                wbe32(ls, base + 96 * i + 4 * k, v);
            }
            wbe32(ls, base + 96 * i + 12, rnd() % (variant % 11 == 0 ? 4 : 3));
        }
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[4] = spu_splat_u32(out);
        for (int k = 0; k < 4; k++) c->gpr[5]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        c->gpr[1]._u32[0] = 0x3F000;
    }
    if (!strcmp(kind, "lp_blend")) {
        const uint32_t n = variant % 8 == 0 ? 1 + rnd() % 4 : 5 + rnd() % (variant % 5 == 0 ? 400 : 60);
        const uint32_t S = area, out9 = area + 0x100, base = area + 0x200 + 16 * (rnd() % 16);
        const int grid = variant % 3 == 0;
        wbe32(ls, S, n);
        wbe32(ls, S + 20, base);
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t v = grid ? (0x3F800000u + ((rnd() % 5) << 21)) | ((rnd() & 1) << 31) : rnd_float(0);
                wbe32(ls, base + 96 * i + 4 * k, v);
            }
            wbe32(ls, base + 96 * i + 12, rnd() % 3);
            for (int k = 0; k < 72; k++) ls[base + 96 * i + 16 + k] = rnd() & 0xFF;
            if (variant % 4 == 1)   /* small coefficients and extremes */
                for (int k = 0; k < 36; k++) { ls[base + 96 * i + 16 + 2 * k] = (rnd() & 1) ? 0 : 0x7F + (rnd() & 1); }
        }
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[5] = spu_splat_u32(out9);
        for (int k = 0; k < 4; k++) c->gpr[4]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        /* the query on (or a hair from) a probe: exact-hit path and the 1/128 threshold */
        if (variant % 6 < 3) {
            const uint32_t j = rnd() % n;
            static const uint32_t dl[] = {0, 0, 0x3C000000u, 0x3C000001u, 0x3BFFFFFFu, 0x3C010000u, 0x3B800000u};
            for (int k = 0; k < 3; k++) {
                uint32_t pv = ((uint32_t)ls[base + 96 * j + 4 * k] << 24) | (ls[base + 96 * j + 4 * k + 1] << 16) |
                              (ls[base + 96 * j + 4 * k + 2] << 8) | ls[base + 96 * j + 4 * k + 3];
                c->gpr[4]._u32[k] = pv;
            }
            /* offset one axis by +delta using the float helpers' own add (host float is fine for a test input) */
            const uint32_t dd = dl[rnd() % 7];
            float f, g;
            memcpy(&f, &c->gpr[4]._u32[0], 4);
            memcpy(&g, &dd, 4);
            f += g;
            if (f == f && f - f == 0) memcpy(&c->gpr[4]._u32[0], &f, 4);
        }
        c->gpr[1]._u32[0] = 0x3F000;
        g_dead_lo = 0x3F000 - 1968 + 0x30;
        g_dead_hi = 0x3F000 - 1968 + 0x790;
    }
    if (!strcmp(kind, "lp_light")) {
        /* r3 = L (64 B), r4 = query quad, r5 = out9; the kind is the halfword at L+0x3C (0 point, 1 spot, else directional) */
        const uint32_t L = area + 0x100 + 16 * (rnd() % 8), out9 = area + 0x300 + 16 * (rnd() % 8);
        const int grid = variant % 3 == 0;
        const uint32_t kinds[] = {0, 0, 1, 1, 2, 7, 0xFFFF};
        const uint32_t kd = kinds[(variant / 2) % 7], sel = variant % 5 == 0 ? rnd() % 3 : (kd == 1 ? (rnd() % 8 == 0 ? 3 : rnd() % 3) : rnd() & 0xFFFF);
        for (int k = 0; k < 16; k++) {
            uint32_t v = rnd_float(grid);
            if (variant % 4 == 1 && (k >= 8 && k < 12)) v = 0x3F000000u + ((rnd() & 0xFF) << 12) + ((rnd() & 1) << 31);   /* W0..W3 of moderate size */
            if (variant % 4 == 2 && k < 3) v = 0x3F800000u + ((rnd() % 5) << 21);                                       /* positions near 1 */
            if (variant % 7 == 3 && k >= 4 && k < 8) v = 0x3F800000u;                                                   /* colour 1 */
            wbe32(ls, L + 4 * k, v);
        }
        if (variant % 4 == 3 || variant % 4 == 0) {
            /* tame values: lights near the query, smoothsteps inside (0, 1) most of the time (both paths pass often) */
            const float lo_[16] = {-4, -4, -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0}, hi_[16] = {4, 4, 4, 0, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 0};
            for (int k = 0; k < 16; k++) {
                float f = lo_[k] + (hi_[k] - lo_[k]) * (float)(rnd() & 0xFFFF) / 65535.0f;
                if (k == 8) f = 0.5f + 3.0f * (float)(rnd() & 0xFFFF) / 65535.0f;            /* W0 */
                if (k == 9) f = -2.0f + 2.5f * (float)(rnd() & 0xFFFF) / 65535.0f;           /* W1 */
                if (k == 10) f = -0.6f * (float)(rnd() & 0xFFFF) / 65535.0f;                 /* W2 */
                if (k == 11) f = 0.3f + 1.5f * (float)(rnd() & 0xFFFF) / 65535.0f;           /* W3 */
                uint32_t v;
                memcpy(&v, &f, 4);
                wbe32(ls, L + 4 * k, v);
            }
        }
        wbe32(ls, L + 0x3C, (kd << 16) | sel);
        c->gpr[3] = spu_splat_u32(L);
        c->gpr[5] = spu_splat_u32(out9);
        for (int k = 0; k < 4; k++) c->gpr[4]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        if (variant % 4 == 3 || variant % 4 == 0)
            for (int k = 0; k < 3; k++) {
                const float f = -3.0f + 6.0f * (float)(rnd() & 0xFFFF) / 65535.0f;
                memcpy(&c->gpr[4]._u32[k], &f, 4);
            }
        if (variant % 6 == 0)   /* query on the light: exact zero distance */
            for (int k = 0; k < 3; k++) c->gpr[4]._u32[k] = ((uint32_t)ls[L + 4 * k] << 24) | (ls[L + 4 * k + 1] << 16) | (ls[L + 4 * k + 2] << 8) | ls[L + 4 * k + 3];
        c->gpr[1]._u32[0] = 0x3F000;
        g_dead_lo = 0x3F000 - 1760 + 0x10;
        g_dead_hi = 0x3F000;
    }
    if (!strcmp(kind, "lp_atten")) {
        const uint32_t buf = area + 0x100 + 16 * (rnd() % 8);
        for (int k = 0; k < 36; k++) wbe32(ls, buf + 4 * k, rnd_float(0));
        c->gpr[5] = spu_splat_u32(buf);
        c->gpr[4]._u32[0] = (variant % 4 == 0) ? (rnd() & 0x3C) : rnd();
    }
    if (!strcmp(kind, "lp_bake")) {
        const uint32_t cnt = variant % 7 == 0 ? 4 * (rnd() % 3) : (rnd() & 1) ? 64 : 128;
        const uint32_t S = area, shp = area + 0x100, scr = area + 0x200, tbl = area + 0x1200 + 4 * (rnd() % 8),
                       oh = area + 0x9000;
        wbe32(ls, S + 28, scr);
        for (uint32_t k = 0; k < 36; k++) wbe32(ls, shp + 4 * k, rnd_float(0));
        for (uint32_t i = 0; i < 128 * 13; i++) wbe32(ls, tbl + 4 * i, rnd_float(0));
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[4] = spu_splat_u32(shp);
        c->gpr[5] = spu_splat_u32(tbl);
        c->gpr[6] = spu_splat_u32(cnt);
        c->gpr[7] = spu_splat_u32(oh);
    }
}

static int load_record(spu_context* c, const char* path, int slot)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char m[8];
    uint32_t h[4];
    int ok = fread(m, 1, 8, f) == 8 && !memcmp(m, KZ3_NJ_MAGIC, 8) && fread(h, 4, 4, f) == 4 && (int)h[0] == slot;
    spu_context_init(c, 0);
    ok = ok && fread(c->gpr, 16, 128, f) == 128 && fread(c->ls, 1, SPU_LS_SIZE, f) == SPU_LS_SIZE;
    fclose(f);
    if (ok) {
        c->image_id = 1;
        c->ls[SENTINEL + 2] = 0x3F;
        c->ls[SENTINEL + 3] = 0xFF;
        c->gpr[0] = spu_splat_u32(SENTINEL);
        c->gpr[0]._u32[1] = c->gpr[0]._u32[2] = c->gpr[0]._u32[3] = 0;
        c->pc = h[1];
    }
    return ok;
}

static int g_equal, g_differ, g_declined, g_nr, g_shown, g_drecs, g_drows;
static int g_cov[3][2];   /* lp_light: [point, spot, directional][returned 0, 1] among the compared states */

static void compare(const char* what, spu_context* a, spu_context* b)
{
    int bad = 0;
    long off = -1;
    if (g_dead_hi > g_dead_lo) memcpy(b->ls + g_dead_lo, a->ls + g_dead_lo, g_dead_hi - g_dead_lo);
    if (memcmp(a->ls, b->ls, SPU_LS_SIZE)) {
        bad = 1;
        for (long i = 0; i < SPU_LS_SIZE; i++)
            if (a->ls[i] != b->ls[i]) { off = i; break; }
    }
    int reg = -1;
    for (int r = 0; r < 128 && !bad; r++)
        if ((r < 2 || r >= 80 || (r == 3 && g_cmp_r3)) && memcmp(&a->gpr[r], &b->gpr[r], 16)) { bad = 1; reg = r; }
    if ((b->pc & SPU_LS_MASK) != (a->pc & SPU_LS_MASK) && !bad) { bad = 1; reg = -2; }
    if (!bad) { g_equal++; return; }
    g_differ++;
    if (g_shown++ < 5) printf("DIFF %s ls_off=0x%lX reg=%d pc a=%X b=%X\n", what, off, reg, a->pc, b->pc);
}

/* DOF gather rows from a slot-6 (unpack) record (pass 135). The record is the driver's state at the unpack call
 * (0x4C54): r82 = &D, r84 = scratch_int, r85 = dst, r86 = stream, r88 = 16 Wp (pitch), r80 = Wp * bpp. After the
 * unpack the driver (0x4C90) calls the gather for each row y < H = D1.hw3 with r3 = dst + y W bpp_out,
 * r4 = scratch_int + y pitch, r5 = stream + y r80, r6 = 0x62C0, r7 = pitch, r8 = W = D1.hw2 (bpp_out = D2.byte13).
 * The rows run in order on the same local store, as in the job; each row is one compared state. */
static void derive_gather(const char* path, fn_t fn, const kz3_native_entry* e)
{
    fn_t unpack = NULL;
    for (unsigned i = 0; i < nreg; i++) if (reg[i].addr == 0x5780) unpack = reg[i].fn;
    if (!unpack || !load_record(&A, path, 6)) { printf("R bad record %s\n", path); g_nr++; return; }
    A.ls = A.ls_store;
    if (run_lifted(&A, unpack) != R_RET) { printf("R %s: unpack did not return\n", path); g_nr++; return; }
    const uint32_t D = A.gpr[82]._u32[0] & SPU_LS_MASK;
    const uint32_t W = ((uint32_t)A.ls[D + 0x14] << 8) | A.ls[D + 0x15], H = ((uint32_t)A.ls[D + 0x16] << 8) | A.ls[D + 0x17];
    const uint32_t bpp_out = A.ls[D + 0x2D], dst0 = A.gpr[85]._u32[0], sc0 = A.gpr[84]._u32[0], st0 = A.gpr[86]._u32[0],
                   pitch = A.gpr[88]._u32[0], sstep = A.gpr[80]._u32[0];
    g_drecs++;
    for (uint32_t y = 0; y < H; y++) {
        char what[200];
        snprintf(what, sizeof what, "%s row %u/%u W=%u", path, y, H, W);
        A.gpr[3] = spu_splat_u32(dst0 + y * W * bpp_out);
        A.gpr[4] = spu_splat_u32(sc0 + y * pitch);
        A.gpr[5] = spu_splat_u32(st0 + y * sstep);
        A.gpr[6] = spu_splat_u32(0x62C0);
        A.gpr[7] = spu_splat_u32(pitch);
        A.gpr[8] = spu_splat_u32(W);
        A.gpr[0] = spu_splat_u32(SENTINEL);
        A.gpr[0]._u32[1] = A.gpr[0]._u32[2] = A.gpr[0]._u32[3] = 0;
        A.pc = e->addr;
        memcpy(&B, &A, sizeof A);
        B.ls = B.ls_store;
        A.ls = A.ls_store;
        memcpy(B.ls_store, A.ls_store, SPU_LS_SIZE);
        if (run_lifted(&A, fn) != R_RET) { printf("R %s: lifted gather did not return\n", what); g_nr++; return; }
        if (!e->fn(&B)) { g_declined++; continue; }
        B.pc = B.gpr[0]._u32[0];
        compare(what, &A, &B);
        g_drows++;
    }
}

static double g_tl, g_tn, g_tpx; static int g_tcalls;
int main(int argc, char** argv)
{
    if (argc < 6) { fprintf(stderr, "usage\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); image_len = ftell(f); fseek(f, 0, SEEK_SET);
    image = malloc(image_len);
    if (fread(image, 1, image_len, f) != (size_t)image_len) return 2;
    fclose(f);
    base = (uint32_t)strtoul(argv[2], 0, 0);
    if (base + (uint32_t)image_len > lo) lo = (base + (uint32_t)image_len + 0xFFF) & ~0xFFFu;
    const int slot = atoi(argv[3]);
    const uint64_t seed = strtoull(argv[4], 0, 0);
    const int trials = atoi(argv[5]);
    const kz3_native_entry* e = find_entry(slot);
    if (!e) { printf("R slot %d: no entry\n", slot); return 2; }
    extern void HARNESS_REGISTER(void);
    HARNESS_REGISTER();
#ifdef SPU_WWS_READ_CAP
    g_wws_read_probe = SPU_WWS_READ_CAP;
    g_wws_code_probe = SPU_WWS_CODE_CAP;
#endif
    spu_hop_init();
    signal(SIGALRM, on_alarm);
    fn_t fn = NULL;
    for (unsigned i = 0; i < nreg; i++) if (reg[i].addr == e->addr) fn = reg[i].fn;
    if (!fn) { printf("R slot %d: no lifted function 0x%X\n", slot, e->addr); return 2; }
    /* the image's pool must be what the native kernel expects (the gate's own check) */
    static spu_context P;
    setup(&P, e->addr, 1);
    if (fnv_pool(P.ls, e->pool_lo, e->pool_hi) != e->pool_hash) {
        printf("R slot %d %s: POOL HASH MISMATCH (image %s) have 0x%016llx\n", slot, e->kind, e->image, (unsigned long long)fnv_pool(P.ls, e->pool_lo, e->pool_hi));
        return 1;
    }
    g_cmp_r3 = !strcmp(e->kind, "snd_k3");
    const int nrec = argc - 6;
    for (int t = 0; t < trials + nrec; t++) {
        char what[96];
        if (t < trials) {
            setup(&A, e->addr, seed + (uint64_t)t * 7919);
            synth(&A, e->kind, t);
            snprintf(what, sizeof what, "synth#%d", t);
        } else {
            snprintf(what, sizeof what, "%s", argv[6 + t - trials]);
            if (!strcmp(e->kind, "dof_gather") && strstr(what, "s06_")) { derive_gather(what, fn, e); continue; }
            if (!load_record(&A, argv[6 + t - trials], slot)) { printf("R bad record %s\n", what); g_nr++; continue; }
        }
        if (!strcmp(e->kind, "lp_light") && t >= trials) {
            g_dead_lo = (A.gpr[1]._u32[0] - 1760 + 0x10) & SPU_LS_MASK;
            g_dead_hi = A.gpr[1]._u32[0] & SPU_LS_MASK;
        }
        if (!strcmp(e->kind, "lp_blend") && t >= trials) {
            g_dead_lo = (A.gpr[1]._u32[0] - 1968 + 0x30) & SPU_LS_MASK;
            g_dead_hi = (A.gpr[1]._u32[0] - 1968 + 0x790) & SPU_LS_MASK;
        }
        memcpy(&B, &A, sizeof A);
        B.ls = B.ls_store;   /* the copy owns its own local store */
        A.ls = A.ls_store;
        memcpy(B.ls_store, A.ls_store, SPU_LS_SIZE);
        uint32_t lkind = 0;
        if (!strcmp(e->kind, "lp_light")) {
            const uint32_t Lp = A.gpr[3]._u32[0] & SPU_LS_MASK;
            lkind = ((uint32_t)A.ls[Lp + 0x3C] << 8) | A.ls[Lp + 0x3D];
        }
        if (getenv("NATIVE_DIFF_TIME")) {   /* SPU pass 140: min of 5 warm runs each, per pixel (mb_blur) */
            static spu_context T0, T1;
            static uint8_t pr[SPU_LS_SIZE];
            memcpy(pr, A.ls_store, SPU_LS_SIZE);
            const spu_context sv = A;
            double bl = 1e18, bn = 1e18;
            for (int k = 0; k < 5; k++) {
                memcpy(&T0, &sv, sizeof sv); T0.ls = T0.ls_store; memcpy(T0.ls_store, pr, SPU_LS_SIZE);
                struct timespec a0, a1;
                clock_gettime(CLOCK_MONOTONIC, &a0);
                const int rr = run_lifted(&T0, fn);
                clock_gettime(CLOCK_MONOTONIC, &a1);
                if (rr != R_RET) { bl = -1; break; }
                const double dl = (a1.tv_sec - a0.tv_sec) * 1e9 + (a1.tv_nsec - a0.tv_nsec);
                if (dl < bl) bl = dl;
                memcpy(&T1, &sv, sizeof sv); T1.ls = T1.ls_store; memcpy(T1.ls_store, pr, SPU_LS_SIZE);
                clock_gettime(CLOCK_MONOTONIC, &a0);
                const int tk = e->fn(&T1);
                clock_gettime(CLOCK_MONOTONIC, &a1);
                if (!tk) { bn = -1; break; }
                const double dn = (a1.tv_sec - a0.tv_sec) * 1e9 + (a1.tv_nsec - a0.tv_nsec);
                if (dn < bn) bn = dn;
            }
            if (bl > 0 && bn > 0) {
                const double px = !strcmp(e->kind, "skin") ? sv.gpr[9]._u32[0] : sv.gpr[7]._u32[0];   /* per vertex (skin) or pixel */
                g_tl += bl; g_tn += bn; g_tpx += px; g_tcalls++;
            }
        }
        const int r = run_lifted(&A, fn);
        if (r != R_RET) { g_nr++; continue; }
        const int took = e->fn(&B);
        if (!took) { g_declined++; continue; }
        B.pc = B.gpr[0]._u32[0];
        compare(what, &A, &B);
        if (!strcmp(e->kind, "lp_light") && 1) {
            const int ret = B.gpr[3]._u32[0] == 1;
            const int ck = lkind == 0 ? 0 : lkind == 1 ? 1 : 2;
            g_cov[ck][ret]++;
        }
    }
    printf("R slot %d %s %s 0x%X: equal=%d differ=%d declined=%d lifted_nr=%d\n", slot, e->kind, e->image, e->addr,
           g_equal, g_differ, g_declined, g_nr);
    if (g_tcalls) printf("R time %d calls, %.0f px: lifted %.1f ns/px (%.1f us/call avg), native %.1f ns/px (%.1f us/call avg), native/lifted %.3f\n", g_tcalls, g_tpx, g_tl / g_tpx, g_tl / g_tcalls / 1e3, g_tn / g_tpx, g_tn / g_tcalls / 1e3, g_tn / g_tl);
    if (g_drecs) printf("R derived from %d slot-6 records: %d gather rows\n", g_drecs, g_drows);
    if (!strcmp(e->kind, "lp_light"))
        printf("R   coverage (returned 0 / 1): point %d / %d, spot %d / %d, directional %d / %d\n", g_cov[0][0], g_cov[0][1], g_cov[1][0], g_cov[1][1], g_cov[2][0], g_cov[2][1]);
    return g_differ ? 1 : 0;
}
