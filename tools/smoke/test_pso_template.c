/* Host test for KZ3 patch 2071 (pipeline template): record layout, write/read, duplicate and bad-header handling,
 * the key set, the fingerprint finder over a synthetic "guest memory" buffer, and the merge tool
 * (tools/smoke/pso_template.py) on files written by the C side. No game data, no GPU.
 * Usage: test_pso_template <dir-for-temp-files> <path-to-pso_template.py> */
#include "rsx_pso_template.h"
#include "rsx_fp_decompiler.h"   /* rsx_fp_program_size / rsx_fp_structural_hash: the validators of the live finder (patch 2166 test) */
#include <time.h>
#include <assert.h>
#include <stddef.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void fill(rsx_pt_rec* r, uint64_t key, uint64_t vp, uint64_t fp)
{
    memset(r, 0, sizeof *r);
    r->key = key; r->vp_hash = vp; r->vp_first = vp * 3 + 1; r->fp_hash = fp; r->fp_first = fp * 5 + 7;
    r->vp_instrs = 12; r->fp_size = 64; r->rs_words = 50; r->rs[0] = (uint32_t)key; r->flags = RSX_PT_FLAG_BUFFERED;
}

typedef struct { int vp_hits, fp_hits; size_t last_off; const uint8_t* base; } hits_t;
static void on_hit(void* u, const uint8_t* p, size_t remaining, uint32_t kind, uint32_t id)
{
    hits_t* h = (hits_t*)u;
    (void)remaining; (void)id;
    if (kind == 0) h->vp_hits++; else h->fp_hits++;
    h->last_off = (size_t)(p - h->base);
}

