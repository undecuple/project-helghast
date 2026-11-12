/* Host test for KZ3 patch 2160 (RSX_PERF_VTXREG counters, rsx_vtx_reg.h): a scripted draw sequence over a fake guest memory
 * with known classes, and a random run checked against an independent byte-compare oracle. No game data, no GPU.
 * Built by test_vtx_reg.sh, which also builds a mutated header copy that this test must reject (negative control). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rsx_vtx_reg.h"

static uint8_t mem[2][1 << 20];
static int tracked[2][16];                       /* per 64 KB page: a tracked write noted */
static const uint8_t* ptr_cb(void* c, const rsx_vr_span* sp) { (void)c; return sp->loc > 1u ? NULL : mem[sp->loc] + sp->off; }
static int wr_cb(void* c, const rsx_vr_span* sp, uint32_t since)
{
    (void)c; (void)since;
    for (uint32_t p = sp->off >> 16; p <= (sp->off + sp->len - 1u) >> 16; p++) if (tracked[sp->loc][p]) return 1;
    return 0;
}
static int fails;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static rsx_vr_prev P; static rsx_vr_stat S; static uint32_t frame;
static int draw(uint32_t fmt, uint32_t first, rsx_vr_span a, rsx_vr_span b, uint32_t verts)
{
    const uint32_t key[6] = { 1, fmt, 7, 7, first, 2 };   /* 4 attribute words, then the batch words */
    rsx_vr_span sp[2] = { a, b };
    return rsx_vr_fetched(&P, &S, key, 6, 4, sp, 2, verts, frame, ptr_cb, wr_cb, NULL);
}

static void scripted(void)
{
    const rsx_vr_span A = { 0, 0x1000, 256 }, B = { 1, 0x20000, 128 }, A2 = { 0, 0x1000, 512 };
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_NEW_ATTR);          /* first draw */
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_SAME_CLEAN);        /* identical */
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_SAME_CLEAN);
    mem[0][0x1010] ^= 1;                                     /* untracked change (a PPU store) */
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_CHG_UNTRACKED);
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_SAME_CLEAN);        /* the copy was refreshed */
    mem[1][0x20010] ^= 1; tracked[1][2] = 1;                 /* tracked change in main memory */
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_CHG_TRACKED);
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_SAME_PAGE_FP);      /* page note still set, bytes equal */
    tracked[1][2] = 0;
    CHECK(draw(1, 0, A, B, 10) == RSX_VR_SAME_CLEAN);
    CHECK(draw(1, 3, A, B, 10) == RSX_VR_RANGE);             /* batch first differs */
    CHECK(draw(1, 3, A2, B, 10) == RSX_VR_RANGE);            /* span length differs */
    CHECK(draw(2, 3, A2, B, 10) == RSX_VR_NEW_ATTR);         /* attribute word differs */
    CHECK(draw(2, 3, A2, B, 10) == RSX_VR_SAME_CLEAN);
    const rsx_vr_span Bad = { 7, 0, 16 };                    /* no guest pointer */
    CHECK(draw(2, 3, A2, Bad, 10) == RSX_VR_RANGE);          /* spans differ first */
    CHECK(draw(2, 3, A2, Bad, 10) == RSX_VR_UNKNOWN);        /* same spans, but no pointer / no copy */
    CHECK(S.fetched == 14 && S.verts == 140);
    uint64_t sum = 0; for (int c = 0; c < RSX_VR_N; c++) { sum += S.cls[c]; CHECK(S.cls_verts[c] == S.cls[c] * 10); }
    CHECK(sum == S.fetched);
    /* a draw with no spans or a source over the copy limit is UNKNOWN once the keys repeat */
    rsx_vr_prev q; memset(&q, 0, sizeof q); rsx_vr_stat t; memset(&t, 0, sizeof t);
    const uint32_t key[2] = { 5, 6 };
    const rsx_vr_span big = { 0, 0, RSX_VR_COPY_MAX + 1u };
    CHECK(rsx_vr_fetched(&q, &t, key, 2, 2, &big, 1, 1, 0, ptr_cb, wr_cb, NULL) == RSX_VR_NEW_ATTR);
    CHECK(rsx_vr_fetched(&q, &t, key, 2, 2, &big, 1, 1, 0, ptr_cb, wr_cb, NULL) == RSX_VR_UNKNOWN);
    CHECK(rsx_vr_fetched(&q, &t, key, 2, 2, NULL, 0, 1, 0, ptr_cb, wr_cb, NULL) == RSX_VR_RANGE);
    CHECK(rsx_vr_fetched(&q, &t, key, 2, 2, NULL, 0, 1, 0, ptr_cb, wr_cb, NULL) == RSX_VR_UNKNOWN);
    free(q.copy);
}

/* random run against an oracle that keeps its own full copy and applies the same rules */
static void random_run(void)
{
    rsx_vr_prev p; memset(&p, 0, sizeof p); rsx_vr_stat st; memset(&st, 0, sizeof st);
    uint8_t oracle[256]; int have = 0; uint32_t okey = 0xFFFFFFFFu; int bad = 0;
    srand(7);
    for (int i = 0; i < 20000; i++) {
        const uint32_t fmt = (uint32_t)(rand() % 3);
        if (rand() % 3 == 0) { mem[0][0x40 + rand() % 256] = (uint8_t)rand(); }
        if (rand() % 4 == 0) tracked[0][0] = rand() & 1;
        const rsx_vr_span a = { 0, 0x40, 256 };
        const uint32_t key[2] = { fmt, 9 };
        const int cls = rsx_vr_fetched(&p, &st, key, 2, 2, &a, 1, 1, (uint32_t)i, ptr_cb, wr_cb, NULL);
        int want;
        if (!have || okey != fmt) want = RSX_VR_NEW_ATTR;
        else { const int same = !memcmp(oracle, mem[0] + 0x40, 256); want = same ? (tracked[0][0] ? RSX_VR_SAME_PAGE_FP : RSX_VR_SAME_CLEAN)
                                                                        : (tracked[0][0] ? RSX_VR_CHG_TRACKED : RSX_VR_CHG_UNTRACKED); }
        if (cls != want) bad++;
        memcpy(oracle, mem[0] + 0x40, 256); okey = fmt; have = 1;
    }
    printf("[random] %d class mismatches; same-clean %llu chg-untracked %llu\n", bad, (unsigned long long)st.cls[RSX_VR_SAME_CLEAN],
           (unsigned long long)st.cls[RSX_VR_CHG_UNTRACKED]);
    CHECK(bad == 0 && st.cls[RSX_VR_SAME_CLEAN] > 1000 && st.cls[RSX_VR_CHG_UNTRACKED] > 1000);
    free(p.copy);
}

int main(void)
{
    scripted(); random_run(); free(P.copy);
    printf(fails ? "[test_vtx_reg] FAIL %d\n" : "[test_vtx_reg] PASS\n", fails);
    return fails ? 1 : 0;
}
