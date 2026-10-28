/* Patch 2080 host test: nv3089_rows_copy (libs/video/rsx_nv3089_rows.h) against the per-pixel loop of
 * cellGcmSys.c's nv3089_blit (copied below with vm_read/vm_write as the big-endian byte swaps of ppu_memory.h),
 * over random unscaled blits: bpp 1/2/4, clips, sub-pixel u0/v0, pitches smaller than a row, overlapping source
 * and destination, and addresses near the 4 GB wrap (the helper must then decline). Every case must leave the
 * memory byte-identical, and the helper must decline exactly when it cannot match (counted, not required).
 * Usage: tools/smoke/test_nv3089_rows.sh (Linux, no game data). */
#include "rsx_nv3089_rows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM (1u << 20)
static uint8_t A[MEM], B[MEM];
static uint32_t rnd(void) { static uint64_t s = 0x9E3779B97F4A7C15ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }

/* cellGcmSys.c nv3089_blit, CPU loop (byte swaps cancel); addresses wrap at 32 bits, masked into the buffer. */
static void ref_copy(uint8_t* m, uint32_t src, uint32_t in_pitch, uint32_t u0, uint32_t v0, uint32_t dst,
                     uint32_t dst_pitch, uint32_t out_x, uint32_t out_y, uint32_t out_w, uint32_t out_h,
                     uint32_t cx0, uint32_t cx1, uint32_t cy0, uint32_t cy1, uint32_t bpp)
{
    for (uint32_t y = 0; y < out_h; y++) {
        uint32_t dy = out_y + y;
        if (dy < cy0 || dy >= cy1) continue;
        uint64_t sv = ((uint64_t)v0 << 16) + (uint64_t)y * 0x100000u;   /* 12.4 point -> 12.20 (patch 2083) */
        uint32_t sy = (uint32_t)(sv >> 20);
        for (uint32_t x = 0; x < out_w; x++) {
            uint32_t dx = out_x + x;
            if (dx < cx0 || dx >= cx1) continue;
            uint64_t su = ((uint64_t)u0 << 16) + (uint64_t)x * 0x100000u;
            uint32_t sx = (uint32_t)(su >> 20);
            uint32_t s = src + sy * in_pitch + sx * bpp, d = dst + dy * dst_pitch + dx * bpp;
            uint8_t t[4];
            for (uint32_t k = 0; k < bpp; k++) t[k] = m[(s + k) & (MEM - 1u)];
            for (uint32_t k = 0; k < bpp; k++) m[(d + k) & (MEM - 1u)] = t[k];
        }
    }
}

static int refuse(uint32_t lo, uint64_t n) { (void)lo; (void)n; return 0; }

