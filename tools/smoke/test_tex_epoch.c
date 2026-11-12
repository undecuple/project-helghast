/* Host test for KZ3 patch 2159 (texture-state epochs, rsx_tex_epoch.h): built by test_tex_epoch.sh against the patched
 * toolkit's rsx_dispatch.c. No game data, no GPU. argv[1] "neg" = negative control: the same random sequence with the
 * epoch bumps hidden (restored after every store); the verify counter must then report mismatches (exit 0 when it does,
 * 1 when it stays silent, i.e. the verify cannot see a stale epoch). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_tex_epoch.h"

#define TEX(u, w) (0x1A00u + (u) * 0x20u + (w) * 4u)
#define SZ1(u) (0x1840u + (u) * 4u)
static int fails;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] line %d: %s\n", __LINE__, #c); fails++; } } while (0)


/* Mirror of the binding-row paths in rsx_live_draw.c (ld_tm_raw, ld_te_record, ld_te_verify; that file is Windows-only and
 * cannot be linked here): a row stores the unit's 9 raw words; a verify after a row hit must find them bit-identical
 * (disabled-unit rows: still disabled). Returns 0 ok, 7/8 as in the runtime. */
static void te_raw(const rsx_dispatch* d, u32 u, u32 raw[9])
{
    for (u32 k = 0; k < 8u; k++) raw[k] = rsx_dsp_reg(d, 0x1A00u + u * 0x20u + k * 4u);
    raw[8] = rsx_dsp_reg(d, 0x1840u + u * 4u);
}
static void te_record(const rsx_dispatch* d, rsx_tex_l1* e, u32 u)
{
    te_raw(d, u, e->raw); e->kind = (e->raw[3] >> 31) ? 2 : 1; e->ep = d->tex_ep[u];
    e->surf_gen = e->zgen = e->epoch = 1;
}
static int te_verify(const rsx_dispatch* d, const rsx_tex_l1* e, u32 u)
{
    u32 raw[9]; te_raw(d, u, raw);
    if (e->kind == 1) return (raw[3] >> 31) ? 7 : 0;
    return memcmp(raw, e->raw, sizeof raw) ? 8 : 0;
}

