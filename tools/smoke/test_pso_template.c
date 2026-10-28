/* Host test for KZ3 patch 2071 (pipeline template): record layout, write/read, duplicate and bad-header handling,
 * the key set, the fingerprint finder over a synthetic "guest memory" buffer, and the merge tool
 * (tools/smoke/pso_template.py) on files written by the C side. No game data, no GPU.
 * Usage: test_pso_template <dir-for-temp-files> <path-to-pso_template.py> */
#include "rsx_pso_template.h"
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

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s DIR TOOL.py\n", argv[0]); return 2; }
    char a[512], b[512], m[512], bad[512], cmd[2048];
    snprintf(a, sizeof a, "%s/a.kzpt", argv[1]); snprintf(b, sizeof b, "%s/b.kzpt", argv[1]);
    snprintf(m, sizeof m, "%s/m.kzpt", argv[1]); snprintf(bad, sizeof bad, "%s/bad.kzpt", argv[1]);

    CHECK(sizeof(rsx_pt_hdr) == 24);
    CHECK(sizeof(rsx_pt_rec) == 556);      /* the Python tool's struct format has the same size */
    CHECK(offsetof(rsx_pt_rec, key) == 0);

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
