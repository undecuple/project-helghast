/* Host test for KZ3 patch 2163 (program identity cache, rsx_pso_memo.h): the cached path must equal the reference path for
 * every derived value including the final key, over scripted sequences (A/B/A/B, rewrite at the same address, same bytes
 * at another address, size boundaries, no end bit, direct-mapped conflicts) and a fuzz; then a micro-benchmark against a
 * model of the old one-slot memos. Built with -DRSX_PM_TEST_FLAW (memcmp skipped) it must FAIL (negative control).
 * run() mirrors get_pso's order of operations; the derivations are the real decompiler functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "rsx_pso_memo.h"

static uint64_t fnv1a(const void* data, uint32_t n, uint64_t h)
{ const uint8_t* p = data; for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
/* verbatim vp_txl_unit_mask from rsx_live_draw.c */
static uint32_t ref_txl(const uint8_t* ucode, uint32_t instrs)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < instrs; i++) {
        const uint8_t* p = ucode + i * 16;
        const uint32_t d1 = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const uint32_t d2 = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

#define ARENA 0x200000u
static uint8_t g_mem[2][ARENA];
static uint8_t g_vp[544 * 16];
static const uint8_t* gp(uint32_t loc, uint32_t off, uint32_t n)
{ if (loc > 1 || (uint64_t)off + n > ARENA) return NULL; return g_mem[loc] + off; }

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

typedef struct { int fail; uint32_t vp_instrs, txl, fp_size, fp_units, nconst; uint64_t vp_h, fpkey, key, cons_h; } Out;

static int run(rsx_pm* pm, uint32_t start, uint32_t loc, uint32_t off, uint32_t mode, Out* o)
{
    memset(o, 0, sizeof *o);
    const uint8_t* vp_uc = g_vp + start * 16;
    const rsx_pm_vp* pv = NULL;
    if (pm) { pv = rsx_pm_vp_get(pm, vp_uc, start, (544 - start) * 16, fnv1a); o->vp_instrs = pv->instrs; }
    else o->vp_instrs = rsx_vp_program_size_instrs(vp_uc, (544 - start) * 16);
    if (!o->vp_instrs) { o->fail = 1; return 1; }
    const uint8_t* fp_uc = gp(loc, off, 16);
    if (!fp_uc) { o->fail = 2; return 1; }
    rsx_pm_fp* pf = NULL; uint32_t fp_size = 0; int same = 0;
    if (pm) { pf = rsx_pm_fp_lookup(pm, loc, off, fp_uc, gp); if (pf) { fp_size = pf->size; same = 1; } }
    if (!same) fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) { o->fail = 3; return 1; }
    fp_uc = gp(loc, off, fp_size);
    if (!fp_uc) { o->fail = 4; return 1; }
    if (pm && !same) pf = rsx_pm_fp_insert(pm, loc, off, fp_uc, fp_size);
    o->fp_size = fp_size;
    o->fp_units = pf ? rsx_pm_fp_units(pm, pf, fp_uc) : rsx_fp_texture_unit_mask(fp_uc, fp_size);
    o->txl = pv ? pv->txl : ref_txl(vp_uc, o->vp_instrs);
    rsx_fp_constant_block cb; cb.count = 0xDEADu;
    const int rc = pf ? rsx_pm_fp_consts(pm, pf, fp_uc, &cb) : rsx_fp_collect_constants(fp_uc, fp_size, &cb);
    if (rc < 0) { o->fail = 5; return 1; }
    o->nconst = cb.count;
    o->cons_h = fnv1a(cb.values, cb.count * 16u, fnv1a(&cb.count, 4, 1469598103934665603ull));
    o->vp_h = pv ? pv->h : fnv1a(vp_uc, o->vp_instrs * 16u, RSX_PM_VP_SEED);
    uint64_t key = o->vp_h, h = 0;
    if (pf && rsx_pm_fp_hash_get(pf, mode, key, &h)) key = h;
    else {
        const uint64_t seed = key;
        key = mode == 1u ? rsx_fp_structural_hash(fp_uc, fp_size, key) : fnv1a(fp_uc, fp_size, key);
        if (pf) rsx_pm_fp_hash_put(pf, mode, seed, key);
    }
    o->fpkey = key;
    const uint64_t pre = key;
    if (pv) { if (pv->flow) key = rsx_pm_vbs_key(key, start); }
    else key = rsx_vp_key_branch_base(key, vp_uc, o->vp_instrs * 16u, start);
    const uint32_t bits = 0x1234u;
    if (key != pre && (pv ? pv->bbits : rsx_vp_uses_branch_bits(vp_uc, o->vp_instrs * 16u))) key = fnv1a(&bits, 4, key);
    o->key = key;
    return 0;
}

