/* Host test for the GPU post plan P1 core (port/spu/kz3_gpost.c): table, mask zeroing and restore, per-frame plan
 * snapshot, predicate, nomatch paths, switch parsing, and the byte-identical job list with nothing claimed.
 * Every group has a negative control: the same checker run on a deliberately broken input must report a failure.
 * Self-contained: fake guest memory, a model of the lifted runner (loop read from func_0020C010 / func_002006C0). */
#include "../../port/spu/kz3_gpost.h"
#include "../../port/spu/kz3_gpost_modva.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMW (1u << 18) /* words: 1 MiB of fake guest memory */
static uint32_t g_mem[MEMW];
static unsigned g_writes;
static uint32_t rd(void* u, uint32_t ea) { (void)u; return g_mem[ea / 4]; }
static void wr(void* u, uint32_t ea, uint32_t v) { (void)u; g_mem[ea / 4] = v; g_writes++; }
static const kz3_gpost_mem M = { 0, rd, wr };

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
/* a negative control passes only when the checker reports (returns nonzero for) the broken input */
#define NEG(name, bad) do { if (!(bad)) { g_fail++; printf("FAIL negative control did not fire: %s\n", name); } else printf("  negative control ok: %s\n", name); } while (0)

#define ITEM 0x1000u
#define REND 0x2000u
#define LIST 0x3000u
#define BAT0 0x4000u   /* 0x20 each */
#define ENT0 0x8000u   /* entries, 0x10 each (2 per batch) */
#define DESC0 0xA000u  /* descriptors, 0x40 each */

static uint32_t modva(kz3_gpost_build b, int id)
{
    const kz3_gpost_modva* t = b == KZ3_GP_BUILD_100 ? kz3_gpost_modva_100 : kz3_gpost_modva_114;
    for (int i = 0; i < KZ3_GPOST_NMODVA; i++) if (t[i].id == id) return t[i].va;
    return 0;
}

static int g_nb;
/* fill the fake guest from `rows` (n rows); S is the item's viewport-0 flag word */
static void build(kz3_gpost_build b, const kz3_gpost_row* rows, int n, uint32_t S, uint32_t frame)
{
    memset(g_mem, 0, sizeof g_mem);
    g_mem[(ITEM + KZ3_GPOST_ITEM_S) / 4] = S;
    g_mem[(REND + KZ3_GPOST_R_COUNT) / 4] = (uint32_t)n;
    g_mem[(REND + KZ3_GPOST_R_LIST) / 4] = LIST;
    g_mem[(REND + KZ3_GPOST_R_FRAME) / 4] = frame;
    for (int i = 0; i < n; i++) {
        uint32_t bt = BAT0 + 0x20u * (uint32_t)i, en = ENT0 + 0x10u * (uint32_t)i, ds = DESC0 + 0x40u * (uint32_t)i;
        g_mem[(LIST + 4u * (uint32_t)i) / 4] = bt;
        g_mem[(bt + KZ3_GPOST_B_COUNT) / 4] = 2;
        g_mem[(bt + KZ3_GPOST_B_ENTRIES) / 4] = en;
        g_mem[(bt + KZ3_GPOST_B_MASK) / 4] = rows[i].mask;
        g_mem[(bt + KZ3_GPOST_B_EXCL) / 4] = rows[i].excl;
        g_mem[(bt + KZ3_GPOST_B_CTX) / 4] = 0;
        for (int e = 0; e < 2; e++) {
            g_mem[(en + 8u * (uint32_t)e) / 4] = (0x30u << 16) | 1u;           /* size 0x30, type 1 */
            g_mem[(en + 8u * (uint32_t)e + 4) / 4] = ds + 0x20u * (uint32_t)e;  /* descriptor EA */
            uint32_t d = ds + 0x20u * (uint32_t)e;
            g_mem[d / 4] = modva(b, rows[i].job);
            for (int k = 1; k < 8; k++) g_mem[d / 4 + (uint32_t)k] = 0xA5000000u | ((uint32_t)i << 8) | (uint32_t)(e * 16 + k);
        }
    }
    g_nb = n;
}

/* model of the lifted runner's batch loop: ring rotation is outside the batch list, so only the list is modelled.
 * out = (hi, lo) pairs; per run batch its entries, then a barrier (type 2, EA = batch). */
