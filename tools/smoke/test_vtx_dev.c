/* Host test for KZ3 patch 2092 (rsx_vtx_dev.h): the device arena (first fit, coalescing, frees deferred to a fence)
 * and the hit/invalidate rule of the device-resident vertex cache (tracked page notes, then a full source compare);
 * patch 2093: the fill's index scratch and staging layout (payload | pad | indices, ring indices equal).
 * No game data, no D3D12. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_vtx_dev.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("[PASS] %s\n", msg); else { printf("[FAIL] %s\n", msg); fails++; } } while (0)

/* ---- arena ---- */
static int arena_consistent(const rsx_vd_arena* a)
{
    uint64_t free_b = 0;
    for (uint32_t i = 0; i < a->n_fr; i++) {
        if (a->fr[i].off % RSX_VD_ALIGN || a->fr[i].len % RSX_VD_ALIGN || !a->fr[i].len) return 0;
        if (i && a->fr[i - 1].off + a->fr[i - 1].len >= a->fr[i].off) return 0;   /* sorted, never adjacent */
        if ((uint64_t)a->fr[i].off + a->fr[i].len > a->size) return 0;
        free_b += a->fr[i].len;
    }
    return free_b + a->used == a->size;
}

#define UNITS 4096u   /* 1 MB arena in 256-byte units */
static uint8_t own[UNITS];   /* 0 free, 1 live, 2 pending */
typedef struct { uint32_t off, len; } blk;

static void test_arena(void)
{
    rsx_vd_arena a;
    CHECK(rsx_vd_init(&a, UNITS * RSX_VD_ALIGN) && a.n_fr == 1 && a.used == 0, "init: one free extent");
    const uint32_t x = rsx_vd_alloc(&a, 100), y = rsx_vd_alloc(&a, 300), z = rsx_vd_alloc(&a, 256);
    CHECK(x == 0 && y == 256 && z == 768 && a.used == 1024, "first fit, sizes rounded to 256");
    CHECK(rsx_vd_free_now(&a, y, 300) && a.n_fr == 2 && arena_consistent(&a), "free in the middle: a hole");
    CHECK(rsx_vd_alloc(&a, 512) == 256 && a.n_fr == 1, "the hole is reused first");
    CHECK(rsx_vd_free_now(&a, x, 100) && rsx_vd_free_now(&a, 256, 512) && rsx_vd_free_now(&a, z, 256) &&
          a.n_fr == 1 && a.used == 0 && a.fr[0].len == a.size, "frees coalesce back to one extent");
    CHECK(rsx_vd_alloc(&a, a.size + 1u) == RSX_VD_NONE && a.used == 0, "too large: none");

    /* a block freed later is not reusable before its fence */
    const uint32_t b = rsx_vd_alloc(&a, a.size);
    CHECK(b == 0 && rsx_vd_alloc(&a, 256) == RSX_VD_NONE, "arena full");
    rsx_vd_free_later(&a, b, a.size, 7);
    CHECK(rsx_vd_alloc(&a, 256) == RSX_VD_NONE, "pending block not allocatable");
    CHECK(rsx_vd_reclaim(&a, 6) == 0 && rsx_vd_alloc(&a, 256) == RSX_VD_NONE, "fence 6 < 7: still pending");
    CHECK(rsx_vd_reclaim(&a, 7) == a.size && a.n_pd == 0 && a.used == 0, "fence 7: reclaimed");

    /* random stress against an ownership map: no overlap, accounting exact, pending blocks untouched */
    blk live[2048]; uint32_t n_live = 0;
    blk pend[8192]; uint64_t pend_f[8192]; uint32_t n_pend = 0;
    uint64_t fence = 1, done = 0; int bad = 0; unsigned seed = 12345u;
    memset(own, 0, sizeof own);
    for (int it = 0; it < 200000 && !bad; it++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t r = (seed >> 8) % 100u;
        if (r < 45u && n_live < 2048u) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t len = 1u + (seed >> 9) % (RSX_VD_ALIGN * 24u);
            const uint32_t o = rsx_vd_alloc(&a, len);
            if (o == RSX_VD_NONE) continue;
            for (uint32_t u = o / RSX_VD_ALIGN; u < (o + rsx_vd_round(len)) / RSX_VD_ALIGN; u++) {
                if (u >= UNITS || own[u]) bad = 1;
                else own[u] = 1;
            }
            live[n_live].off = o; live[n_live].len = len; n_live++;
        } else if (r < 85u && n_live) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t k = (seed >> 7) % n_live;
            const blk bl = live[k]; live[k] = live[--n_live];
            for (uint32_t u = bl.off / RSX_VD_ALIGN; u < (bl.off + rsx_vd_round(bl.len)) / RSX_VD_ALIGN; u++) own[u] = 2;
            rsx_vd_free_later(&a, bl.off, bl.len, fence + 1u);   /* the open list's value */
            if (n_pend < 8192u) { pend[n_pend] = bl; pend_f[n_pend] = fence + 1u; n_pend++; }
        } else if (r < 95u) {
            fence++;   /* a submit signals */
        } else {
            if (done < fence) done += 1u + ((seed >> 5) % (fence - done));
            rsx_vd_reclaim(&a, done);
            uint32_t m = 0;
            for (uint32_t i = 0; i < n_pend; i++) {
                if (pend_f[i] <= done) {
                    for (uint32_t u = pend[i].off / RSX_VD_ALIGN; u < (pend[i].off + rsx_vd_round(pend[i].len)) / RSX_VD_ALIGN; u++)
                        own[u] = 0;
                } else { pend[m] = pend[i]; pend_f[m] = pend_f[i]; m++; }
            }
            n_pend = m;
        }
        if ((it & 1023) == 0 && !arena_consistent(&a)) bad = 2;
    }
    CHECK(!bad && n_pend < 8192u, "200k random alloc / deferred free / reclaim: no overlap with live or pending blocks");
    for (uint32_t i = 0; i < n_live; i++) rsx_vd_free_now(&a, live[i].off, live[i].len);
    rsx_vd_reclaim(&a, ~0ull);
    CHECK(a.used == 0 && a.n_fr == 1 && a.n_pd == 0 && arena_consistent(&a), "all freed: one extent, nothing used");
    rsx_vd_destroy(&a);
}