static void deterministic(void)
{
    static rsx_dispatch d; rsx_dispatch_init(&d, NULL);
    for (u32 u = 0; u < 16; u++) { CHECK(d.tex_ep[u] != 0); for (u32 v = 0; v < u; v++) CHECK(d.tex_ep[u] != d.tex_ep[v]); }
    u32 e0[16]; memcpy(e0, d.tex_ep, sizeof e0);
    rsx_dispatch_method(&d, TEX(3, 0), 0x1000);                  /* offset only */
    CHECK(d.tex_ep[3] != e0[3]);
    for (u32 u = 0; u < 16; u++) if (u != 3) CHECK(d.tex_ep[u] == e0[u]);
    const u32 e3 = d.tex_ep[3];
    rsx_dispatch_method(&d, TEX(3, 0), 0x1000);                  /* same value: no bump */
    CHECK(d.tex_ep[3] == e3);
    rsx_dispatch_method(&d, TEX(3, 0), 0x1100);                  /* offset changes again */
    CHECK(d.tex_ep[3] != e3);
    const u32 e3b = d.tex_ep[3];
    rsx_dispatch_method(&d, SZ1(3), 0x00200100);                 /* SIZE1 (pitch/depth) */
    CHECK(d.tex_ep[3] != e3b);
    const u32 e3c = d.tex_ep[3];
    rsx_dispatch_method(&d, SZ1(3), 0x00200100);
    CHECK(d.tex_ep[3] == e3c);
    rsx_dispatch_method(&d, TEX(15, 7), 5);                      /* last word of the last unit */
    CHECK(d.tex_ep[15] != e0[15]);
    const u32 snap[16] = { 0 }; (void)snap;
    u32 before[16]; memcpy(before, d.tex_ep, sizeof before);
    rsx_dispatch_method(&d, 0x0324, 0x01010100);                 /* COLOR_MASK */
    rsx_dispatch_method(&d, 0x1C00, 0x3F800000);                 /* first vertex attr 4F word: just past the block */
    rsx_dispatch_method(&d, 0x19FC, 0x1234);                     /* just below the block */
    rsx_dispatch_method(&d, 0x183C, 0x1234);                     /* just below SIZE1 */
    rsx_dispatch_method(&d, 0x1880, 0x3F800000);                 /* just above SIZE1 (DATA2F) */
    CHECK(!memcmp(before, d.tex_ep, sizeof before));
    /* decode cache + binding row */
    static rsx_tex_dec dc[16]; rsx_tex_dec_stat st; memset(&st, 0, sizeof st);
    rsx_dsp_texture t, f;
    rsx_dispatch_method(&d, TEX(2, 0), 0x2000);
    rsx_dispatch_method(&d, TEX(2, 3), 0x80000000u);
    rsx_tex_dec_get(dc, &d, 2, &t, 1, &st); CHECK(st.miss == 1 && t.offset == 0x2000 && t.enabled);
    rsx_tex_dec_get(dc, &d, 2, &t, 1, &st); CHECK(st.hit == 1 && t.offset == 0x2000);
    rsx_dispatch_method(&d, TEX(2, 0), 0x2000);                  /* same value */
    rsx_tex_dec_get(dc, &d, 2, &t, 1, &st); CHECK(st.hit == 2 && st.miss == 1);
    rsx_dispatch_method(&d, TEX(2, 0), 0x3000);                  /* offset-only change must be seen */
    rsx_tex_dec_get(dc, &d, 2, &t, 1, &st); CHECK(st.miss == 2 && t.offset == 0x3000);
    rsx_tex_dec_get(dc, &d, 2, &t, 2, &st); CHECK(st.bad == 0 && st.hit == 3);
    rsx_tex_dec_get(dc, &d, 5, &f, 0, &st); CHECK(st.hit == 3 && st.miss == 2);   /* mode 0 touches nothing */
    rsx_dsp_get_texture(&d, 2, &f); CHECK(!memcmp(&f, &t, sizeof f));
    rsx_tex_l1 row; memset(&row, 0, sizeof row);
    row.kind = 2; row.ep = d.tex_ep[2]; row.surf_gen = 4; row.zgen = 6; row.epoch = 9;
    CHECK(rsx_tex_l1_hit(&row, &d, 2, 4, 6, 9));
    rsx_dispatch_method(&d, TEX(2, 0), 0x3000);                  /* same value */
    CHECK(rsx_tex_l1_hit(&row, &d, 2, 4, 6, 9));
    rsx_dispatch_method(&d, TEX(7, 0), 0x9000);                  /* another unit */
    CHECK(rsx_tex_l1_hit(&row, &d, 2, 4, 6, 9));
    CHECK(!rsx_tex_l1_hit(&row, &d, 2, 5, 6, 9) && !rsx_tex_l1_hit(&row, &d, 2, 4, 7, 9) && !rsx_tex_l1_hit(&row, &d, 2, 4, 6, 10));
    row.kind = 1;                                                /* disabled-unit row ignores the generations */
    CHECK(rsx_tex_l1_hit(&row, &d, 2, 99, 99, 99));
    rsx_dispatch_method(&d, TEX(2, 3), 0x80000000u);             /* same */
    CHECK(rsx_tex_l1_hit(&row, &d, 2, 1, 1, 1));
    rsx_dispatch_method(&d, TEX(2, 3), 0);                       /* enable bit cleared */
    CHECK(!rsx_tex_l1_hit(&row, &d, 2, 1, 1, 1));
    row.ep = d.tex_ep[2]; row.kind = 2; row.surf_gen = row.zgen = row.epoch = 1;
    rsx_dispatch_method(&d, TEX(2, 8 - 1), 0xAA);                /* border colour word */
    CHECK(!rsx_tex_l1_hit(&row, &d, 2, 1, 1, 1));
    row.ep = d.tex_ep[2];
    u32 regs[0x4000 / 4]; memset(regs, 0, sizeof regs);
    rsx_dispatch_seed_registers(&d, regs, 0x4000 / 4);           /* a re-seed voids every row */
    CHECK(!rsx_tex_l1_hit(&row, &d, 2, 1, 1, 1));
    /* binding-row record / verify */
    {
        rsx_tex_l1 r; rsx_dispatch_method(&d, TEX(9, 3), 0x80000000u); rsx_dispatch_method(&d, TEX(9, 0), 0x4000);
        te_record(&d, &r, 9); CHECK(r.kind == 2);
        CHECK(rsx_tex_l1_hit(&r, &d, 9, 1, 1, 1) && te_verify(&d, &r, 9) == 0);
        rsx_dispatch_method(&d, TEX(9, 0), 0x4000);               /* same value */
        CHECK(rsx_tex_l1_hit(&r, &d, 9, 1, 1, 1) && te_verify(&d, &r, 9) == 0);
        rsx_dispatch_method(&d, SZ1(9), 0x00100100);              /* SIZE1 only */
        CHECK(!rsx_tex_l1_hit(&r, &d, 9, 1, 1, 1) && te_verify(&d, &r, 9) == 8);
        te_record(&d, &r, 9);
        rsx_dispatch_method(&d, TEX(9, 7), 0x77);                 /* last word of the block */
        CHECK(!rsx_tex_l1_hit(&r, &d, 9, 1, 1, 1) && te_verify(&d, &r, 9) == 8);
        rsx_tex_l1 off; te_record(&d, &off, 12);                  /* disabled unit */
        CHECK(off.kind == 1 && rsx_tex_l1_hit(&off, &d, 12, 1, 1, 1) && te_verify(&d, &off, 12) == 0);
        rsx_dispatch_method(&d, TEX(12, 0), 0x500);               /* offset on a disabled unit still bumps, verify stays ok */
        CHECK(!rsx_tex_l1_hit(&off, &d, 12, 1, 1, 1) && te_verify(&d, &off, 12) == 0);
        te_record(&d, &off, 12);
        rsx_dispatch_method(&d, TEX(12, 3), 0x80000000u);
        CHECK(!rsx_tex_l1_hit(&off, &d, 12, 1, 1, 1) && te_verify(&d, &off, 12) == 7);
    }
    rsx_dispatch d2; rsx_dispatch_init(&d2, NULL);               /* a re-init hands out fresh epochs too */
    for (u32 u = 0; u < 16; u++) CHECK(d2.tex_ep[u] != d.tex_ep[u]);
}

