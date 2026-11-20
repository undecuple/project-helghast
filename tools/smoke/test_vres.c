/* Host test for KZ3 patch 2198 (S7.4): libs/kzr/core/vres (resident raw vertex streams, pure core) and its bridge
 * libs/video/rsx_vtx_res.h. Design input and the stress model are Killzone 2's tools/runtime/test_vtx_res.c (patch
 * 6079, rounds 88-91b); the write generation here is a test-side model of the OS write watch (stamps from a clock,
 * Wine's semantics: a page is reported from its first write until a reset) behind kzr_wgen_ops.
 * Properties (each prints "FAIL <prop>: ..." when broken; tools/smoke/test_vres.sh runs every control and requires
 * the named property to fail):
 *   decode        decode through the block lists equals 2076's decode bit for bit over 3,000 random draws
 *   stale         at every draw the pool blocks hold the guest bytes (300,000-step stress, GPU three frames behind,
 *                 page copies as delayed jobs)                                   (TEST_NEG=1 blind watch; 3 writes in an epoch)
 *   overwrite     a block a frame read is not overwritten before the frame completes   (TEST_NEG=2 ignored fence)
 *   pending_copy  a block with a pending page copy job is not taken        (mutant COPYSEQ)
 *   evict_current a current page is not evicted while a safe retired block exists  (mutant FIRST_FIT)
 *   verify_bad    verify finds no bad reuse with legal writes; finds them with writes inside an epoch (TEST_NEG=3 TEST_VERIFY=1)
 *   stamp_record  vr_stamp at record protects a block over 3 ring flushes       (TEST_NEG=4)
 *   hold          a producer's write after an unmet acquire is seen            (TEST_NEG=5)
 *   acquire_epoch Killzone 2 stale-reuse model: 2,000 draws a frame, an unmet acquire mid frame, the producer writes after it: no
 *                 stale reuse; ACQUIRE ends the epoch                          (mutant ACQUIRE_NOOP)
 *   arm_order     a write landing during the copy, collected by ANOTHER user of the pages before our arm would be taken
 *                 (shared-page reset) is not lost                              (mutant ARM_AFTER)
 *   shared_reset  the stamps, not a per-entry reset, keep a write another entry's collect reset (TEST_NEG=6 naive clean)
 *   write_after   a write after the copy gives a new block, the old block stays intact for the frames that read it
 *   min_age       a block the GPU completed (completed = frame - 1) is still not overwritten in the next frame; it is two frames later
 *                                                                              (a copy of the header with min_age 0)
 *   zero_blocks   a pool of no blocks: init, alloc, spare_push and map_span neither divide by zero nor hand out a block (a copy without the guards)
 * No game data, no GPU. */
#define RSX_VG_C_REFERENCE
static const unsigned* g_blk; /* 2076's per-draw block */
static const float* g_lut;
#define VG_RAW(a) g_blk[(a) >> 2]
#define VG_LUT(i) g_lut[i]
#include "rsx_vtx_res.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(a, n) _aligned_malloc((n), (a)) /* MinGW has no aligned_alloc; the memory is never freed */
#endif

/* the paged decode as C (same conventions as 2076's reference) */
static const uint32_t* g_tab; /* draw table, as dwords */
static const uint8_t* g_pool;
static inline unsigned vr_raw_c(unsigned pt, unsigned a)
{
    unsigned w;
    memcpy(&w, g_pool + ((size_t)g_tab[(pt + (a >> 12) * 4u) >> 2] << 12) + (a & 4095u), 4);
    return w;
}
#define KZR_VRES_RAW(pt, a) vr_raw_c(pt, a)
#define uint rsx_vg_uint
KZR_VRES_SHARED(RSX_VG_CODE)
#undef uint

#define NP 96u
#define NB 56u
#define BATCH 8u
static uint8_t* G; /* guest pages (watched) */
static int g_blind, g_query_fail, g_naive;
static uint8_t g_poolmem[256][KZR_VRES_PAGE];

