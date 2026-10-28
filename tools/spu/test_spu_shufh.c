/* Patch 1108 host test: spu_shufb1_h(a, spu_shufb1_dec(c), c) (the hoisted one-source shufb) against the byte-loop
 * reference spu_shufb_ref(a, a, c): every control byte value in every lane position (with random other lanes), then
 * random vectors; and a negative control (bit 7 dropped from the index) that must differ. Built twice by the .sh
 * (default and -DSPU_SHUFB_HOIST=0, where spu_shufb1_h is spu_shufb1). No game data. */
#include "spu_helpers.h"
#include <stdio.h>
#include <string.h>

int g_kz3_spu_xr, g_kz3_spu_slowcg, g_kz3_fma_stats, g_kz3_xr_stats, g_kz3_spu_xrq = 1;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra) { (void)op; (void)m; (void)a; (void)b; (void)c; (void)r; (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s; (void)ftz; }

static uint64_t s = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }

int main(void)
{
    long n = 0, bad = 0, neg = 0;
    struct { u128 gpr[2]; } ctxs, *ctx = &ctxs;   /* SPU_SHUFB1_DEC(n) reads ctx->gpr[n], as in lifted code */
    for (int pass = 0; pass < 2; pass++)
        for (long k = 0; k < (pass ? 4000000L : 256L * 16 * 64); k++) {
            u128 a;
            for (int i = 0; i < 4; i++) { a._u32[i] = rnd(); ctx->gpr[1]._u32[i] = rnd(); }
            if (!pass) ctx->gpr[1]._u8[(k >> 6) & 15] = (uint8_t)(k >> 10);   /* every byte value x lane position */
            SPU_SHUFB1_DEC(1)
            u128 got = spu_shufb1_h(a, spu__sd1, ctx->gpr[1]);
            u128 want = spu_shufb_ref(a, a, ctx->gpr[1]);
            n++;
            if (memcmp(&got, &want, 16)) {
                if (bad++ < 5) printf("differs: c=%08X %08X %08X %08X\n", ctx->gpr[1]._u32[0], ctx->gpr[1]._u32[1],
                                      ctx->gpr[1]._u32[2], ctx->gpr[1]._u32[3]);
            }
#if SPU_SHUFB_HOIST && defined(__SSE4_1__)
            /* negative control: the 1082 index without bit 7 (special lanes not zeroed before the OR) */
            spu_shufb1_d d = spu__sd1;
            d.idx = _mm_and_si128(d.idx, _mm_set1_epi8(0x7F));
            u128 wrong = spu_shufb1_h(a, d, ctx->gpr[1]);
            if (memcmp(&wrong, &want, 16)) neg++;
#endif
        }
    printf("test_spu_shufh SPU_SHUFB_HOIST=%d: %ld vectors, %ld differ; negative control differs in %ld\n",
           SPU_SHUFB_HOIST, n, bad, neg);
#if SPU_SHUFB_HOIST && defined(__SSE4_1__)
    if (neg == 0) { printf("negative control did not differ: the test cannot see a wrong decode\n"); return 1; }
#endif
    return bad != 0;
}
