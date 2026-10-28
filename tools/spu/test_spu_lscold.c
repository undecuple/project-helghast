/* Patch 1107 host test: the lifted code's LS accessors (spu_ls_read128_l / spu_ls_write128_l, patches 1082/1100) with
 * the gated side out of line (SPU_LS_COLD=1, the default) or inlined (=0, the old form; the .sh builds both) give the
 * same values and the same LS image as a byte-wise big-endian reference, on randomised LS images, with the debug gate
 * clear and set, at addresses on the mask, alignment and WWS-probe boundaries and at random addresses. No game data. */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* runtime symbols the full (gated) accessor uses that the header only declares; the header's own gate globals
 * keep their initial values (probes read their env, empty here; the WWS probes only act for image 2) */
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
}

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }

static u128 ref_read(const uint8_t* ls, uint32_t lsa)
{
    u128 v; lsa &= 0x3FFF0u;
    for (int i = 0; i < 4; i++)
        v._u32[i] = (uint32_t)ls[lsa + 4*i] << 24 | (uint32_t)ls[lsa + 4*i + 1] << 16 |
                    (uint32_t)ls[lsa + 4*i + 2] << 8 | ls[lsa + 4*i + 3];
    return v;
}
static void ref_write(uint8_t* ls, uint32_t lsa, u128 v)
{
    lsa &= 0x3FFF0u;
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++) ls[lsa + 4*i + b] = (uint8_t)(v._u32[i] >> (24 - 8*b));
}

int main(void)
{
    static const uint32_t edge[] = {0, 1, 0xF, 0x10, 0x11, 0xDEF, 0xDF0, 0xDF1, 0xEAF, 0xEB0, 0x131F, 0x1320, 0x1321,
                                    0x3FFE0, 0x3FFEF, 0x3FFF0, 0x3FFFF, 0x40000, 0x40001, 0x4000F, 0x40010, 0x7FFF0,
                                    0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu};
    const int ne = (int)(sizeof edge / sizeof edge[0]);
    spu_context* ctx = calloc(1, sizeof *ctx);
    uint8_t* ls = aligned_alloc(64, 0x40000);
    uint8_t* rl = aligned_alloc(64, 0x40000);
    ctx->ls = ls;
    long bad = 0, n = 0;
    for (int img = 0; img < 64; img++) {
        for (int i = 0; i < 0x40000; i++) ls[i] = (uint8_t)rnd();
        memcpy(rl, ls, 0x40000);
        for (int k = 0; k < 20000; k++) {
            const uint32_t lsa = k < ne * 4 ? edge[k % ne] + (uint32_t)(k / ne) * 0x10u * (k & 1) : rnd();
            const int dbg = (int)(rnd() & 3);
            u128 got = spu_ls_read128_l(ctx, ls, dbg, lsa), want = ref_read(rl, lsa);
            n++; if (memcmp(&got, &want, 16)) { if (bad++ < 5) printf("read lsa=0x%X dbg=%d differs\n", lsa, dbg); }
            u128 v; for (int i = 0; i < 4; i++) v._u32[i] = rnd();
            const uint32_t wa = (k & 3) ? rnd() : edge[(k >> 2) % ne];
            spu_ls_write128_l(ctx, ls, (int)(rnd() & 3), wa, v);
            ref_write(rl, wa, v);
        }
        n++; if (memcmp(ls, rl, 0x40000)) { if (bad++ < 5) printf("LS image %d differs after the writes\n", img); }
    }
    printf("test_spu_lscold SPU_LS_COLD=%d: %ld checks, %ld differ\n", SPU_LS_COLD, n, bad);
    return bad != 0;
}
