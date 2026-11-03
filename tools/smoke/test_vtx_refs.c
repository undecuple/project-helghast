/* Host test for KZ3 patch 2139: rsx_vertex_refs_from_indices (the compact fetch's index loop) against the
 * per-index loop it replaced (big-endian u16/u32 read, restart index -> cut, else a {index, base} ref).
 * Random index streams, restart on/off, adjacent and edge restarts, odd counts. No game data, no GPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

/* rsx_vertex_compact.c links these three, this test never calls them: stubs (host tests stay self-contained) */
u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out) { (void)rsx; (void)index; memset(out, 0, sizeof(*out)); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4]) { (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static unsigned long long rs = 88172645463325252ull;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 16); }

/* the old loop: refs and the n_refs at which each cut was recorded */
static unsigned ref_loop(rsx_vertex_ref* refs, unsigned* cuts, unsigned* ncuts, const unsigned char* s, unsigned count,
                         int is_u32, int rest_en, unsigned rest, unsigned base)
{
    unsigned n = 0; *ncuts = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char* p = s + (size_t)i * (is_u32 ? 4u : 2u);
        const unsigned idx = is_u32 ? (((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3])
                                    : (unsigned)((p[0] << 8) | p[1]);
        if (rest_en && idx == rest) { cuts[(*ncuts)++] = n; continue; }
        refs[n].vertex_id = idx; refs[n].base_index = base; n++;
    }
    return n;
}
/* the new loop as the walker drives it */
static unsigned new_loop(rsx_vertex_ref* refs, unsigned* cuts, unsigned* ncuts, const unsigned char* s, unsigned count,
                         int is_u32, int rest_en, unsigned rest, unsigned base)
{
    unsigned n = 0, i = 0; *ncuts = 0;
    const unsigned esz = is_u32 ? 4u : 2u;
    while (i < count) {
        const unsigned got = rsx_vertex_refs_from_indices(refs + n, s + (size_t)i * esz, count - i, is_u32, rest_en, rest, base);
        n += got; i += got;
        if (i >= count) break;
        cuts[(*ncuts)++] = n; i++;
    }
    return n;
}
int main(void)
{
    unsigned fails = 0, cases = 0;
    for (int iter = 0; iter < 4000; iter++) {
        const int is_u32 = iter & 1, rest_en = (iter >> 1) & 1;
        const unsigned count = iter < 40 ? (unsigned)(iter >> 2) : rnd() % 3000u;   /* 0..9 first, then random */
        const unsigned esz = is_u32 ? 4u : 2u;
        const unsigned rest = is_u32 ? 0xFFFFFFFFu : 0xFFFFu;
        unsigned char* s = (unsigned char*)malloc((size_t)count * esz + 8);
        for (unsigned i = 0; i < count; i++) {
            unsigned v = rnd();
            const unsigned mode = rnd() % 8u;   /* mostly small indices, some restarts, some full-range */
            if (mode == 0) v = rest; else if (mode < 5) v &= 0x3FFu; else if (!is_u32) v &= 0xFFFEu;
            for (unsigned b = 0; b < esz; b++) s[i * esz + b] = (unsigned char)(v >> (8u * (esz - 1u - b)));
        }
        rsx_vertex_ref* a = (rsx_vertex_ref*)malloc((count + 1u) * sizeof *a);
        rsx_vertex_ref* b = (rsx_vertex_ref*)malloc((count + 1u) * sizeof *b);
        unsigned* ca = (unsigned*)malloc((count + 1u) * 4u); unsigned* cb = (unsigned*)malloc((count + 1u) * 4u);
        unsigned na_c, nb_c; const unsigned base = rnd();
        const unsigned na = ref_loop(a, ca, &na_c, s, count, is_u32, rest_en, rest, base);
        const unsigned nb = new_loop(b, cb, &nb_c, s, count, is_u32, rest_en, rest, base);
        cases++;
        if (na != nb || na_c != nb_c || memcmp(a, b, (size_t)na * sizeof *a) || memcmp(ca, cb, (size_t)na_c * 4u)) {
            fails++; printf("[FAIL] iter %d u32=%d restart=%d count=%u: refs %u/%u cuts %u/%u\n", iter, is_u32, rest_en, count, na, nb, na_c, nb_c);
        }
        free(s); free(a); free(b); free(ca); free(cb);
    }
    printf(fails ? "[FAIL] test_vtx_refs: %u of %u cases differ\n" : "[PASS] test_vtx_refs: %u of %u cases differ\n", fails, cases);
    return fails ? 1 : 0;
}