/* ---- the model write generation (KZ2 6077's bookkeeping over a model OS watch) ---- */
static unsigned char g_written[NP];
static uint64_t g_stamp[NP], g_clock = 1;
static int g_last_collect_pages; /* naive control: pages the last collect reported */
static int model_query(uintptr_t base, size_t size, uintptr_t* addrs, uintptr_t* count)
{
    if (g_query_fail) return 1;
    uintptr_t a = base, pos = 0;
    const uintptr_t end = a + size;
    for (; pos < *count && a < end; a += KZR_VRES_PAGE) {
        const uintptr_t p = (a - (uintptr_t)G) >> KZR_VRES_PAGE_SHIFT;
        if (p < NP && g_written[p] && !g_blind) addrs[pos++] = a;
    }
    for (uintptr_t r = base; r < a; r += KZR_VRES_PAGE) { /* reset only what was scanned */
        const uintptr_t p = (r - (uintptr_t)G) >> KZR_VRES_PAGE_SHIFT;
        if (p < NP) g_written[p] = 0;
    }
    *count = pos;
    return 0;
}
static int ww_collect(void* user, uintptr_t lo, uint32_t size)
{
    (void)user;
    uintptr_t a = lo & ~(uintptr_t)(KZR_VRES_PAGE - 1u);
    const uintptr_t end = (lo + size + KZR_VRES_PAGE - 1u) & ~(uintptr_t)(KZR_VRES_PAGE - 1u);
    int bumped = 0;
    g_last_collect_pages = 0;
    while (a < end) {
        const uintptr_t n =
            end - a < ((uintptr_t)BATCH << KZR_VRES_PAGE_SHIFT) ? end - a : ((uintptr_t)BATCH << KZR_VRES_PAGE_SHIFT);
        uintptr_t addrs[BATCH], cnt = BATCH;
        if (model_query(a, (size_t)n, addrs, &cnt) != 0 || cnt > BATCH) return 0;
        if (cnt && !bumped) {
            g_clock++;
            bumped = 1;
        }
        for (uintptr_t i = 0; i < cnt; i++) g_stamp[(addrs[i] - (uintptr_t)G) >> KZR_VRES_PAGE_SHIFT] = g_clock;
        g_last_collect_pages += (int)cnt;
        a += n;
    }
    return 1;
}
static void ww_arm(void* user, kzr_wgen_arm* e, uintptr_t lo)
{
    (void)user;
    e->seq = g_clock;
    e->lo = lo;
}
static int ww_clean(void* user, const kzr_wgen_arm* e, uintptr_t lo, uint32_t size)
{
    (void)user;
    if (!e->seq || e->lo != lo) return 0;
    if (g_naive) return g_last_collect_pages == 0; /* TEST_NEG=6: "my own collect found nothing" */
    const uintptr_t p0 = (lo - (uintptr_t)G) >> KZR_VRES_PAGE_SHIFT,
                    p1 = (lo - (uintptr_t)G + size - 1u) >> KZR_VRES_PAGE_SHIFT;
    for (uintptr_t p = p0; p <= p1; p++)
        if (g_stamp[p] > e->seq) return 0;
    return 1;
}
static const kzr_wgen_ops WG = {NULL, ww_collect, ww_arm, ww_clean};
static void ww_reset(void)
{
    memset(g_written, 0, sizeof g_written);
    memset(g_stamp, 0, sizeof g_stamp);
    g_clock = 1;
    g_naive = 0;
    g_blind = 0;
    g_query_fail = 0;
}

/* ---- page copies as delayed jobs ---- */
typedef struct {
    uint64_t seq;
    uint32_t block;
    uint8_t data[KZR_VRES_PAGE];
} job_t;
#define MAXJ 4096u
static job_t g_jobs[MAXJ];
static unsigned g_jh, g_jn;
static uint64_t g_jseq;
static int g_async, g_race_page = -1, g_race_other;
static uint8_t g_pending[256];
static unsigned long long g_pending_copy;
static unsigned long long g_stale_jobs;

static uint64_t rng = 0x243F6A8885A308D3ull;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}
static int fails;
static jmp_buf g_fpe_jmp;
static void on_fpe(int sig)
{
    (void)sig;
    longjmp(g_fpe_jmp, 1);
}
#define CHECKP(prop, c, ...)                                                                                           \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s: ", prop);                                                                                 \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void guest_write(unsigned p)
{
    const unsigned n = 1 + rnd(64);
    for (unsigned i = 0; i < n; i++) G[(size_t)p * KZR_VRES_PAGE + rnd(KZR_VRES_PAGE)] = (uint8_t)rnd(256);
    g_written[p] = 1;
}

static uint64_t copy_block(void* user, uint32_t block, const uint8_t* src)
{
    (void)user;
    if (g_race_page >=
        0) { /* arm_order / shared_reset: after the read a writer lands, another user's collect resets it */
        memcpy(g_poolmem[block], src, KZR_VRES_PAGE);
        guest_write((unsigned)g_race_page);
        if (g_race_other) ww_collect(NULL, (uintptr_t)G + (size_t)g_race_page * KZR_VRES_PAGE, KZR_VRES_PAGE);
        g_race_page = -1;
        return 0;
    }
    if (!g_async) {
        memcpy(g_poolmem[block], src, KZR_VRES_PAGE);
        return 0;
    }
    if (g_pending[block]) g_pending_copy++; /* a block taken with its copy job pending */
    if (g_jn == MAXJ) {
        fprintf(stderr, "job queue full\n");
        exit(2);
    }
    job_t* j = &g_jobs[(g_jh + g_jn) % MAXJ];
    j->seq = ++g_jseq;
    j->block = block;
    memcpy(j->data, src, KZR_VRES_PAGE);
    g_pending[block]++;
    g_jn++;
    return j->seq;
}
static void jobs_apply(kzr_vres* s, uint64_t upto, unsigned max)
{
    while (g_jn && g_jobs[g_jh].seq <= upto && max--) {
        job_t* j = &g_jobs[g_jh];
        memcpy(g_poolmem[j->block], j->data, KZR_VRES_PAGE);
        g_pending[j->block]--;
        kzr_vres_copies_done(s, j->seq);
        g_jh = (g_jh + 1u) % MAXJ;
        g_jn--;
    }
}
static const uint8_t* peek_block(void* user, uint32_t block)
{
    (void)user;
    return g_poolmem[block];
}
static uint32_t g_label[KZR_VRES_HOLD_MAX + 2];
static uint32_t read_label(void* user, uint32_t ea)
{
    (void)user;
    return g_label[ea];
}