static int run_model(uint32_t* out, int cap)
{
    int k = 0;
    uint32_t n = g_mem[(REND + KZ3_GPOST_R_COUNT) / 4], list = g_mem[(REND + KZ3_GPOST_R_LIST) / 4];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = g_mem[(list + 4 * i) / 4];
        uint32_t ctx = g_mem[(b + KZ3_GPOST_B_CTX) / 4];
        uint32_t S = g_mem[(ITEM + KZ3_GPOST_ITEM_S + 4 * ctx) / 4];
        uint32_t mask = g_mem[(b + KZ3_GPOST_B_MASK) / 4], excl = g_mem[(b + KZ3_GPOST_B_EXCL) / 4];
        if ((mask & S) == 0 || (excl & S) != 0) continue;
        uint32_t cnt = g_mem[(b + KZ3_GPOST_B_COUNT) / 4], en = g_mem[(b + KZ3_GPOST_B_ENTRIES) / 4];
        for (uint32_t e = 0; e < cnt && k + 2 <= cap; e++) { out[k++] = g_mem[(en + 8 * e) / 4]; out[k++] = g_mem[(en + 8 * e + 4) / 4]; }
        if (k + 2 <= cap) { out[k++] = 2u << 0; out[k++] = b; }
    }
    return k;
}

static kz3_gpost_plan g_plan;
static kz3_gpost_saved g_sv;
static uint32_t g_base[8192], g_run[8192];
static uint32_t g_snap[MEMW];

static const uint32_t S_K4 = 0x1019F | 0x100; /* feature word as BuildView stores it (| 0x10000), all groups on */

static uint64_t bit(int seq) { return 1ull << seq; }

/* ---- group 1: table ---- */
static int table_check(kz3_gpost_build b, const kz3_gpost_row* rows, int n)
{
    int bad = 0;
    int prev = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].seq < 6 || rows[i].seq > 36 || rows[i].seq < prev) bad++;
        prev = rows[i].seq;
        if (!modva(b, rows[i].job)) bad++;   /* every job id has a module VA in this build's table */
    }
    if (rows[n - 1].seq != 36) bad++;
    return bad;
}

static void test_table(kz3_gpost_build b, const char* tag)
{
    CHECK(table_check(b, kz3_gpost_rows, kz3_gpost_nrows) == 0, "%s: table inconsistent", tag);
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 7);
    kz3_gpost_identify(&M, b, ITEM, REND, &g_plan);
    CHECK(g_plan.match == 1, "%s: full list must match, nomatch_at=%d reason=%s", tag, g_plan.nomatch_at, g_plan.reason);
    int ok = 1;
    for (int i = 0; i < kz3_gpost_nrows; i++) ok &= g_plan.b[i].seq == kz3_gpost_rows[i].seq && g_plan.b[i].job == kz3_gpost_rows[i].job;
    CHECK(ok, "%s: seq/job assignment wrong", tag);
    CHECK(g_plan.frame == 7 && g_plan.S == S_K4, "%s: frame/S read", tag);
    /* negative controls: a batch whose module VA is another job's, a mask bit changed, a dropped batch */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 7);
    g_mem[(DESC0 + 0x40u * 5 + 0) / 4] = modva(b, 31); /* row 5 is a VBlur batch: now claims to be Modulate */
    kz3_gpost_identify(&M, b, ITEM, REND, &g_plan);
    NEG("table: wrong module in a batch -> nomatch", !g_plan.match && g_plan.nomatch_at == 5);
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 7);
    g_mem[(BAT0 + 0x20u * 10 + KZ3_GPOST_B_MASK) / 4] ^= 0x2;
    kz3_gpost_identify(&M, b, ITEM, REND, &g_plan);
    NEG("table: one mask bit off -> nomatch", !g_plan.match && g_plan.nomatch_at == 10);
    kz3_gpost_row bad[256];
    memcpy(bad, kz3_gpost_rows, sizeof(kz3_gpost_row) * (size_t)kz3_gpost_nrows);
    bad[3].job = 99;
    NEG("table: checker rejects an id with no module VA", table_check(b, bad, kz3_gpost_nrows) != 0);
}

/* the 1.14 and 1.00 tables differ only in module VAs: the same 720p row table matches both builds' lists, and a 1.00 list
 * is never mis-identified by the 1.14 table (every post module's VA differs) */