/* the same FNV-1a over a u32 that ld_pso_key_tail applies to vp_branch_bits (rsx_live_draw.c fnv1a) */
static uint64_t fnv1a_u32(uint64_t h, uint32_t v)
{
    const uint8_t* p = (const uint8_t*)&v;
    for (int i = 0; i < 4; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- patch 2166: image pass at step 8, step-16 negative control, mutants -------------------------------------------------
 * The validator below is ld_pt_hit (rsx_live_draw.c) with the bookkeeping replaced by a record of what it accepted. */
typedef struct { uint32_t size; uint64_t hash; int found; size_t off; } synth_prog;
typedef struct { synth_prog vp[1], fp[1]; const uint8_t* base; size_t nraw; struct { size_t off; uint32_t kind, id; size_t rem; } raw[64]; } synth_t;
static uint64_t fnv_bytes(const uint8_t* d, uint32_t n, uint64_t h) { for (uint32_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h; }
static void synth_hit(void* u, const uint8_t* p, size_t remaining, uint32_t kind, uint32_t id)
{
    synth_t* t = (synth_t*)u;
    if (t->nraw < 64) { t->raw[t->nraw].off = (size_t)(p - t->base); t->raw[t->nraw].kind = kind; t->raw[t->nraw].id = id; t->raw[t->nraw].rem = remaining; t->nraw++; }
    if (kind == 0) {
        synth_prog* g = &t->vp[id];
        if (g->found || remaining < g->size) return;
        uint8_t host[256];
        for (uint32_t k = 0; k < g->size / 4u; k++) { host[4*k] = p[4*k+3]; host[4*k+1] = p[4*k+2]; host[4*k+2] = p[4*k+1]; host[4*k+3] = p[4*k]; }
        if (fnv_bytes(host, g->size, 1469598103934665603ull) == g->hash) { g->found = 1; g->off = (size_t)(p - t->base); }
    } else {
        synth_prog* g = &t->fp[id];
        if (g->found) return;
        const uint32_t lim = remaining > 0x10000u ? 0x10000u : (uint32_t)remaining;
        const uint32_t sz = rsx_fp_program_size(p, lim);
        if (sz != g->size) return;
        if (rsx_fp_structural_hash(p, sz, RSX_PT_FP_SEED) != g->hash) return;
        g->found = 1; g->off = (size_t)(p - t->base);
    }
}
static void put_fpw(uint8_t* d, uint32_t v) { v = (v << 16) | (v >> 16); d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16); d[2] = (uint8_t)(v >> 8); d[3] = (uint8_t)v; }
/* the old rsx_pt_scan loop, verbatim (before patch 2166), as the reference for "zero ranges = old behaviour" */
static size_t old_scan(const uint8_t* base, size_t len, const rsx_pt_qtab* t, rsx_pt_hit_fn hit, void* user)
{
    if (!t->cap) return 0;
    const uint32_t mask = t->cap - 1u;
    size_t blocks = 0;
    for (size_t o = 0; o + 16u <= len; o += 16u, blocks++) {
        const uint8_t* p = base + o;
        const uint64_t q = rsx_pt_quick(p);
        uint32_t i = (uint32_t)(q ^ (q >> 31)) & mask;
        while (t->e[i].q) { if (t->e[i].q == q) hit(user, p, len - o, t->e[i].kind, t->e[i].id); i = (i + 1u) & mask; }
    }
    return blocks;
}
static int raw_same(const synth_t* a, const synth_t* b)
{
    if (a->nraw != b->nraw) return 0;
    for (size_t i = 0; i < a->nraw; i++)
        if (a->raw[i].off != b->raw[i].off || a->raw[i].kind != b->raw[i].kind || a->raw[i].id != b->raw[i].id || a->raw[i].rem != b->raw[i].rem) return 0;
    return 1;
}

static void test_image_pass(void)
{
    const size_t LEN = 1u << 20, VPO = 0x40000, FPO = 0x80008;   /* VP at 0 mod 16, FP at 8 mod 16 */
    uint8_t* mem = (uint8_t*)calloc(1, LEN + 64);
    /* VP: 3 instructions of 4 BE words; FP: instr 0 plain, instr 1 reads a constant (type 2 in w1) and has END, + 16 B constant */
    uint8_t vp[48], fp[48];
    for (int i = 0; i < 48; i++) vp[i] = (uint8_t)(i * 13 + 5);
    memset(fp, 0, sizeof fp);
    put_fpw(fp + 0, 0x02000000u); put_fpw(fp + 4, 0x00000001u); put_fpw(fp + 8, 0x00000001u); put_fpw(fp + 12, 0x00000001u);
    put_fpw(fp + 16, 0x04000001u); put_fpw(fp + 20, 0x00000002u); put_fpw(fp + 24, 0x00000001u); put_fpw(fp + 28, 0x00000001u);   /* END, w1 const */
    for (int i = 0; i < 16; i++) fp[32 + i] = (uint8_t)(0x3F + i);                                                                  /* the constant slot */
    const uint32_t fp_size = 48;
    CHECK(rsx_fp_program_size(fp, 48) == fp_size);
    memcpy(mem + VPO, vp, 48); memcpy(mem + FPO, fp, 48);
    /* template values, ld_pt_note's formulas */
    uint8_t vpf[16]; memcpy(vpf, vp, 16);   /* vp_first = quick over the first 16 BE bytes as stored */
    uint8_t host[48];
    for (int k = 0; k < 12; k++) { host[4*k] = vp[4*k+3]; host[4*k+1] = vp[4*k+2]; host[4*k+2] = vp[4*k+1]; host[4*k+3] = vp[4*k]; }
    synth_t st; memset(&st, 0, sizeof st); st.base = mem;
    st.vp[0].size = 48; st.vp[0].hash = fnv_bytes(host, 48, 1469598103934665603ull);
    st.fp[0].size = fp_size; st.fp[0].hash = rsx_fp_structural_hash(fp, fp_size, RSX_PT_FP_SEED);
    rsx_pt_rec recs[3];
    for (int i = 0; i < 3; i++) { fill(&recs[i], 0x100 + (uint64_t)i, st.vp[0].hash, st.fp[0].hash); recs[i].vp_branch_bits = 0x11u * (uint32_t)i; recs[i].vp_instrs = 3; recs[i].fp_size = fp_size; }
    CHECK(sizeof(rsx_pt_rec) == 560 && offsetof(rsx_pt_rec, vp_branch_bits) == 108);   /* the v2 layout of patch 2165 */
    rsx_pt_qtab qt; CHECK(rsx_pt_qtab_build(&qt, 2));
    rsx_pt_qtab_add(&qt, rsx_pt_quick(vpf), 0, 0);
    rsx_pt_qtab_add(&qt, rsx_pt_quick(fp), 1, 0);
    const uint32_t BOTH = RSX_PT_KIND_VP | RSX_PT_KIND_FP;
#define QUEUEABLE(s) ((s).vp[0].found && (s).fp[0].found ? 3 : 0)

    /* the step-8 image pass finds both programs, 3/3 records queueable */
    synth_t a = st; a.nraw = 0;
    size_t blocks = rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &a, 8u, BOTH);
    CHECK(blocks == (LEN - 16) / 8 + 1);
    CHECK(a.vp[0].found && a.vp[0].off == VPO && a.fp[0].found && a.fp[0].off == FPO && (FPO & 15) == 8 && (VPO & 15) == 0);
    CHECK(QUEUEABLE(a) == 3);
    /* negative control: the step-16 scan finds the VP and misses the FP */
    synth_t b = st; b.nraw = 0;
    rsx_pt_scan(mem, LEN, &qt, synth_hit, &b);
    CHECK(b.vp[0].found && !b.fp[0].found && QUEUEABLE(b) == 0);
    /* kind mask: VP only / FP only */
    synth_t c = st; c.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &c, 8u, RSX_PT_KIND_VP);
    CHECK(c.vp[0].found && !c.fp[0].found);
    synth_t d = st; d.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &d, 8u, RSX_PT_KIND_FP);
    CHECK(!d.vp[0].found && d.fp[0].found);
    /* zero ranges = the old behaviour: the general pass (rsx_pt_scan) hit list, with remaining, equals the verbatim old loop's,
       also with a 16-aligned copy of the FP planted (found by both) */
    memcpy(mem + 0x20000, fp, 48);
    synth_t o1 = st, o2 = st; o1.nraw = o2.nraw = 0;
    CHECK(old_scan(mem, LEN, &qt, synth_hit, &o1) == rsx_pt_scan(mem, LEN, &qt, synth_hit, &o2));
    CHECK(raw_same(&o1, &o2) && o1.nraw == 2 && o1.fp[0].found && o1.fp[0].off == 0x20000);
    synth_t o3 = st; o3.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &o3, 16u, BOTH);
    CHECK(raw_same(&o1, &o3));
    memset(mem + 0x20000, 0, 48);

    /* mutants must fail */
    uint8_t save[48];
    memcpy(save, mem + FPO, 48);
    mem[FPO + 17] ^= 0x40;                       /* a fragment instruction word flipped: fingerprint (first 16 B) still matches, hash must not */
    synth_t m1 = st; m1.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &m1, 8u, BOTH);
    CHECK(m1.nraw >= 2 && !m1.fp[0].found);
    memcpy(mem + FPO, save, 48);
    put_fpw(mem + FPO + 16, 0x04000000u);        /* END moved: instr 1 no longer ends, the program runs on to a later END or none: size differs */
    put_fpw(mem + FPO + 48, 0x04000001u);
    synth_t m2 = st; m2.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &m2, 8u, BOTH);
    CHECK(m2.vp[0].found && !m2.fp[0].found);
    memcpy(mem + FPO, save, 48); memset(mem + FPO + 48, 0, 16);
    mem[VPO + 20] ^= 0x01;                       /* a vertex instruction word flipped (past the fingerprint) */
    synth_t m3 = st; m3.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &m3, 8u, BOTH);
    CHECK(!m3.vp[0].found && m3.fp[0].found);
    mem[VPO + 20] ^= 0x01;
    /* range end cut 16 B short of the program: remaining < size, both kinds refuse */
    synth_t m4 = st; m4.nraw = 0; rsx_pt_scan_step(mem + FPO, 32, &qt, synth_hit, &m4, 8u, BOTH);
    CHECK(!m4.fp[0].found);
    synth_t m5 = st; m5.nraw = 0; rsx_pt_scan_step(mem + VPO, 32, &qt, synth_hit, &m5, 8u, BOTH);
    CHECK(!m5.vp[0].found);
    synth_t m6 = st; m6.nraw = 0; rsx_pt_scan_step(mem + FPO, 48, &qt, synth_hit, &m6, 8u, BOTH);   /* exact end: found (control for m4) */
    CHECK(m6.fp[0].found);
    /* only the constant slot flipped: still matches (the structural hash skips constants) */
    mem[FPO + 40] ^= 0xFF;
    synth_t k1 = st; k1.nraw = 0; rsx_pt_scan_step(mem, LEN, &qt, synth_hit, &k1, 8u, BOTH);
    CHECK(k1.fp[0].found);
    mem[FPO + 40] ^= 0xFF;
    free(qt.e); free(mem);

    /* cost of the one-off image pass: a ToD-sized image (the plan's estimate: 8-10 MB, 154 table entries) */
    const size_t N = (size_t)9 << 20;
    uint8_t* big = (uint8_t*)malloc(N);
    uint64_t x = 88172645463325252ull;
    for (size_t i = 0; i < N; i += 8) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; memcpy(big + i, &x, 8); }
    rsx_pt_qtab q2; rsx_pt_qtab_build(&q2, 154);
    for (uint32_t i = 0; i < 154; i++) rsx_pt_qtab_add(&q2, (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull, i < 67 ? 0 : 1, 0);
    synth_t sink; memset(&sink, 0, sizeof sink); sink.base = big;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    const size_t nb = rsx_pt_scan_step(big, N, &q2, synth_hit, &sink, 8u, BOTH);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("image pass cost: %zu probes over %.1f MB at step 8 in %.2f ms (154-entry table, random data, host test machine)\n", nb, N / 1048576.0,
           (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
    free(q2.e); free(big);
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s DIR TOOL.py\n", argv[0]); return 2; }
    char a[512], b[512], m[512], bad[512], cmd[2048];
    snprintf(a, sizeof a, "%s/a.kzpt", argv[1]); snprintf(b, sizeof b, "%s/b.kzpt", argv[1]);
    snprintf(m, sizeof m, "%s/m.kzpt", argv[1]); snprintf(bad, sizeof bad, "%s/bad.kzpt", argv[1]);

    CHECK(sizeof(rsx_pt_hdr) == 24);
    CHECK(sizeof(rsx_pt_rec) == 560);      /* the Python tool's struct format has the same size */
    CHECK(offsetof(rsx_pt_rec, key) == 0);
    CHECK(RSX_PT_VERSION == 2);

    /* write / read, a duplicate key and a record with an impossible rs_words are dropped */
    rsx_pt_rec ra[4], rb[3];
    fill(&ra[0], 0x10, 1, 2); fill(&ra[1], 0x20, 1, 3); fill(&ra[2], 0x10, 1, 2); fill(&ra[3], 0x30, 4, 4);
    ra[2].vp_start = 8; ra[2].rs[2] = 0x80; ra[2].rs[3] = 1;   /* pass 73: a clash only in fields outside the pipeline */
    ra[3].rs_words = 65;
    fill(&rb[0], 0x20, 1, 3); fill(&rb[1], 0x40, 5, 6); fill(&rb[2], 0x05, 7, 8);
    CHECK(rsx_pt_write(a, ra, 4, RSX_PT_FLAG_BUFFERED) == 0);
    CHECK(rsx_pt_write(b, rb, 3, RSX_PT_FLAG_BUFFERED) == 0);
    rsx_pt_rec* got = NULL; uint32_t n = 0, dropped = 0, flags = 0;
    CHECK(rsx_pt_read(a, &got, &n, &dropped, &flags) == 0);
    CHECK(n == 2 && dropped == 2 && flags == RSX_PT_FLAG_BUFFERED);
    CHECK(n == 2 && got[0].key == 0x10 && got[0].vp_hash == 1 && got[1].key == 0x20);
    free(got);
    CHECK(rsx_pt_read("/nonexistent/x.kzpt", &got, &n, &dropped, &flags) == -1);
    { FILE* f = fopen(bad, "wb"); rsx_pt_hdr h = { RSX_PT_MAGIC, RSX_PT_VERSION, (uint32_t)sizeof(rsx_pt_rec), RSX_PT_KEYVER + 1, 0, 0 };
      fwrite(&h, sizeof h, 1, f); fclose(f); }
    CHECK(rsx_pt_read(bad, &got, &n, &dropped, &flags) == -2);   /* another key version is ignored */

    /* patch 2165: vp_branch_bits survives write/read; a v1 file (old version field) is refused */
    { char v[512]; rsx_pt_rec rv; snprintf(v, sizeof v, "%s/v.kzpt", argv[1]);
      fill(&rv, 0x77, 1, 2); rv.vp_branch_bits = 0x00A5C3F1u;
      CHECK(rsx_pt_write(v, &rv, 1, RSX_PT_FLAG_BUFFERED) == 0);
      CHECK(rsx_pt_read(v, &got, &n, &dropped, &flags) == 0 && n == 1 && got[0].vp_branch_bits == 0x00A5C3F1u);
      /* negative control: the key check of ld_pt_build is key_tail(rebuilt inputs) == r->key. The key folds vp_branch_bits
         (patch 2100), so rebuilding with 0 (no copy) must give another key than rebuilding with the stored bits. */
      if (n == 1) {
          const uint32_t stored = got[0].vp_branch_bits;
          uint64_t with = fnv1a_u32(0xCBF29CE484222325ull, stored), without = fnv1a_u32(0xCBF29CE484222325ull, 0u);
          CHECK(with != without);
      }
      free(got);
      FILE* f = fopen(v, "r+b"); CHECK(f != NULL);
      if (f) { uint32_t one = 1; fseek(f, 4, SEEK_SET); fwrite(&one, 4, 1, f); fclose(f); }   /* header.version = 1 */
      CHECK(rsx_pt_read(v, &got, &n, &dropped, &flags) == -2);
      snprintf(cmd, sizeof cmd, "python3 '%s' check '%s' 2> /dev/null", argv[2], v);
      CHECK(system(cmd) != 0); }

    /* key set */
    rsx_pt_set s; memset(&s, 0, sizeof s);
    int fresh = 0;
    for (uint64_t k = 1; k <= 20000; k++) fresh += rsx_pt_set_add(&s, k * 0x9E3779B97F4A7C15ull);
    CHECK(fresh == 20000);
    CHECK(rsx_pt_set_add(&s, 5 * 0x9E3779B97F4A7C15ull) == 0);
    CHECK(rsx_pt_set_add(&s, 0) == 1 && rsx_pt_set_add(&s, 0) == 0);
    free(s.a);

    /* finder: a 1 MB buffer with two planted blocks at 16-byte offsets, zeros elsewhere */
    uint8_t* mem = (uint8_t*)calloc(1, 1u << 20);
    uint8_t vpb[16], fpb[16];
    for (int i = 0; i < 16; i++) { vpb[i] = (uint8_t)(i * 7 + 3); fpb[i] = (uint8_t)(255 - i * 11); }
    memcpy(mem + 0x4560, vpb, 16); memcpy(mem + 0x91230, fpb, 16); memcpy(mem + 0xA0001, vpb, 16);   /* last one is unaligned */
    rsx_pt_qtab qt; CHECK(rsx_pt_qtab_build(&qt, 3));
    rsx_pt_qtab_add(&qt, rsx_pt_quick(vpb), 0, 0);
    rsx_pt_qtab_add(&qt, rsx_pt_quick(fpb), 1, 7);
    hits_t h; memset(&h, 0, sizeof h); h.base = mem;
    const size_t blocks = rsx_pt_scan(mem, 1u << 20, &qt, on_hit, &h);
    CHECK(blocks == (1u << 20) / 16);
    CHECK(h.vp_hits == 1 && h.fp_hits == 1);   /* the unaligned copy is not seen */
    CHECK(rsx_pt_quick(mem) != 0);              /* zero block: fingerprint is never 0 */
    free(qt.e); free(mem);

    test_image_pass();

    /* the merge tool on the C-written files */
    snprintf(cmd, sizeof cmd, "python3 '%s' merge '%s' '%s' '%s' > /dev/null", argv[2], m, a, b);
    CHECK(system(cmd) == 0);
    CHECK(rsx_pt_read(m, &got, &n, &dropped, &flags) == 0);
    CHECK(n == 4);   /* 0x10 0x20 (from a; a's duplicate 0x10 record is dropped by the C reader but kept by the tool's first-wins rule) 0x40 0x05 */
    if (n == 4) CHECK(got[0].key == 0x05 && got[1].key == 0x10 && got[2].key == 0x20 && got[3].key == 0x40);
    if (n == 4) CHECK(got[1].vp_start == 0 && got[1].rs[2] == 0);   /* first occurrence wins */
    free(got);
    /* pass 73: records of one key that differ in pipeline state (rs[14] = cull_face) make the merge fail */
    { char c[512], m2[512]; rsx_pt_rec rc; snprintf(c, sizeof c, "%s/c.kzpt", argv[1]); snprintf(m2, sizeof m2, "%s/m2.kzpt", argv[1]);
      fill(&rc, 0x20, 1, 3); rc.rs[14] = 1;
      CHECK(rsx_pt_write(c, &rc, 1, RSX_PT_FLAG_BUFFERED) == 0);
      remove(m2);
      snprintf(cmd, sizeof cmd, "python3 '%s' merge '%s' '%s' '%s' > /dev/null 2>&1", argv[2], m2, a, c);
      CHECK(system(cmd) != 0);
      FILE* f = fopen(m2, "rb"); CHECK(f == NULL); if (f) fclose(f); }
    snprintf(cmd, sizeof cmd, "python3 '%s' check '%s'", argv[2], m);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "python3 '%s' check '%s' 2> /dev/null", argv[2], bad);
    CHECK(system(cmd) != 0);
    snprintf(cmd, sizeof cmd, "python3 '%s' info '%s' > /dev/null", argv[2], m);
    CHECK(system(cmd) == 0);

    if (fails) { printf("test_pso_template: %d FAILED\n", fails); return 1; }
    printf("test_pso_template: PASS\n");
    return 0;
}