int main(void)
{
    unsigned copied = 0, declined = 0, empty = 0, bad = 0, refused_ok = 0;
    for (int it = 0; it < 20000; it++) {
        for (uint32_t i = 0; i < MEM; i += 4) { uint32_t r = rnd(); memcpy(A + i, &r, 4); }
        memcpy(B, A, MEM);
        const uint32_t bpp = (uint32_t[]){1, 2, 4}[rnd() % 3];
        const uint32_t out_w = 1 + rnd() % 64, out_h = 1 + rnd() % 32;
        const uint32_t out_x = rnd() % 16, out_y = rnd() % 16;
        const uint32_t in_pitch = (rnd() % 4 == 0) ? rnd() % 64 : out_w * bpp + rnd() % 64;
        const uint32_t dst_pitch = (rnd() % 4 == 0) ? rnd() % 64 : (out_x + out_w) * bpp + rnd() % 64;
        /* 12.4 origins up to 64 px (patch 2083: whole-pixel origins matter now; they keep the source in the buffer) */
        const uint32_t u0 = rnd() % (64u << 4), v0 = rnd() % (32u << 4);
        uint32_t src = rnd() % (MEM / 2), dst = (rnd() % 3 == 0) ? src + (rnd() % 512) - 256 : rnd() % (MEM / 2);
        if (rnd() % 50 == 0) dst = 0xFFFFF000u + rnd() % 0x1000u;   /* wraps: must decline */
        uint32_t cx0 = 0, cx1 = 0xFFFFFFFFu, cy0 = 0, cy1 = 0xFFFFFFFFu;
        if (rnd() % 2) { cx0 = rnd() % 32; cx1 = cx0 + rnd() % 64; cy0 = rnd() % 24; cy1 = cy0 + rnd() % 32; }
        uint64_t bytes = 0;
        const int done = (dst >= MEM / 2 + 0x40000u) ? 0 :   /* outside the test buffer: only the reference runs */
            nv3089_rows_copy(B, src, in_pitch, u0, v0, dst, dst_pitch, out_x, out_y, out_w, out_h, cx0, cx1, cy0,
                             cy1, bpp, &bytes, NULL);
        if (!done) { memcpy(B, A, MEM); ref_copy(B, src, in_pitch, u0, v0, dst, dst_pitch, out_x, out_y, out_w, out_h,
                                                 cx0, cx1, cy0, cy1, bpp); declined++; }
        else if (bytes) copied++; else empty++;
        ref_copy(A, src, in_pitch, u0, v0, dst, dst_pitch, out_x, out_y, out_w, out_h, cx0, cx1, cy0, cy1, bpp);
        if (memcmp(A, B, MEM)) {
            if (bad++ < 5) fprintf(stderr, "[test_nv3089_rows] MISMATCH it=%d bpp=%u %ux%u at %u,%u src=0x%X/%u dst=0x%X/%u\n",
                                   it, bpp, out_w, out_h, out_x, out_y, src, in_pitch, dst, dst_pitch);
        }
        if (it % 500 == 0) {   /* the store-hook predicate declines */
            if (nv3089_rows_copy(B, 0, 64, 0, 0, MEM / 2, 64, 0, 0, 4, 4, 0, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4, &bytes,
                                 refuse) == 0) refused_ok++;
            else bad++;
            /* last row ends past 4 GB: must decline before touching memory (the base is never dereferenced) */
            if (nv3089_rows_copy(B, 0, 64, 0, 0, 0xFFFFFF00u, 64, 0, 0, 16, 8, 0, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4,
                                 &bytes, NULL) == 0) refused_ok++;
            else bad++;
        }
    }
    /* Patch 2083: the source origin against the IMAGE_IN_POINT definition, not the loop's formula: u (low 16) and
     * v (high 16) are 12.4, so the destination pixel (x, y) of a 1:1 blit reads source pixel (u >> 4) + x,
     * (v >> 4) + y -- what the GPU mirror in nv3089_blit uses ((in_uv & 0xFFFF) >> 4). Each source pixel holds its
     * own coordinates; origins 0..40 px with the half-pixel centre (+8) and other fractions. Before 2083 the copies
     * read column u0 >> 12, so every origin >= 16 px failed here. */
    unsigned spec = 0;
    for (uint32_t org = 0; org <= 40; org += 5) {
        for (uint32_t fr = 0; fr < 16; fr += 8) {
            const uint32_t u0 = (org << 4) | fr, v0 = ((org / 2) << 4) | fr, in_pitch = 128 * 4, dst = MEM / 2;
            for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 128; x++) {
                const uint32_t id = (y << 16) | x; memcpy(A + y * in_pitch + x * 4, &id, 4);
            }
            memcpy(B, A, MEM);
            uint64_t bytes = 0;
            const int done = nv3089_rows_copy(B, 0, in_pitch, u0, v0, dst, 16 * 4, 0, 0, 16, 8, 0, 0xFFFFFFFFu, 0,
                                              0xFFFFFFFFu, 4, &bytes, NULL);
            ref_copy(A, 0, in_pitch, u0, v0, dst, 16 * 4, 0, 0, 16, 8, 0, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4);
            for (uint32_t y = 0; y < 8; y++) for (uint32_t x = 0; x < 16; x++) {
                const uint32_t want = ((y + (v0 >> 4)) << 16) | (x + (u0 >> 4));
                uint32_t a, b; memcpy(&a, A + dst + y * 64 + x * 4, 4); memcpy(&b, B + dst + y * 64 + x * 4, 4);
                if (a != want || !done || b != want) {
                    if (spec++ < 3) fprintf(stderr, "[test_nv3089_rows] ORIGIN u0=0x%X v0=0x%X (%u,%u): want 0x%08X"
                                            " loop 0x%08X rows 0x%08X\n", u0, v0, x, y, want, a, b);
                }
            }
        }
    }
    bad += spec;
    printf("[test_nv3089_rows] copied=%u empty=%u declined=%u refused=%u origin-bad=%u bad=%u\n", copied, empty,
           declined, refused_ok, spec, bad);
    if (bad || copied < 1000) { printf("[test_nv3089_rows] FAIL\n"); return 1; }
    printf("[test_nv3089_rows] PASS\n");
    return 0;
}