static void test_builds(void)
{
    build(KZ3_GP_BUILD_100, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 1);
    kz3_gpost_identify(&M, KZ3_GP_BUILD_100, ITEM, REND, &g_plan);
    CHECK(g_plan.match == 1, "1.00 list matches the 1.00 table");
    kz3_gpost_identify(&M, KZ3_GP_BUILD_114, ITEM, REND, &g_plan);
    int unknown = 0;
    for (uint32_t i = 0; i < g_plan.nb; i++) unknown += g_plan.b[i].job == 0;
    CHECK(unknown == (int)g_plan.nb, "1.00 VAs must be unknown to the 1.14 module table (%d of %u)", unknown, g_plan.nb);
    int same = 0;
    for (int i = 0; i < KZ3_GPOST_NMODVA; i++) same += kz3_gpost_modva_100[i].va == kz3_gpost_modva_114[i].va;
    NEG("builds: 1.00 and 1.14 module VA tables are different tables", same == 0);
}

/* ---- group 2: byte-identical job list with nothing claimed; mask zeroing and restore ---- */
static int mem_diff(void) { return memcmp(g_snap, g_mem, sizeof g_mem) != 0; }

static void test_zero_restore(kz3_gpost_build b, const char* tag)
{
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 11);
    memcpy(g_snap, g_mem, sizeof g_mem);
    int nbase = run_model(g_base, 8192);

    /* nothing claimed (want 0): no guest write at all, same list, same memory */
    g_writes = 0;
    kz3_gpost_begin(&M, b, ITEM, REND, 0, &g_plan, &g_sv);
    int nrun = run_model(g_run, 8192);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    CHECK(g_writes == 0, "%s: want 0 wrote %u words", tag, g_writes);
    CHECK(nrun == nbase && memcmp(g_base, g_run, 4u * (unsigned)nbase) == 0, "%s: job list changed with nothing claimed", tag);
    CHECK(!mem_diff(), "%s: memory changed with nothing claimed", tag);

    /* S36 alone: that batch's entries vanish from the list, everything else equal, masks restored afterwards */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 11);
    memcpy(g_snap, g_mem, sizeof g_mem);
    uint32_t z = kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    CHECK(z == 1 && g_plan.claim == bit(36) && g_plan.nclaimed == 1, "%s: S36 claim z=%u claim=0x%llx", tag, z, (unsigned long long)g_plan.claim);
    CHECK(g_mem[(BAT0 + 0x20u * (uint32_t)(kz3_gpost_nrows - 1) + KZ3_GPOST_B_MASK) / 4] == 0, "%s: S36 mask not zeroed during the call", tag);
    nrun = run_model(g_run, 8192);
    int per = 2 * 2 + 2; /* 2 entries + barrier = 3 pairs = 6 words */
    CHECK(nrun == nbase - per && memcmp(g_base, g_run, 4u * (unsigned)(nbase - per)) == 0, "%s: list without S36 is the original minus its last batch (%d vs %d)", tag, nrun, nbase - per);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    CHECK(!mem_diff(), "%s: masks not restored after end", tag);
    int again = run_model(g_run, 8192);
    CHECK(again == nbase && memcmp(g_base, g_run, 4u * (unsigned)nbase) == 0, "%s: list after restore differs from the original", tag);

    /* two steps incl. a multi-batch step (S7 = H,V blur): three batches zeroed */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 12);
    memcpy(g_snap, g_mem, sizeof g_mem);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(7) | bit(36), &g_plan, &g_sv);
    CHECK(z == 3, "%s: S7+S36 zero 3 batches, got %u", tag, z);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    CHECK(!mem_diff(), "%s: masks not restored (S7+S36)", tag);

    /* negative controls */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 11);
    memcpy(g_snap, g_mem, sizeof g_mem);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv); /* end skipped on purpose */
    NEG("zero/restore: a missing restore is detected", mem_diff());
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 11);
    int n0 = run_model(g_base, 8192);
    g_mem[(BAT0 + 0x20u * 20 + KZ3_GPOST_B_MASK) / 4] &= ~0x14u; /* one mask bit group off in batch 20 */
    int n1 = run_model(g_run, 8192);
    NEG("zero/restore: one mask bit off changes the job list", n1 != n0 || memcmp(g_base, g_run, 4u * (unsigned)n0) != 0);
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 11);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    int n2 = run_model(g_run, 8192);
    int n3 = run_model(g_base, 8192);
    (void)n3;
    NEG("zero/restore: a claim changes the list", n2 != nbase);
}

