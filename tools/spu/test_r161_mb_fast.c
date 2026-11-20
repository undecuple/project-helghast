#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>
#include "spu_helpers.h"
int g_kz3_spu_xr = 1; int g_kz3_xr_stats = 0;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r, const void* ra) { (void)op;(void)m;(void)a;(void)b;(void)c;(void)r;(void)ra; }
#include MBFAST_H
static uint64_t st = 88172645463325252ull;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)(st >> 11); }
int main(void)
{
    long bad = 0, slow = 0, n = 20000000;
    printf("test_r161_mb_fast: scalar estimate pair and cfltu low byte against the four-lane helpers\n");
    for (long i = 0; i < n; i++) {
        uint32_t ra = rnd() ^ (rnd() << 21);
        if (i % 5 == 0) ra = (ra & 0x807FFFFF) | ((rnd() % 4 == 0 ? (rnd() & 0xFF) : 0x60 + rnd() % 0x40) << 23);
        int sl = 0; uint32_t r = mb_rec_scalar(ra, &sl);
        u128 s0 = spu_splat_u32(ra); u128 rec = spu_fi(s0, spu_frest(s0));
        if (sl) { slow++; continue; }
        if (rec._u32[0] != r) { if (bad++ < 6) printf("rec mismatch ra=%08x mine=%08x helper=%08x\n", ra, r, rec._u32[0]); }
    }
    printf("rec (fi(S0, frest(S0)) lane 0): %ld bad, %ld slow of %ld\n", bad, slow, n);
    long bad2 = 0, sl2 = 0;
    for (long i = 0; i < n; i++) {
        uint32_t w[4]; for (int k = 0; k < 4; k++) { w[k] = rnd() ^ (rnd() << 21); if (i % 3 == 0) w[k] = (w[k] & 0x807FFFFF) | ((0x70 + rnd() % 0x30) << 23); }
        u128 q; memcpy(&q, w, 16); __m128 qv; memcpy(&qv, w, 16); __m128i bytes;
        u128 ref = spu_cfltu(q, 165);
        if (!mb_cflt_bytes(qv, &bytes)) { sl2++; continue; }
        uint32_t o[4]; memcpy(o, &bytes, 16);
        for (int k = 0; k < 4; k++) if ((o[k] & 0xFF) != (ref._u32[k] & 0xFF)) { if (bad2++ < 6) printf("cflt mismatch q=%08x mine=%02x ref=%08x\n", w[k], o[k]&0xff, ref._u32[k]); }
    }
    printf("cflt (random exponents): %ld bad, %ld slow of %ld\n", bad2, sl2, n);
    long bad3 = 0, fast3 = 0;
    for (long i = 0; i < n; i++) {   /* the range the blur produces: 0 .. a few, some negatives, zeros, tiny values */
        float f[4]; for (int k = 0; k < 4; k++) { f[k] = (float)(rnd() % 100000) / 20000.0f - (i % 7 == 0 ? 0.5f : 0.0f); if (i % 11 == 0 && k == 1) f[k] = 0.0f; if (i % 13 == 0 && k == 2) f[k] = 1e-40f; if (i % 17 == 0 && k == 3) f[k] = 8388607.0f; }
        u128 q; memcpy(&q, f, 16); __m128 qv; memcpy(&qv, f, 16); __m128i bytes;
        u128 ref = spu_cfltu(q, 165);
        if (!mb_cflt_bytes(qv, &bytes)) continue;
        fast3++;
        uint32_t o[4]; memcpy(o, &bytes, 16);
        for (int k = 0; k < 4; k++) if ((o[k] & 0xFF) != (ref._u32[k] & 0xFF)) { if (bad3++ < 6) printf("cflt mismatch f=%g mine=%02x ref=%08x\n", f[k], o[k]&0xff, ref._u32[k]); }
    }
    printf("cflt (blur range): %ld bad, %ld took the fast path of %ld\n", bad3, fast3, n);
    bad2 += bad3; if (fast3 < n / 2) bad2++;
    printf(bad || bad2 ? "FAIL\n" : "PASS\n");
    return bad || bad2;
}