/* Random method stream; after each store 0-2 random decode calls in verify mode (2). */
static int random_run(int neg)
{
    static rsx_dispatch d; rsx_dispatch_init(&d, NULL);
    static rsx_tex_dec dc[16]; rsx_tex_dec_stat st; memset(&st, 0, sizeof st);
    static rsx_tex_l1 row[16];
    for (u32 u = 0; u < 16; u++) te_record(&d, &row[u], u);
    u64 rowbad = 0, rowhit = 0;
    srand(12345);
    for (int i = 0; i < 200000; i++) {
        u32 m;
        switch (rand() % 4) {
        case 0: m = TEX(rand() % 16, rand() % 8); break;
        case 1: m = SZ1(rand() % 16); break;
        case 2: m = 0x1A00u - 8u + (rand() % 4) * 4u; break;      /* neighbours: the word just below unit 0 and unit 0's first words (stores to unit 0 do bump) */
        default: m = 0x1C00u + (rand() % 8) * 4u; break;
        }
        const u32 arg = (rand() & 3) ? (u32)(rand() % 3) : ((u32)rand() << 1) ^ (u32)(rand() & 1);   /* full 32 bits (bit 31 = unit enable); many repeats of a few values */
        u32 keep[16]; memcpy(keep, d.tex_ep, sizeof keep);
        rsx_dispatch_method(&d, m, arg);
        if (neg) memcpy(d.tex_ep, keep, sizeof keep);              /* an epoch that ignores changes */
        for (u32 u = 0; u < 16; u++) {                              /* binding rows: a hit must verify; re-record on a miss */
            if (rsx_tex_l1_hit(&row[u], &d, u, 1, 1, 1)) { rowhit++; if (te_verify(&d, &row[u], u)) rowbad++; }
            else te_record(&d, &row[u], u);
        }
        for (int k = rand() % 3; k > 0; k--) { rsx_dsp_texture t; rsx_tex_dec_get(dc, &d, (u32)(rand() % 16), &t, 2, &st); }
    }
    printf("[random%s] decode hits=%llu miss=%llu bad=%llu | rows hit=%llu bad=%llu\n", neg ? " NEG" : "", st.hit, st.miss, st.bad,
           (unsigned long long)rowhit, (unsigned long long)rowbad);
    return neg ? (st.bad > 0 && rowbad > 0 ? 0 : 1) : (st.bad == 0 && rowbad == 0 && rowhit > 1000 && st.hit > 1000 && st.miss > 1000 ? 0 : 1);
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "neg")) return random_run(1);
    /* pass 151: opt-in default; unset/empty/0 off, 1 on, verify = 2 */
    if (rsx_tex_epoch_parse(NULL) != 0 || rsx_tex_epoch_parse("") != 0 || rsx_tex_epoch_parse("0") != 0 ||
        rsx_tex_epoch_parse("1") != 1 || rsx_tex_epoch_parse("verify") != 2) { printf("[FAIL] epoch parse defaults\n"); fails++; }
    deterministic();
    if (random_run(0)) { printf("[FAIL] random run\n"); fails++; }
    printf(fails ? "[test_tex_epoch] FAIL %d\n" : "[test_tex_epoch] PASS\n", fails);
    return fails ? 1 : 0;
}