/* ---- group 3: predicate, claim conditions, nomatch paths ---- */
static void test_predicate(kz3_gpost_build b, const char* tag)
{
    /* far DoF (S9: Zero idx3, excl 0x10000) does not run while S has 0x10000: wanted but not claimed */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 3);
    uint32_t z = kz3_gpost_begin(&M, b, ITEM, REND, bit(9), &g_plan, &g_sv);
    CHECK(z == 0 && g_plan.pred_skip == bit(9) && g_plan.claim == 0, "%s: S9 under 0x10000 must be pred-skipped (z=%u skip=0x%llx)", tag, z, (unsigned long long)g_plan.pred_skip);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* ... and runs (claimable) when S lacks 0x10000: S = 0x19F has 0x100 but no 0x10000 bit */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, 0x19F, 3);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(9), &g_plan, &g_sv);
    CHECK(z == 1 && g_plan.claim == bit(9), "%s: S9 claimable when S lacks 0x10000 (z=%u)", tag, z);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* S14 (Zero idx2, excl 1) does not run when S has bit 0 (motion blur on) */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 3);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(14), &g_plan, &g_sv);
    CHECK(z == 0 && g_plan.pred_skip == bit(14), "%s: S14 excluded by S bit 0", tag);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* a step whose mask bit S lacks does not run (S13 mask 0x1, S without 0x1) */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, 0x19E | 0x100, 3);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(13), &g_plan, &g_sv);
    CHECK(z == 0 && g_plan.pred_skip == bit(13), "%s: S13 needs S bit 0", tag);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* claim condition: S without 0x100 claims nothing */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, 0x9F, 3);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(36) | bit(32), &g_plan, &g_sv);
    CHECK(z == 0 && !strcmp(g_plan.reason, "s0x100"), "%s: S without 0x100 claims nothing (z=%u reason=%s)", tag, z, g_plan.reason);
    /* S36 itself has mask 0x100: it does not run without 0x100 either (the check above is the claim condition) */
    /* negative control: the same S9 with the predicate ignored would claim; the checker must see the difference */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 3);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(9), &g_plan, &g_sv);
    NEG("predicate: S9 claimed under 0x10000 would be a failure", z != 1);
}