static void vinit(kzr_vres* s, uint32_t nb, int verify)
{
    static uint32_t map[NP];
    static kzr_vres_block blocks[256];
    static uint32_t spare[256];
    ww_reset();
    g_async = 0;
    g_race_page = -1;
    g_jh = g_jn = 0;
    g_jseq = 0;
    memset(g_pending, 0, sizeof g_pending);
    kzr_vres_init(s, &WG, (uintptr_t)G, NP, map, blocks, spare, nb, copy_block, NULL, verify);
}

/* ---- decode equality ---- */
static const unsigned ESZ[8] = {0, 2, 4, 2, 1, 2, 4, 1}; /* bytes per component, CMP32 whole */
static void part_decode(kzr_vres* s)
{
    static float lut[RSX_VG_LUT_WORDS];
    for (unsigned i = 0; i < RSX_VG_LUT_WORDS; i++) {
        unsigned w = (unsigned)rnd(1u << 31) * 2u + rnd(2);
        memcpy(&lut[i], &w, 4);
    }
    g_lut = lut;
    static uint8_t blk[64 * KZR_VRES_PAGE], tab[KZR_VRES_TABLE_BYTES + 4 * 4096];
    unsigned long long comps = 0, diff = 0;
    uint32_t frame = 1;
    for (int trial = 0; trial < 3000; trial++) {
        for (unsigned p = 0; p < NP; p++)
            if (rnd(4) == 0) guest_write(p);
        kzr_vres_epoch(s);
        rsx_vg_draw d;
        memset(&d, 0, sizeof d);
        for (unsigned a = 0; a < 16; a++) {
            for (int c = 0; c < 4; c++) d.table[a].dflt[c] = (float)rnd(1000) / 7.0f;
        }
        struct {
            unsigned slot, type, n, stride, first, last, esz;
            const uint8_t* at;
        } at[6];
        const unsigned na = 1 + rnd(6);
        const uint8_t* reg[3];
        const unsigned nreg = 1 + rnd(3); /* 1-3 regions; attributes interleave or sit apart */
        for (unsigned r = 0; r < nreg; r++) reg[r] = G + rnd((NP - 24) * KZR_VRES_PAGE);
        unsigned used = 0;
        for (unsigned i = 0; i < na; i++) {
            unsigned slot;
            do slot = rnd(16);
            while (used & (1u << slot));
            used |= 1u << slot;
            at[i].slot = slot;
            at[i].type = 1 + rnd(7);
            at[i].n = 1 + rnd(4);
            at[i].esz = at[i].type == 6 ? 4 : ESZ[at[i].type] * at[i].n;
            at[i].stride = at[i].esz + rnd(3) * 4 + rnd(2) * rnd(9);
            at[i].first = rnd(200);
            at[i].last = at[i].first + rnd(300);
            at[i].at = reg[rnd(nreg)] + rnd(64);
        }
        struct {
            const uint8_t* src;
            uint64_t bytes;
        } cand[6]; /* 2076's plan: spans merged by host address */
        for (unsigned i = 0; i < na; i++) {
            cand[i].src = at[i].at;
            cand[i].bytes = (uint64_t)(at[i].last - at[i].first) * at[i].stride + at[i].esz;
        }
        for (unsigned i = 1; i < na; i++)
            for (unsigned j = i; j > 0 && cand[j].src < cand[j - 1].src; j--) {
                __typeof__(cand[0]) t = cand[j];
                cand[j] = cand[j - 1];
                cand[j - 1] = t;
            }
        uint64_t cursor = RSX_VG_TABLE_BYTES;
        for (unsigned i = 0; i < na; i++) {
            rsx_vg_span* last = d.n_span ? &d.span[d.n_span - 1] : NULL;
            if (last && cand[i].src <= last->src + last->bytes) {
                const uint64_t end = (uint64_t)(cand[i].src - last->src) + cand[i].bytes;
                if (end > last->bytes) {
                    cursor += end - last->bytes;
                    last->bytes = (u32)end;
                }
                continue;
            }
            cursor = ((cursor + 15u) & ~(uint64_t)15u) + ((uintptr_t)cand[i].src & 3u);
            rsx_vg_span* sp = &d.span[d.n_span++];
            sp->src = cand[i].src;
            sp->bytes = (u32)cand[i].bytes;
            sp->dst = (u32)cursor;
            cursor += cand[i].bytes;
        }
        d.block_bytes = (u32)((cursor + 15u) & ~(uint64_t)15u);
        for (unsigned i = 0; i < na; i++) {
            const rsx_vg_span* sp = d.span;
            while (!(at[i].at >= sp->src && at[i].at < sp->src + sp->bytes)) sp++;
            const u32 first_at = sp->dst + (u32)(at[i].at - sp->src);
            rsx_vg_attr* t = &d.table[at[i].slot];
            t->stride = at[i].stride;
            t->base = first_at - at[i].first * at[i].stride;
            t->lim = first_at + (at[i].last - at[i].first) * at[i].stride;
            t->fmt = at[i].type | (at[i].n << 4);
        }
        if (d.block_bytes > sizeof blk) continue;
        rsx_vg_write_block(&d, blk);
        uint32_t lists[RSX_DSP_NUM_VERTEX_ATTR][2048];
        const uint32_t* lp[RSX_DSP_NUM_VERTEX_ATTR];
        uint32_t np[RSX_DSP_NUM_VERTEX_ATTR];
        int ok = 1;
        for (unsigned k = 0; k < d.n_span && ok; k++) {
            const int n =
                kzr_vres_map_span(s, (uintptr_t)d.span[k].src, d.span[k].bytes, frame, frame - 1, lists[k], 2048, NULL);
            if (n < 0)
                ok = 0;
            else {
                np[k] = (uint32_t)n;
                lp[k] = lists[k];
            }
        }
        CHECKP("decode", ok, "a span did not map (trial %d)", trial);
        if (!ok) continue;
        rsx_vr_write_table(&d, lp, np, tab);
        g_blk = (const unsigned*)blk;
        g_tab = (const uint32_t*)tab;
        g_pool = &g_poolmem[0][0];
        const kzr_vres_row* rows = (const kzr_vres_row*)tab;
        for (unsigned a = 0; a < 16; a++) {
            const rsx_vg_attr* t = &d.table[a];
            const kzr_vres_row* r = &rows[a];
            unsigned e0 = 0, e1 = 0;
            for (unsigned i = 0; i < na; i++)
                if (at[i].slot == a) {
                    e0 = at[i].first;
                    e1 = at[i].last + 3;
                }
            for (unsigned e = e0; e <= e1; e++)
                for (unsigned c = 0; c < 4; c++) {
                    const float x = vg_fetch(t->base, t->stride, t->fmt, t->lim, t->dflt[c], e, c);
                    const float y = kzr_vres_fetch(r->pt, r->base, r->stride, r->fmt, r->lim, r->dflt[c], e, c);
                    comps++;
                    if (memcmp(&x, &y, 4)) diff++;
                }
        }
        frame++;
    }
    CHECKP("decode", comps > 0, "no component was compared");
    CHECKP("decode", diff == 0, "%llu of %llu components differ from 2076's decode", diff, comps);
    printf("decode: %llu components over 3000 draws, %llu differ\n", comps, diff);
}