static int g_fail = 0, g_checks = 0;
static void check(rsx_pm* pm, const char* what, uint32_t start, uint32_t loc, uint32_t off, uint32_t mode)
{
    Out a, b;
    run(pm, start, loc, off, mode, &a);
    run(NULL, start, loc, off, mode, &b);
    g_checks++;
    if (memcmp(&a, &b, sizeof a)) {
        if (g_fail < 12) printf("[FAIL] %s: start=%u loc=%u off=%x mode=%u fail %d/%d key %016llx/%016llx size %u/%u\n", what, start, loc, off, mode,
                                a.fail, b.fail, (unsigned long long)a.key, (unsigned long long)b.key, a.fp_size, b.fp_size);
        g_fail++;
    }
}

/* program builders: random instructions; the end bit is set only on the last one */
static void make_vp(uint8_t* d, uint32_t n_instr, int end)
{
    for (uint32_t i = 0; i < n_instr; i++) {
        uint8_t* p = d + i * 16;
        for (int k = 0; k < 16; k++) p[k] = (uint8_t)rnd();
        if (rnd() % 4 == 0) { uint32_t d1 = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;   /* TXL vec op */
            d1 = (d1 & ~(0x1Fu << 22)) | (0x19u << 22); p[4] = d1; p[5] = d1 >> 8; p[6] = d1 >> 16; p[7] = d1 >> 24; }
        if (rnd() % 16 == 0) { uint32_t d1 = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;   /* flow / BRB / CLB scalar op */
            static const uint32_t ops[] = {8, 9, 10, 11, 12, 0x11, 0x12};
            d1 = (d1 & ~(0x1Fu << 27)) | (ops[rnd() % 7] << 27); p[4] = d1; p[5] = d1 >> 8; p[6] = d1 >> 16; p[7] = d1 >> 24; }
        p[12] &= ~1u;
    }
    if (end && n_instr) d[(n_instr - 1) * 16 + 12] |= 1u;
}
/* FP words are big-endian with swapped half-words: the END bit (word bit 0) is raw[1] bit 0, the source register type
 * (bits 0..1 of words 1..3, 2 = inline CONST) is raw[5], raw[9], raw[13] bits 0..1. Constants only where `consts`. */
static void make_fp(uint8_t* d, uint32_t n_instr, int end)
{
    const int consts = rnd() % 3 == 0;
    for (uint32_t i = 0; i < n_instr; i++) {
        uint8_t* p = d + i * 16;
        for (int k = 0; k < 16; k++) p[k] = (uint8_t)rnd();
        p[1] &= ~1u;
        for (int k = 5; k < 16; k += 4) if ((p[k] & 3u) == 2u) p[k] ^= 1u;
        if (consts && i + 2 < n_instr && rnd() % 4 == 0) {
            const int sl = 5 + 4 * (int)(rnd() % 3);   /* one source slot becomes CONST: the next slot is its payload */
            p[sl] = (uint8_t)((p[sl] & ~3u) | 2u);
            i++; for (int k = 0; k < 16; k++) d[(i) * 16 + k] = (uint8_t)rnd();   /* the payload slot (any bytes) */
        }
    }
    if (end && n_instr) d[(n_instr - 1) * 16 + 1] |= 1u;
    if (rsx_fp_program_size(d, n_instr * 16) != (end ? n_instr * 16 : 0)) { printf("[FAIL] make_fp generator\n"); exit(2); }
}