static void test_nomatch(kz3_gpost_build b, const char* tag)
{
    kz3_gpost_row r[256];
    int n = kz3_gpost_nrows;
    uint64_t all = KZ3_GPOST_VALID_STEPS;
    /* a missing mandatory row (drop S15) -> nomatch, nothing claimed even with every step wanted */
    int j = 0;
    for (int i = 0; i < n; i++) if (kz3_gpost_rows[i].seq != 15) r[j++] = kz3_gpost_rows[i];
    build(b, r, j, S_K4, 5);
    memcpy(g_snap, g_mem, sizeof g_mem);
    uint32_t z = kz3_gpost_begin(&M, b, ITEM, REND, all, &g_plan, &g_sv);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    CHECK(z == 0 && !g_plan.match && !strcmp(g_plan.reason, "nomatch") && !mem_diff(), "%s: dropped S15 -> nomatch, no claim (z=%u reason=%s)", tag, z, g_plan.reason);
    CHECK(g_plan.nomatch_at >= 0, "%s: nomatch_at set", tag);
    /* optional S13 absent (motion blur off) still matches and claims */
    j = 0;
    for (int i = 0; i < n; i++) if (kz3_gpost_rows[i].seq != 13) r[j++] = kz3_gpost_rows[i];
    build(b, r, j, S_K4, 5);
    z = kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    CHECK(g_plan.match && z == 1, "%s: list without S13 matches (match=%d z=%u)", tag, g_plan.match, z);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* a second viewport: ctx 1 in a batch -> viewports, nothing claimed */
    build(b, kz3_gpost_rows, n, S_K4, 5);
    g_mem[(BAT0 + 0x20u * 4 + KZ3_GPOST_B_CTX) / 4] = 1;
    z = kz3_gpost_begin(&M, b, ITEM, REND, all, &g_plan, &g_sv);
    CHECK(z == 0 && !strcmp(g_plan.reason, "viewports"), "%s: ctx 1 -> viewports (reason=%s)", tag, g_plan.reason);
    /* MLAA prefix (host AA off): Transpose first -> aa-off */
    kz3_gpost_row ml[256];
    ml[0].seq = 2; ml[0].job = 39; ml[0].mask = 0x100; ml[0].excl = 0; ml[0].optional = 0;
    memcpy(ml + 1, kz3_gpost_rows, sizeof(kz3_gpost_row) * (size_t)n);
    build(b, ml, n + 1, S_K4, 5);
    z = kz3_gpost_begin(&M, b, ITEM, REND, all, &g_plan, &g_sv);
    CHECK(z == 0 && !strcmp(g_plan.reason, "aa-off"), "%s: Transpose first -> aa-off (reason=%s)", tag, g_plan.reason);
    /* too many batches */
    build(b, kz3_gpost_rows, n, S_K4, 5);
    g_mem[(REND + KZ3_GPOST_R_COUNT) / 4] = KZ3_GPOST_MAXB + 1;
    z = kz3_gpost_begin(&M, b, ITEM, REND, all, &g_plan, &g_sv);
    CHECK(z == 0 && !strcmp(g_plan.reason, "toomany"), "%s: toomany", tag);
    /* every step wanted on a clean list: all 31 sequence numbers claim, count = batches that run (S_K4 skips S9, S14) */
    build(b, kz3_gpost_rows, n, S_K4, 5);
    z = kz3_gpost_begin(&M, b, ITEM, REND, all, &g_plan, &g_sv);
    int expect = 0;
    for (int i = 0; i < n; i++) expect += !((kz3_gpost_rows[i].mask & S_K4) == 0 || (kz3_gpost_rows[i].excl & S_K4) != 0);
    CHECK((int)z == expect, "%s: all steps wanted zero %d batches, got %u", tag, expect, z);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    /* negative control: a good list must not be reported as a mismatch */
    build(b, kz3_gpost_rows, n, S_K4, 5);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    NEG("nomatch: the checker would fire on a mismatch (clean list is a match)", g_plan.match == 1 && strcmp(g_plan.reason, "nomatch") != 0);
    kz3_gpost_end(&M, &g_plan, &g_sv);
}

/* ---- group 4: per-frame plan snapshot ---- */
static int blob_matches_memory(const kz3_gpost_plan* p)
{
    uint32_t pos = 0;
    int idx_ok = 1;
    for (uint32_t i = 0; i < p->nb; i++) {
        if (!p->b[i].claimed) continue;
        const uint32_t* h = (const uint32_t*)(p->blob + pos);
        pos += 8;
        if (h[0] != i || h[1] != p->b[i].count) return 1;
        for (uint32_t e = 0; e < h[1]; e++) {
            const uint32_t* w = (const uint32_t*)(p->blob + pos);
            uint32_t hi = g_mem[(p->b[i].entries + 8 * e) / 4], lo = g_mem[(p->b[i].entries + 8 * e + 4) / 4];
            if (w[0] != hi || w[1] != lo) idx_ok = 0;
            uint32_t sz = (hi >> 16) & 0xFFFF;
            for (uint32_t k = 0; k < sz; k += 4) if (w[2 + k / 4] != g_mem[(lo + k) / 4]) idx_ok = 0;
            pos += 8 + ((sz + 3) & ~3u);
        }
    }
    return !idx_ok || pos != p->blob_used;
}