/* zero_blocks: SIGFPE (a modulo by zero) is caught and reported as the failure (own function: setjmp and main's locals) */
static void part_zero_blocks(void)
{
    kzr_vres z;
    static kzr_vres_block b0[1];
    static uint32_t sp0[1], map0[NP];
    void (*old)(int) = signal(SIGFPE, on_fpe);
    volatile int ok = 0, crashed = 0;
    if (setjmp(g_fpe_jmp) == 0) {
        ww_reset();
        kzr_vres_init(&z, &WG, (uintptr_t)G, NP, map0, b0, sp0, 0, copy_block, NULL, 0);
        kzr_vres_spare_push(&z, 0);
        uint32_t o[2];
        const int n = kzr_vres_map_span(&z, (uintptr_t)G, 10, 10, 9, o, 2, NULL);
        ok = n == -1 && z.alloc_fail == 1 && z.spare_n == 0 && kzr_vres_alloc(&z, 10, 9) == KZR_VRES_NONE;
    } else
        crashed = 1;
    signal(SIGFPE, old);
    CHECKP("zero_blocks", !crashed, "a pool of no blocks divided by zero");
    CHECKP("zero_blocks", ok || crashed, "a pool of no blocks must refuse every map and keep the spare FIFO empty");
}

typedef struct {
    uint32_t block;
    uint64_t sum;
} rec_t;
static rec_t g_rec[8][4096];
static unsigned g_nrec[8];