/* ---- hit / invalidate rule ---- */
static uint8_t guest[4u << 20];   /* 4 MB of "main memory" */
static uint32_t td_live[2048], stamps[65536];
static uint32_t frame;

static void mark(uint32_t off, uint32_t len)   /* a tracked writer (SPU put, file read, NV0039 ...) */
{
    for (uint32_t p = off >> 16; p <= (off + len - 1u) >> 16; p++) td_live[p >> 5] |= 1u << (p & 31u);
}
static void flip(void)   /* the walker's flip: live bits become stamps (1 + frame) */
{
    for (uint32_t w = 0; w < 2048u; w++)
        for (uint32_t b = 0; td_live[w] && b < 32u; b++)
            if (td_live[w] & (1u << b)) stamps[w * 32u + b] = frame + 1u;
    memset(td_live, 0, sizeof td_live);
    frame++;
}
static int written(void* ctx, const rsx_vd_span* sp, uint32_t since)
{
    (void)ctx; return rsx_vd_pages_written(td_live, stamps, sp->off, sp->len, since);
}
static const uint8_t* ptr(void* ctx, const rsx_vd_span* sp)
{
    (void)ctx; return (uint64_t)sp->off + sp->len <= sizeof guest ? guest + sp->off : NULL;
}

typedef struct { rsx_vd_span spans[3], cmp[2]; uint32_t n_spans, n_cmp, fill_frame; uint8_t* copy; } entry;
static void fill(entry* e)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < e->n_cmp; i++) n += e->cmp[i].len;
    e->copy = (uint8_t*)realloc(e->copy, n);
    uint8_t* c = e->copy;
    for (uint32_t i = 0; i < e->n_cmp; i++) { memcpy(c, guest + e->cmp[i].off, e->cmp[i].len); c += e->cmp[i].len; }
    e->fill_frame = frame;
}
static int lookup(entry* e, uint64_t* cmp)
{
    return rsx_vd_validate(e->spans, e->n_spans, e->cmp, e->n_cmp, e->copy, e->fill_frame, written, ptr, NULL, cmp);
}