static volatile uint64_t sink;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(void)
{
    static rsx_pm pm;
    const uint32_t mode_of[2] = {1, 2};
    /* 1. alternating A/B/A/B across two FP addresses and two VP programs, three N values */
    static uint8_t fpA[4096], fpB[4096], vpA[2048], vpB[2048];
    make_fp(fpA, 40, 1); make_fp(fpB, 60, 1); make_vp(vpA, 50, 1); make_vp(vpB, 70, 1);
    for (uint32_t n = 1; n <= 16; n *= 2) {
        rsx_pm_init(&pm, n, 0);
        memcpy(g_mem[1] + 0x1000, fpA, 40 * 16); memcpy(g_mem[1] + 0x2000, fpB, 60 * 16);
        memcpy(g_vp, vpA, 50 * 16); memcpy(g_vp + 8 * 16, vpB, 70 * 16);
        for (int i = 0; i < 400; i++) check(&pm, "alternating", (i & 1) ? 8 : 0, 1, (i & 2) ? 0x2000 : 0x1000, mode_of[(i >> 2) & 1]);
        if (n >= 4 && pm.fp_hit < 300) { printf("[FAIL] alternating n=%u: few hits (fp %llu vp %llu)\n", n, (unsigned long long)pm.fp_hit, (unsigned long long)pm.vp_hit); g_fail++; }
    }
    /* 2. rewrite of the same address (same size, different size, same program shifted) and of the VP in place */
    rsx_pm_init(&pm, 16, 0);
    memcpy(g_mem[1] + 0x3000, fpA, 40 * 16); memcpy(g_vp, vpA, 50 * 16);
    check(&pm, "rewrite-pre", 0, 1, 0x3000, 1); check(&pm, "rewrite-pre", 0, 1, 0x3000, 1);
    static uint8_t fpC[4096]; make_fp(fpC, 40, 1);
    memcpy(g_mem[1] + 0x3000, fpC, 40 * 16);                  /* same size, other bytes */
    check(&pm, "rewrite-same-size", 0, 1, 0x3000, 1);
    memcpy(g_mem[1] + 0x3000, fpB, 60 * 16);                  /* longer */
    check(&pm, "rewrite-longer", 0, 1, 0x3000, 1);
    memcpy(g_mem[1] + 0x3000, fpA, 40 * 16);                  /* shorter than the previous, tail still holds fpB's end */
    check(&pm, "rewrite-shorter", 0, 1, 0x3000, 1);
    { uint8_t save = g_mem[1][0x3000 + 5]; g_mem[1][0x3000 + 5] ^= 0x40; check(&pm, "rewrite-1bit", 0, 1, 0x3000, 1); g_mem[1][0x3000 + 5] = save; }
    memcpy(g_vp, vpB, 70 * 16); check(&pm, "vp-rewrite-longer", 0, 1, 0x3000, 1);
    memcpy(g_vp, vpA, 50 * 16); check(&pm, "vp-rewrite-shorter", 0, 1, 0x3000, 1);
    /* a VP whose first n bytes are unchanged but whose end bit was cleared and extended (cached n bytes equal, prefix is not an end) */
    { static uint8_t vpD[2048]; memcpy(vpD, vpA, 50 * 16); vpD[49 * 16 + 12] &= ~1u; make_vp(vpD + 50 * 16, 5, 1);
      memcpy(g_vp, vpD, 55 * 16); check(&pm, "vp-end-moved", 0, 1, 0x3000, 1); memcpy(g_vp, vpA, 50 * 16); }
    /* 3. same bytes at another address (FP: other offset and other location; VP: other start) */
    memcpy(g_mem[1] + 0x5000, fpA, 40 * 16); memcpy(g_mem[0] + 0x5000, fpA, 40 * 16);
    memcpy(g_vp + 20 * 16, vpA, 50 * 16);
    check(&pm, "same-bytes", 0, 1, 0x5000, 1); check(&pm, "same-bytes", 20, 1, 0x5000, 1);
    check(&pm, "same-bytes", 20, 0, 0x5000, 1); check(&pm, "same-bytes", 0, 0, 0x5000, 2); check(&pm, "same-bytes", 20, 1, 0x1000, 2);
    /* 4. size boundaries */
    { static uint8_t big[RSX_PM_FP_CAP + 64], edge[RSX_PM_FP_CAP], bigvp[544 * 16];
      make_fp(edge, RSX_PM_FP_CAP / 16, 1); memcpy(g_mem[1] + 0x10000, edge, RSX_PM_FP_CAP);
      check(&pm, "fp-cap", 0, 1, 0x10000, 1); check(&pm, "fp-cap", 0, 1, 0x10000, 1);
      uint64_t b0 = pm.big;
      make_fp(big, RSX_PM_FP_CAP / 16 + 4, 1); memcpy(g_mem[1] + 0x20000, big, RSX_PM_FP_CAP + 64);
      check(&pm, "fp-over-cap", 0, 1, 0x20000, 1); check(&pm, "fp-over-cap", 0, 1, 0x20000, 2);
      if (pm.big <= b0) { printf("[FAIL] over-cap programs were not flagged big\n"); g_fail++; }
      make_vp(bigvp, 544, 1); memcpy(g_vp, bigvp, 544 * 16); check(&pm, "vp-544", 0, 1, 0x3000, 1); check(&pm, "vp-544", 0, 1, 0x3000, 1);
      make_vp(bigvp, 543, 1); memcpy(g_vp + 16, bigvp, 543 * 16); check(&pm, "vp-543-at-1", 1, 1, 0x3000, 1); check(&pm, "vp-543-at-1", 1, 1, 0x3000, 1);
      make_vp(bigvp, 544, 0); memcpy(g_vp + 16, bigvp, 543 * 16); check(&pm, "vp-no-end-at-1", 1, 1, 0x3000, 1);
      make_vp(bigvp, 544, 0); memcpy(g_vp, bigvp, 544 * 16); check(&pm, "vp-no-end", 0, 1, 0x3000, 1); check(&pm, "vp-no-end", 0, 1, 0x3000, 1);
      g_vp[543 * 16 + 12] |= 1; check(&pm, "vp-end-last-slot", 0, 1, 0x3000, 1);
      memcpy(g_vp, vpA, 50 * 16);
      memset(g_mem[1] + 0x30000, 0, 0x10000); check(&pm, "fp-no-end", 0, 1, 0x30000, 1); check(&pm, "fp-no-end", 0, 1, 0x30000, 1);
      check(&pm, "fp-unmapped", 0, 1, ARENA - 8, 1);
    }
    /* 5. fuzz: random programs rewritten at random addresses with direct-mapped conflicts, all N, verify mode on and off */
    for (int vm = 0; vm < 2; vm++) for (uint32_t n = 1; n <= 16; n *= 2) {
        rsx_pm_init(&pm, n, vm);
        static uint8_t pf[40][2048], pvp[40][2048]; static uint32_t pfn[40], pvn[40];
        for (int i = 0; i < 40; i++) { pfn[i] = 4 + rnd() % 60; pvn[i] = 3 + rnd() % 120; make_fp(pf[i], pfn[i], 1); make_vp(pvp[i], pvn[i], 1); }
        for (int it = 0; it < 6000; it++) {
            const uint32_t fo = 0x40000 + (rnd() % 12) * 0x800, vs = (rnd() % 10) * 16;
            if (rnd() % 4 == 0) { const int k = rnd() % 40; memcpy(g_mem[1] + fo, pf[k], pfn[k] * 16); }
            if (rnd() % 4 == 0) { const int k = rnd() % 40; memcpy(g_vp + vs * 16, pvp[k], pvn[k] * 16); }
            check(&pm, "fuzz", vs, 1, fo, mode_of[rnd() & 1]);
        }
        if (vm && pm.mismatch) { printf("[FAIL] verify mode reported %llu mismatches on honest data\n", (unsigned long long)pm.mismatch); g_fail++; }
    }
    printf("[test_pso_memo] %d checks, %d failures\n", g_checks, g_fail);
    if (g_fail) return 1;

    /* 6. micro-benchmark: old one-slot memos vs the cache, miss-heavy alternating sequence (host, includes the real derivations) */
    {
        enum { NP = 8 };
        static uint8_t pf[NP][2048], pvp[NP][2048]; uint32_t pfn[NP], pvn[NP];
        for (int i = 0; i < NP; i++) { pfn[i] = 30 + (i * 7) % 40; pvn[i] = 40 + (i * 11) % 60; make_fp(pf[i], pfn[i], 1); make_vp(pvp[i], pvn[i], 1);
            memcpy(g_mem[1] + 0x60000 + i * 0x800, pf[i], pfn[i] * 16); memcpy(g_vp + i * 100 * 16 % (400 * 16) , pvp[i], pvn[i] * 16); }
        /* VP programs live at starts 0,100,200,300 (4 distinct, 2 draws sharing the same VP via i%4 below) */
        for (int i = 0; i < 4; i++) memcpy(g_vp + i * 100 * 16, pvp[i], pvn[i] * 16);
        const int iters = 400000;
        const int seqs[3] = {2, 4, 8};
        for (int si = 0; si < 3; si++) {
            const int span = seqs[si];
            /* old model: one FP memo (bytes, size, hash) and one VP memo (bytes, hash), everything else recomputed per draw */
            static struct { uint32_t n; uint64_t h; uint8_t b[544 * 16]; } ovp; static struct { uint32_t loc, off, size, hv, hm; uint64_t hs, h; uint8_t b[0x10000]; } ofp;
            memset(&ovp, 0, sizeof ovp); memset(&ofp, 0, sizeof ofp);
            double t0 = now();
            for (int i = 0; i < iters; i++) {
                const uint32_t k = i % span, fo = 0x60000 + k * 0x800, vs = (k % 4) * 100;
                const uint8_t* vp_uc = g_vp + vs * 16; const uint32_t vi = rsx_vp_program_size_instrs(vp_uc, (544 - vs) * 16);
                const uint8_t* fp_uc = gp(1, fo, 16); uint32_t fs = 0; int same = 0;
                if (ofp.size && ofp.loc == 1 && ofp.off == fo && !memcmp(gp(1, fo, ofp.size), ofp.b, ofp.size)) { fs = ofp.size; same = 1; }
                if (!same) fs = rsx_fp_program_size(fp_uc, 0x10000);
                fp_uc = gp(1, fo, fs);
                if (!same) { ofp.loc = 1; ofp.off = fo; ofp.size = fs; ofp.hv = 0; memcpy(ofp.b, fp_uc, fs); }
                uint32_t u = rsx_fp_texture_unit_mask(fp_uc, fs); rsx_fp_constant_block cb; rsx_fp_collect_constants(fp_uc, fs, &cb);
                uint32_t txl = ref_txl(vp_uc, vi);
                uint64_t vh; const uint32_t vn = vi * 16;
                if (vn == ovp.n && !memcmp(vp_uc, ovp.b, vn)) vh = ovp.h; else { vh = fnv1a(vp_uc, vn, RSX_PM_VP_SEED); ovp.n = vn; ovp.h = vh; memcpy(ovp.b, vp_uc, vn); }
                uint64_t key = vh;
                if (same && ofp.hv && ofp.hm == 1 && ofp.hs == key) key = ofp.h;
                else { const uint64_t seed = key; key = rsx_fp_structural_hash(fp_uc, fs, key); if (ofp.size == fs && ofp.off == fo) { ofp.hv = 1; ofp.hm = 1; ofp.hs = seed; ofp.h = key; } }
                const uint64_t pre = key; key = rsx_vp_key_branch_base(key, vp_uc, vn, vs);
                if (key != pre && rsx_vp_uses_branch_bits(vp_uc, vn)) key = fnv1a(&u, 4, key);
                sink += key + u + txl + cb.count;
            }
            const double old_ns = (now() - t0) / iters;
            rsx_pm_init(&pm, 16, 0); t0 = now();
            for (int i = 0; i < iters; i++) {
                const uint32_t k = i % span; Out o; run(&pm, (k % 4) * 100, 1, 0x60000 + k * 0x800, 1, &o); sink += o.key;
            }
            const double new_ns = (now() - t0) / iters;
            printf("[time] %d alternating programs: old one-slot %.0f ns/draw, cache %.0f ns/draw (%.1fx)\n", span, old_ns, new_ns, old_ns / new_ns);
        }
    }
    return 0;
}