int main(void)
{
    const char* ne = getenv("TEST_NEG");
    const int neg = ne ? atoi(ne) : 0;
    const char* ve = getenv("TEST_VERIFY");
    const int verify = ve ? atoi(ve) : 0;
    G = aligned_alloc(KZR_VRES_PAGE, NP * KZR_VRES_PAGE);
    for (unsigned i = 0; i < NP * KZR_VRES_PAGE; i++) G[i] = (uint8_t)rnd(256);
    kzr_vres s;
    uint32_t out[16];

    if (!neg) { /* decode and directed cases */
        vinit(&s, 64, 1);
        part_decode(&s);
        CHECKP("verify_bad", s.bad == 0, "verify found %llu bad reuses with legal writes", (unsigned long long)s.bad);
        vinit(&s, 4, 0);
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + 100, 5 * KZR_VRES_PAGE, 10, 9, out, 4, NULL) == -1 &&
                   s.too_big == 1,
               "span over max_pages refused");
        CHECKP("directed", kzr_vres_map_span(&s, (uintptr_t)G + 100, 3 * KZR_VRES_PAGE, 10, 7, out, 16, NULL) == 4,
               "4 pages fit a 4-block pool");
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + 9 * KZR_VRES_PAGE, 10, 11, 7, out, 16, NULL) == -1 &&
                   s.alloc_fail == 1,
               "a full pool whose blocks are in flight refuses");
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + 100, 3 * KZR_VRES_PAGE, 11, 7, out, 16, NULL) == 4 &&
                   s.reuse_stand == 4,
               "same epoch: stand, no query");
        kzr_vres_epoch(&s);
        guest_write(1);
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + 100, 3 * KZR_VRES_PAGE, 12, 7, out, 16, NULL) == -1 &&
                   s.retires == 1,
               "a written page needs a new block; none free while in flight");
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + 100, 3 * KZR_VRES_PAGE, 20, 19, out, 16, NULL) == 4 &&
                   !memcmp(g_poolmem[out[1]], G + KZR_VRES_PAGE, KZR_VRES_PAGE),
               "after completion the written page is copied again");
        kzr_vres_epoch(&s);
        g_query_fail = 1;
        CHECKP("directed", kzr_vres_map_span(&s, (uintptr_t)G + 100, 10, 21, 20, out, 16, NULL) == -1 && s.ww_fail == 1,
               "a failing query refuses");
        g_query_fail = 0;
        CHECKP("directed",
               kzr_vres_map_span(&s, (uintptr_t)G + ((uintptr_t)1 << 32) + 10, 10, 21, 20, out, 16, NULL) == -1,
               "out of the watched range");
    }
    { /* a current page is not evicted while a safe retired block exists (3 blocks) */
        vinit(&s, 3, 0);
        uint32_t o[2];
        kzr_vres_map_span(&s, (uintptr_t)G, 2 * KZR_VRES_PAGE, 10, 9, o, 2, NULL); /* pages 0, 1 -> blocks 0, 1 */
        kzr_vres_epoch(&s);
        guest_write(1);
        kzr_vres_map_span(&s, (uintptr_t)G + KZR_VRES_PAGE, 10, 20, 19, o, 2,
                          NULL); /* page 1: block 1 retired, block 2 */
        kzr_vres_epoch(&s);
        guest_write(1);
        kzr_vres_map_span(&s, (uintptr_t)G + KZR_VRES_PAGE, 10, 30, 29, o, 2,
                          NULL); /* page 1 again: block 1, not evict page 0 */
        CHECKP("evict_current", s.evicts == 0 && s.map[0] == 1 && o[0] == 1,
               "a retired block is taken before current page 0 is evicted (evicts %llu)", (unsigned long long)s.evicts);
    }
    if (!neg || neg == 4) { /* stamp_record: the record's stamp, after 3 flushes between the map and the record */
        vinit(&s, 1, 0);
        uint32_t o[1];
        uint64_t fv = 10; /* GPU idle: completed = 10 */
        CHECKP("stamp_record",
               kzr_vres_map_span(&s, (uintptr_t)G, 10, (uint32_t)(neg == 4 ? fv + 2 : fv + 1), (uint32_t)fv, o, 1,
                                 NULL) == 1,
               "the draw maps page 0");
        fv += 3;                                                    /* vertex, VS-constant, descriptor ring */
        if (neg != 4) kzr_vres_stamp(&s, o, 1, (uint32_t)(fv + 1)); /* the record: its list signals 14 */
        fv++;
        kzr_vres_epoch(&s);
        G[0] ^= 0xFF;
        g_written[0] = 1; /* a new generation of page 0 */
        const int n =
            kzr_vres_map_span(&s, (uintptr_t)G, 10, (uint32_t)(fv + 1), 13, o, 1, NULL); /* the GPU is on 14 */
        CHECKP("stamp_record", n == -1 && s.alloc_fail == 1,
               "the only block is read by the list that signals 14; no reuse at 13 (n %d)", n);
        if (neg != 4)
            CHECKP("stamp_record",
                   kzr_vres_map_span(&s, (uintptr_t)G, 10, (uint32_t)(fv + 2), 14, o, 1, NULL) == 1 &&
                       g_poolmem[0][0] == G[0],
                   "after 14 completes the page is copied again");
    }
    if (!neg || neg == 5) { /* hold: a producer writes after an acquire the walker passed unmet */
        vinit(&s, 8, 0);
        memset(g_label, 0, sizeof g_label);
        uint32_t o[1];
        kzr_vres_map_span(&s, (uintptr_t)G, 10, 2, 1, o, 1, NULL); /* page 0 resident */
        if (neg == 5)
            kzr_vres_epoch(&s);
        else
            kzr_vres_acquire(&s, 0, 1, g_label[0] == 1); /* label 0 wants 1: unmet */
        kzr_vres_hold_check(&s, read_label, NULL);
        kzr_vres_map_span(&s, (uintptr_t)G, 10, 2, 1, o, 1, NULL); /* clean: kept in the new epoch */
        G[5] ^= 0x5A;
        g_written[0] = 1; /* the producer writes, label not yet released */
        kzr_vres_hold_check(&s, read_label, NULL);
        kzr_vres_map_span(&s, (uintptr_t)G, 10, 2, 1, o, 1, NULL);
        const int fresh1 = !memcmp(g_poolmem[o[0]], G, KZR_VRES_PAGE);
        G[6] ^= 0x33;
        g_written[0] = 1;
        g_label[0] = 1; /* writes again and releases */
        const int held = kzr_vres_hold_check(&s, read_label, NULL);
        kzr_vres_map_span(&s, (uintptr_t)G, 10, 2, 1, o, 1, NULL);
        const int fresh2 = !memcmp(g_poolmem[o[0]], G, KZR_VRES_PAGE);
        CHECKP("hold", fresh1 && (neg == 5 || s.hold_maps >= 1),
               "a write after an unmet acquire leaves a stale page (held maps %llu)", (unsigned long long)s.hold_maps);
        if (neg != 5) {
            CHECKP("hold", !held && fresh2 && s.hold_releases == 1 && s.hold_n == 0,
                   "the label met releases the hold, page fresh");
            for (uint32_t i = 0; i <= KZR_VRES_HOLD_MAX; i++)
                kzr_vres_acquire(&s, i + 1, 7, 0); /* one more than the table */
            for (uint32_t i = 0; i <= KZR_VRES_HOLD_MAX; i++) g_label[i + 1] = 7;
            CHECKP("hold", s.hold_overflow == 1 && kzr_vres_hold_check(&s, read_label, NULL) && s.hold_n == 0,
                   "an overflow keeps the hold (sticky) after every tracked label is met");
            kzr_vres_acquire(&s, 1, 7, 1);
            CHECKP("hold", s.holds == 1 + KZR_VRES_HOLD_MAX + 1, "a met acquire is no hold");
        }
    }
    if (!neg) { /* min_age: completed = frame - 1 satisfies last_use <= completed, so only the margin keeps a block read by frame 10 out of frame 11 */
        vinit(&s, 1, 0);
        uint32_t o[1];
        CHECKP("min_age", kzr_vres_map_span(&s, (uintptr_t)G, 10, 10, 9, o, 1, NULL) == 1,
               "the draw of frame 10 maps page 0");
        kzr_vres_epoch(&s);
        guest_write(0);
        CHECKP("min_age", kzr_vres_map_span(&s, (uintptr_t)G, 10, 11, 10, o, 1, NULL) == -1 && s.alloc_fail == 1,
               "the only block was read in frame 10 (completed 10, frame 11): the written page must not overwrite it "
               "yet (alloc_fail %llu)",
               (unsigned long long)s.alloc_fail);
        kzr_vres_epoch(&s);
        CHECKP("min_age",
               kzr_vres_map_span(&s, (uintptr_t)G, 10, 12, 11, o, 1, NULL) == 1 &&
                   !memcmp(g_poolmem[o[0]], G, KZR_VRES_PAGE),
               "two frames after its last use the block is taken again");
    }
    if (!neg) part_zero_blocks();
    if (!neg) { /* acquire_epoch: Killzone 2 stale-reuse shape (1,937 draws a frame, an unmet acquire in the frame, the producer writes after it) */
        vinit(&s, 200, 1);
        s.peek = peek_block;
        memset(g_label, 0, sizeof g_label);
        unsigned long long draws = 0, stale = 0;
        uint32_t frame = 1;
        for (int f = 0; f < 20; f++, frame++) {
            kzr_vres_epoch(&s); /* the flip */
            for (int d = 0; d < 2000; d++) {
                if (d == 1000) kzr_vres_acquire(&s, 0, (uint32_t)f + 1, g_label[0] == (uint32_t)f + 1); /* unmet */
                if (d == 1100)
                    for (unsigned k = 0; k < 10; k++) guest_write(rnd(NP)); /* the producer, after the acquire */
                if (d == 1500) g_label[0] = (uint32_t)f + 1;                /* the label is met */
                kzr_vres_hold_check(&s, read_label, NULL);
                const unsigned p = rnd(NP - 3), np = 1 + rnd(3);
                uint32_t o[4];
                const int n = kzr_vres_map_span(&s, (uintptr_t)G + (size_t)p * KZR_VRES_PAGE + rnd(64),
                                                np * KZR_VRES_PAGE - 128, frame, frame - 1, o, 4, NULL);
                if (n < 0) {
                    kzr_vres_epoch(&s);
                    continue;
                }
                draws++;
                for (int i = 0; i < n; i++)
                    if (memcmp(g_poolmem[o[i]], G + (size_t)(p + (unsigned)i) * KZR_VRES_PAGE, KZR_VRES_PAGE)) stale++;
            }
        }
        printf("acquire_epoch: draws %llu stale %llu bad %llu (stand %llu clean %llu) hold_maps %llu releases %llu\n",
               draws, stale, (unsigned long long)s.bad, (unsigned long long)s.bad_stand,
               (unsigned long long)s.bad_clean, (unsigned long long)s.hold_maps, (unsigned long long)s.hold_releases);
        CHECKP("acquire_epoch", stale == 0,
               "%llu stale reuses (ACQUIRE must end the epoch and hold until the label is met)", stale);
        CHECKP("acquire_epoch", s.bad == 0, "verify: %llu bad reuses", (unsigned long long)s.bad);
    }
    if (!neg || neg == 6) { /* arm_order / shared_reset / write_after */
        { /* a write during the copy, reset by another user's collect before an arm after the read would be taken */
            vinit(&s, 8, 0);
            g_naive = neg == 6;
            uint32_t o[1];
            g_race_page = 3;
            g_race_other = 1;
            kzr_vres_map_span(&s, (uintptr_t)G + 3 * KZR_VRES_PAGE, 10, 2, 1, o, 1, NULL);
            kzr_vres_epoch(&s);
            const int n = kzr_vres_map_span(&s, (uintptr_t)G + 3 * KZR_VRES_PAGE, 10, 3, 2, o, 1, NULL);
            CHECKP("arm_order", n == 1 && !memcmp(g_poolmem[o[0]], G + 3 * KZR_VRES_PAGE, KZR_VRES_PAGE),
                   "a write after the arm, collected by another user, was lost (the arm must precede the read)");
            CHECKP("shared_reset", n == 1 && !memcmp(g_poolmem[o[0]], G + 3 * KZR_VRES_PAGE, KZR_VRES_PAGE),
                   "a write another entry's collect reset was lost (a per-entry reset is not enough)");
        }
        { /* the same without the other user: conservative recopy (the copy raced a writer) */
            vinit(&s, 8, 0);
            uint32_t o[1];
            g_race_page = 4;
            g_race_other = 0;
            kzr_vres_map_span(&s, (uintptr_t)G + 4 * KZR_VRES_PAGE, 10, 2, 1, o, 1, NULL);
            kzr_vres_epoch(&s);
            const int n = kzr_vres_map_span(&s, (uintptr_t)G + 4 * KZR_VRES_PAGE, 10, 3, 2, o, 1, NULL);
            CHECKP("arm_order", n == 1 && !memcmp(g_poolmem[o[0]], G + 4 * KZR_VRES_PAGE, KZR_VRES_PAGE),
                   "a write during the copy is not recopied");
        }
        if (neg != 6) { /* a write after the copy: a new block, the old one intact */
            vinit(&s, 8, 0);
            uint32_t o1[1], o2[1];
            kzr_vres_map_span(&s, (uintptr_t)G + 5 * KZR_VRES_PAGE, 10, 2, 1, o1, 1, NULL);
            uint8_t old[KZR_VRES_PAGE];
            memcpy(old, g_poolmem[o1[0]], KZR_VRES_PAGE);
            kzr_vres_epoch(&s);
            guest_write(5);
            const int n = kzr_vres_map_span(&s, (uintptr_t)G + 5 * KZR_VRES_PAGE, 10, 3, 1, o2, 1, NULL);
            CHECKP("write_after",
                   n == 1 && o2[0] != o1[0] && !memcmp(g_poolmem[o2[0]], G + 5 * KZR_VRES_PAGE, KZR_VRES_PAGE) &&
                       !memcmp(g_poolmem[o1[0]], old, KZR_VRES_PAGE),
                   "the write got a new block and the old block (read by frame 2) changed");
        }
    }
    if (neg == 6) {
        printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
        return fails ? 1 : 0;
    }

    /* stress */
    vinit(&s, NB, verify);
    if (verify) s.peek = peek_block;
    g_blind = neg == 1;
    g_async = 1;
    uint32_t frame = 1, completed = 0;
    unsigned long long draws = 0, stale = 0, overwrite = 0, refused = 0, nwait = 0, gated = 0;
    unsigned stall = 0;
    memset(g_nrec, 0, sizeof g_nrec);
    for (int step = 0; step < 300000; step++) {
        const unsigned op = rnd(100);
        if (stall)
            stall--;
        else if (rnd(500) == 0)
            stall = 100 + rnd(100); /* the worker falls behind for ~100 steps */
        if (!stall && g_jn && rnd(3)) jobs_apply(&s, g_jobs[g_jh].seq + rnd(2), 1 + rnd(3));
        if (op < 20) { /* a guest write; legal ones follow a walker wait */
            if (neg != 3) kzr_vres_epoch(&s);
            guest_write(rnd(NP));
        } else if (op < 25) {
            kzr_vres_epoch(&s);
        } else if (op < 33) { /* frame end; the GPU completes frame - 3 (min_age alone covers 2) */
            frame++;
            const uint32_t nc = frame > 3 ? frame - 3 : 0;
            for (uint32_t f = completed + 1; f <= nc; f++) { /* everything frame f read must still be there */
                for (unsigned i = 0; i < g_nrec[f & 7]; i++)
                    if (kzr_vres_sum(g_poolmem[g_rec[f & 7][i].block]) != g_rec[f & 7][i].sum) overwrite++;
                g_nrec[f & 7] = 0;
            }
            completed = nc;
            kzr_vres_epoch(&s); /* a flip is a wait */
        } else {
            const unsigned p = rnd(NP - 6), off = rnd(KZR_VRES_PAGE), np = 1 + rnd(6);
            uint32_t bytes = np * KZR_VRES_PAGE - off - rnd(KZR_VRES_PAGE / 2);
            if (!bytes) bytes = 1;
            uint32_t o[8];
            uint64_t need = 0;
            const int n = kzr_vres_map_span(&s, (uintptr_t)G + (size_t)p * KZR_VRES_PAGE + off, bytes, frame,
                                            neg == 2 ? frame : completed, o, 8, &need);
            if (n < 0) {
                refused++;
                continue;
            }
            draws++;
            if (need) {
                nwait++;
                if (need > s.done_seq) gated++;
            }
            jobs_apply(&s, need, MAXJ); /* the draw's reads gate: its page copies are finished before it executes */
            for (int i = 0; i < n; i++) {
                if (memcmp(g_poolmem[o[i]], G + (size_t)(p + (unsigned)i) * KZR_VRES_PAGE, KZR_VRES_PAGE)) stale++;
                if (g_nrec[frame & 7] < 4096)
                    g_rec[frame & 7][g_nrec[frame & 7]++] = (rec_t){o[i], kzr_vres_sum(g_poolmem[o[i]])};
            }
        }
    }
    printf("stress%s%s: draws %llu refused %llu | stand %llu clean %llu copies %llu evicts %llu retires %llu collects "
           "%llu "
           "alloc-fail %llu bad %llu (stand %llu clean %llu; read %llu unread %llu) | draws with a copy seq %llu (%llu "
           "still pending at the map) pending-copy takes %llu | stale %llu in-flight overwrites %llu\n",
           neg ? " NEG=" : "", neg ? ne : "", draws, refused, (unsigned long long)s.reuse_stand,
           (unsigned long long)s.reuse_clean, (unsigned long long)s.copies, (unsigned long long)s.evicts,
           (unsigned long long)s.retires, (unsigned long long)s.collects, (unsigned long long)s.alloc_fail,
           (unsigned long long)s.bad, (unsigned long long)s.bad_stand, (unsigned long long)s.bad_clean,
           (unsigned long long)s.bad_read, (unsigned long long)s.bad_unread, nwait, gated, g_pending_copy, stale,
           overwrite);
    (void)g_stale_jobs;
    CHECKP("stale", stale == 0, "%llu stale pages", stale);
    CHECKP("overwrite", overwrite == 0, "%llu in-flight overwrites", overwrite);
    CHECKP("pending_copy", g_pending_copy == 0, "%llu blocks taken with their copy job pending", g_pending_copy);
    CHECKP("verify_bad", s.bad == 0, "verify: %llu bad reuses (stand %llu, clean %llu)", (unsigned long long)s.bad,
           (unsigned long long)s.bad_stand, (unsigned long long)s.bad_clean);
    if (verify)
        CHECKP("verify_split", s.bad_stand + s.bad_clean == s.bad && s.bad_read + s.bad_unread == s.bad,
               "verify split adds up");
    if (!neg) {
        CHECKP("coverage", s.reuse_stand > 0 && s.reuse_clean > 0 && s.evicts > 0 && s.retires > 0, "every path taken");
        CHECKP("coverage", draws > 100000, "most draws map (%llu)", draws);
        CHECKP("coverage", nwait > 0, "no draw ever waited on a page copy");
    }
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