static void test_rule(void)
{
    CHECK(rsx_vd_pages_written(td_live, stamps, 0x10000, 0, 0), "an unknown (empty) span counts as written");
    mark(0x2FFFF, 1);
    CHECK(rsx_vd_pages_written(td_live, stamps, 0x20000, 0x10000, 0) &&
          !rsx_vd_pages_written(td_live, stamps, 0x30000, 0x10000, 0), "live bit: only its 64 KB page");
    CHECK(rsx_vd_pages_written(td_live, stamps, 0x1FFF0, 0x20, 0), "a span crossing into a written page");
    flip();   /* frame 0 -> 1, page 2 stamp 1 */
    CHECK(rsx_vd_pages_written(td_live, stamps, 0x20000, 16, 0) && !rsx_vd_pages_written(td_live, stamps, 0x20000, 16, 1),
          "stamp: written since frame 0, not since frame 1");

    for (uint32_t i = 0; i < sizeof guest; i++) guest[i] = (uint8_t)(i * 2654435761u >> 13);
    /* an indexed draw: index range + two attributes of one interleaved buffer (merged into one compare span) */
    entry e = {0};
    e.spans[0] = (rsx_vd_span){1, 0x100000, 0x600};          /* indices */
    e.spans[1] = (rsx_vd_span){1, 0x180000, 0x3000};         /* position */
    e.spans[2] = (rsx_vd_span){1, 0x18000C, 0x3000};         /* uv, same buffer */
    e.n_spans = 3;
    e.cmp[0] = e.spans[0]; e.cmp[1] = (rsx_vd_span){1, 0x180000, 0x300C}; e.n_cmp = 2;
    fill(&e);
    uint64_t cmp = 0;
    CHECK(lookup(&e, &cmp) == RSX_VD_HIT && cmp == 0x600u + 0x300Cu, "unchanged: hit, every source byte compared");
    flip(); flip();
    CHECK(lookup(&e, NULL) == RSX_VD_HIT, "two frames later, still unchanged: hit");
    guest[0x180000 + 0x2FFF] ^= 0x01;   /* an untracked store (recompiled PPU code) into the vertex buffer */
    CHECK(lookup(&e, NULL) == RSX_VD_CONTENT, "untracked one-byte store: content invalidation");
    fill(&e);
    CHECK(lookup(&e, NULL) == RSX_VD_HIT, "refilled: hit again");
    guest[0x100000 + 0x5FF] ^= 0x80;
    CHECK(lookup(&e, NULL) == RSX_VD_CONTENT, "untracked store into the index range: content invalidation");
    fill(&e);
    mark(0x180000 + 0x100, 4);   /* a tracked write that leaves the bytes equal (same value rewritten) */
    CHECK(lookup(&e, NULL) == RSX_VD_TRACKED, "tracked write this frame: invalidated before any compare");
    flip();
    CHECK(lookup(&e, NULL) == RSX_VD_TRACKED, "after the flip the page stamp still invalidates the old fill");
    fill(&e);
    CHECK(lookup(&e, NULL) == RSX_VD_HIT, "filled after the write: hit");
    mark(0x400000 - 16, 16);   /* another page */
    CHECK(lookup(&e, NULL) == RSX_VD_HIT, "a tracked write elsewhere does not invalidate");
    guest[0x200000] ^= 1;   /* outside every span */
    CHECK(lookup(&e, NULL) == RSX_VD_HIT, "an untracked store outside the spans does not invalidate");
    e.cmp[1].len = (uint32_t)(sizeof guest - e.cmp[1].off + 1u);
    CHECK(lookup(&e, NULL) == RSX_VD_CONTENT, "an unmapped source span never hits");
    free(e.copy);
}

/* ---- patch 2093: fill bytes from CPU memory ---- */
static void topo(uint32_t* ix, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) ix[i] = seed * 7919u + i * 3u; }
static void test_stage(void)
{
    uint32_t* scr = NULL; uint32_t cap = 0;
    static uint8_t stage[1u << 16], ring[1u << 16];
    uint8_t vb[1000];
    int ok_grow = 1, ok_layout = 1, ok_pad = 1, ok_ring = 1;
    const uint32_t sizes[] = { 12u, 4000u, 400u, 16000u, 4u, 16000u, 8000u };
    for (uint32_t t = 0; t < sizeof sizes / sizeof sizes[0]; t++) {
        const uint32_t ib = sizes[t], vbn = 100u + 113u * t, vb_r = rsx_vd_round(vbn);
        for (uint32_t i = 0; i < vbn; i++) vb[i] = (uint8_t)(i * 31u + t);
        if (!rsx_vd_scratch(&scr, &cap, ib) || cap < ib) { ok_grow = 0; break; }
        topo(scr, ib / 4u, t);
        memset(stage, 0xA5, sizeof stage); memset(ring, 0x5A, sizeof ring);
        memcpy(ring, scr, ib);   /* ld_vd_fill: the ring first, then the staging block */
        rsx_vd_stage_write(stage, NULL, vb, vbn, vb_r, scr, ib);
        uint32_t want[4000]; topo(want, ib / 4u, t);
        if (memcmp(stage, vb, vbn) || memcmp(stage + vb_r, want, ib)) ok_layout = 0;
        for (uint32_t i = vbn; i < vb_r; i++) if (stage[i] != 0xA5) ok_pad = 0;
        if (stage[vb_r + ib] != 0xA5) ok_pad = 0;
        if (memcmp(ring, want, ib) || memcmp(ring, stage + vb_r, ib)) ok_ring = 0;
        rsx_vd_stage_write(stage, ring + 8192u, vb, vbn, vb_r, scr, ib);   /* the optional ring copy */
        if (memcmp(ring + 8192u, want, ib)) ok_ring = 0;
    }
    CHECK(ok_grow, "index scratch grows to every size and keeps the larger capacity");
    CHECK(ok_layout, "staging block = payload, then the indices at the rounded payload size");
    CHECK(ok_pad, "the staging write touches nothing outside payload and indices");
    CHECK(ok_ring, "ring and staging hold the same indices");
    free(scr);
}

int main(void)
{
    test_arena();
    test_rule();
    test_stage();
    printf("[test_vtx_dev] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