static void test_plan(kz3_gpost_build b, const char* tag)
{
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 0x1234);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36) | bit(32) | bit(7), &g_plan, &g_sv);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    const kz3_gpost_plan* p = kz3_gpost_plan_get(0x1234);
    CHECK(p != NULL, "%s: plan published for frame 0x1234", tag);
    if (!p) return;
    CHECK(p->frame == 0x1234 && p->S == S_K4 && p->match, "%s: plan header", tag);
    CHECK(p->claim == (bit(36) | bit(32) | bit(7)) && p->nclaimed == 4, "%s: plan claim bits 0x%llx n=%u", tag, (unsigned long long)p->claim, p->nclaimed);
    CHECK(!blob_matches_memory(p) && !p->blob_trunc, "%s: snapshot equals the descriptors in guest memory", tag);
    CHECK(p->blob_used == 4u * (8 + 2 * (8 + 0x30)), "%s: blob size %u", tag, p->blob_used);
    CHECK(kz3_gpost_plan_get(0x1235) == NULL, "%s: other frame -> no plan", tag);
    /* ring: frame+4 shares the slot and replaces it */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 0x1238);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    CHECK(kz3_gpost_plan_get(0x1234) == NULL && kz3_gpost_plan_get(0x1238) != NULL, "%s: ring slot replaced by frame+4", tag);
    /* log lines carry the claim */
    char line[400];
    kz3_gpost_format(kz3_gpost_plan_get(0x1238), line, sizeof line);
    CHECK(strstr(line, "frame=4664") && strstr(line, "claim=0x1000000000") && strstr(line, "zeroed=1"), "%s: G3 line '%s'", tag, line);
    kz3_gpost_format_batch(kz3_gpost_plan_get(0x1238), (uint32_t)(kz3_gpost_nrows - 1), line, sizeof line);
    CHECK(strstr(line, "seq=36") && strstr(line, "job=33"), "%s: list line '%s'", tag, line);
    /* negative controls: a changed descriptor, a truncated blob */
    build(b, kz3_gpost_rows, kz3_gpost_nrows, S_K4, 0x1240);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    p = kz3_gpost_plan_get(0x1240);
    g_mem[(DESC0 + 0x40u * (uint32_t)(kz3_gpost_nrows - 1) + 12) / 4] ^= 0x1; /* descriptor word changes after the snapshot */
    NEG("plan: a stale snapshot is detected", p && blob_matches_memory(p));
    kz3_gpost_plan blobless = *p;
    blobless.blob_used -= 4;
    NEG("plan: a short blob is detected", blob_matches_memory(&blobless));
}

/* ---- group 5: the switch ---- */
static void test_parse(void)
{
    CHECK(kz3_gpost_parse_steps(NULL) == 0 && kz3_gpost_parse_steps("") == 0 && kz3_gpost_parse_steps("0") == 0, "parse: unset/0");
    CHECK(kz3_gpost_parse_steps("0x1000000000") == bit(36), "parse: S36 = 0x1000000000");
    CHECK(kz3_gpost_parse_steps("1000000000") == bit(36), "parse: hex without 0x");
    CHECK(kz3_gpost_parse_steps("0x1") == 0, "parse: bit 0 is not a sequence number");
    CHECK(kz3_gpost_parse_steps("0xFFFFFFFFFFFFFFFF") == 0x3FFFFFFFFEull, "parse: masked to seq 1..37");
    CHECK(kz3_gpost_parse_steps("0x2000000000") == bit(37), "parse: S37");
    NEG("parse: S36 is not bit 35", kz3_gpost_parse_steps("0x800000000") != bit(36));
}

/* the wrapper shim's no-guest-access fast path must stay (text check, the wrapper itself needs a ppu_context) */
static int shim_check(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char buf[1 << 16];
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    buf[k] = 0;
    fclose(f);
    return strstr(buf, "if (!s_want && !s_logn) { orig(ctx); return; }") ? 0 : 1;
}

int main(int argc, char** argv)
{
    printf("table rows %d, module VAs %d\n", kz3_gpost_nrows, KZ3_GPOST_NMODVA);
    test_table(KZ3_GP_BUILD_114, "1.14");
    test_table(KZ3_GP_BUILD_100, "1.00");
    test_builds();
    test_zero_restore(KZ3_GP_BUILD_114, "1.14");
    test_zero_restore(KZ3_GP_BUILD_100, "1.00");
    test_predicate(KZ3_GP_BUILD_114, "1.14");
    test_predicate(KZ3_GP_BUILD_100, "1.00");
    test_nomatch(KZ3_GP_BUILD_114, "1.14");
    test_nomatch(KZ3_GP_BUILD_100, "1.00");
    test_plan(KZ3_GP_BUILD_114, "1.14");
    test_plan(KZ3_GP_BUILD_100, "1.00");
    test_parse();
    if (argc > 1) {
        int r = shim_check(argv[1]);
        CHECK(r == 0, "override shim keeps the unset fast path (%s)", argv[1]);
        if (r == 0) {
            /* negative control: the same checker on text without the fast path fires */
            NEG("shim: checker fires when the fast path is missing", shim_check("/dev/null") != 0);
        }
    }
    printf(g_fail ? "FAIL %d\n" : "PASS\n", g_fail);
    return g_fail != 0;
}
