/* Host test for the GPU post plan P2 snapshot (port/spu/kz3_gpost.c, kz3_gpost_p2_parse / p2_snapshot): follows each claimed
 * step's WWS command list, user block D, input/RMW lists and parameter list/blocks over a fake guest memory.
 * The fixture mirrors the builders of the parameter layout (commands, order, size formula, D, lists), for every
 * row of the 1.14 and 1.00 sequence tables. Cases from s.11: S6, S13, S15, S17, S31, S35, a split-tile (grid) case, and negative
 * controls for each rejection (wrong size, truncated list, pointer out of range, stale frame, type bits flipped, pad dropped).
 * Also proves nothing is written and the job list is byte-identical with nothing claimed (KZ3_GPU_POST_STEPS unset), and that a
 * claimed frame's guest memory is identical after end() (P2 only reads). Self-contained, no game files.
 * Pass 164b adds the cases the first mutation check missed (tools/spu/mut_gpost_p2.sh applies each mutant and needs this
 * test to fail): range checks at the 0xC0000000 edge (a 4 KB window of fake memory ends exactly there), the odd-row pad
 * element, the blob and step capacities, and the 64-bit products (a wrapped 32-bit tile address, pitch*h). */
#include "../../port/spu/kz3_gpost.h"
#include "../../port/spu/kz3_gpost_modva.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMW (1u << 20) /* words: 4 MiB */
static uint32_t g_mem[MEMW];
#define HIWIN 0xBFFFF000u /* a 4 KB window of fake memory that ends exactly at KZ3_GPOST_EA_HI (0xC0000000) */
static uint32_t g_hi[0x1000 / 4];
static unsigned g_writes, g_reads;
static uint32_t rd(void* u, uint32_t ea)
{
    (void)u;
    g_reads++;
    if (ea >= HIWIN && ea < KZ3_GPOST_EA_HI) return g_hi[(ea - HIWIN) / 4];
    return ea / 4 < MEMW ? g_mem[ea / 4] : 0xDEADBEEFu;
}
static void wr(void* u, uint32_t ea, uint32_t v)
{
    (void)u;
    g_mem[ea / 4] = v;
    g_writes++;
}
static const kz3_gpost_mem M = {0, rd, wr};

static int g_fail;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            g_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)
#define NEG(name, bad)                                                                                                 \
    do {                                                                                                               \
        if (!(bad)) {                                                                                                  \
            g_fail++;                                                                                                  \
            printf("FAIL negative control did not fire: %s\n", name);                                                  \
        } else                                                                                                         \
            printf("  negative control ok: %s\n", name);                                                               \
    } while (0)

#define ITEM 0x1000u
#define REND 0x10000u
#define LIST 0x14000u
#define BAT0 0x14400u
#define CUR0 0x20000u
#define PSRC 0x180000u /* parameter source blocks, 0x1000 per batch */
#define SURFEA(i) (0x10000000u + (uint32_t)(i) * 0x00800000u)
#define MAXT 128

static uint32_t modva(kz3_gpost_build b, int id)
{
    const kz3_gpost_modva* t = b == KZ3_GP_BUILD_100 ? kz3_gpost_modva_100 : kz3_gpost_modva_114;
    for (int i = 0; i < KZ3_GPOST_NMODVA; i++)
        if (t[i].id == id) return t[i].va;
    return 0;
}

typedef struct {
    int job, mode, nin, rmw, extra, np;
    uint32_t psz[KZ3_GPOST_P2_MAXPARAM], pea[KZ3_GPOST_P2_MAXPARAM];
    uint32_t cols, rows, tw, th, bpp, pitch, bx[4], by[4], x0, y0, hiflags;
    int surf[4], dsurf;
} spec_t;

typedef struct {
    uint32_t B, ent, ntile, plist, nel_p, size;
    uint32_t cl[MAXT], inl[MAXT][4], rmwl[MAXT];
    spec_t sp;
    int seq, job, ncmd;
} fb_t;
static uint32_t g_ov_cols, g_ov_rows; /* grid override for test_grids */
static uint32_t g_ov_th, g_ov_tw,
    g_ov_pitch; /* tile height / width / pitch overrides (0 = the default 90 / 160 / 5120) */
static fb_t g_b[KZ3_GPOST_MAXB];
static int g_nb;
static uint32_t g_cur;

static uint32_t alloc(uint32_t bytes, uint32_t al)
{
    g_cur = (g_cur + al - 1) & ~(al - 1);
    uint32_t a = g_cur;
    g_cur += bytes;
    return a;
}
static void put64(uint32_t ea, uint32_t hi, uint32_t lo)
{
    g_mem[ea / 4] = hi;
    g_mem[ea / 4 + 1] = lo;
}
static uint32_t cmdR(int idx, int cnt, int pg, int pgs, uint32_t* hi)
{
    *hi = 0x200u | ((uint32_t)idx << 5);
    return ((uint32_t)pg << 24) | ((uint32_t)pgs << 16) | ((uint32_t)cnt << 8);
}
static uint32_t cmdU(uint32_t sz, int A, int B, int f, int idx, uint32_t ea, uint32_t* hi)
{
    *hi = ((sz / 16) << 18) | ((uint32_t)A << 15) | ((uint32_t)B << 14) | ((uint32_t)f << 12) | (2u << 9) |
          ((uint32_t)idx << 5);
    return ea;
}

static int is_rmw_seq(int seq, int job)
{
    return job == 24 /* DoF */ || job == 30 /* ILRSharp */ ||
           (job == 33 && (seq == 10 || seq == 22 || seq == 30 || seq == 32 || seq == 36));
}

static void make_spec(int i, const kz3_gpost_row* r, spec_t* s)
{
    memset(s, 0, sizeof *s);
    s->job = r->job;
    switch (r->job) {
    case 22:
        s->nin = 2;
        s->np = 1;
        s->psz[0] = 0x220;
        break; /* BloomCapture */
    case 23:
        s->nin = 1;
        s->np = 1;
        s->psz[0] = 0x10;
        break; /* BloomIntegrate */
    case 24:
        s->nin = 2;
        s->extra = 1;
        break; /* DoF */
    case 25:
        s->nin = 1;
        s->np = 1;
        s->psz[0] = 0x10;
        break; /* DepthToFuzzy */
    case 26:
    case 28:
    case 29:
    case 33:
    case 34:
        s->nin = 1;
        break; /* Downsample HBlur ILR Upsample VBlur */
    case 27:
        s->nin = 2;
        s->np = 1;
        s->psz[0] = 0x210;
        break; /* GrainWeight */
    case 30:
        s->nin = 0;
        break; /* ILRSharp: chunks below */
    case 31:
        s->nin = 1;
        s->np = 1;
        s->psz[0] = 0x10;
        break; /* Modulate */
    case 32:
        s->nin = 2;
        s->extra = 1;
        break; /* MotionBlur */
    case 35:
        s->nin = 1;
        s->np = 1;
        s->psz[0] = 0x10;
        break; /* Vignette */
    case 37:
        s->nin = 0;
        break; /* Zero */
    default:
        break;
    }
    s->rmw = is_rmw_seq(r->seq, r->job);
    s->mode = s->rmw ? (r->job == 24 || (r->job == 33 && (r->seq == 32 || r->seq == 36)) ? 2 : 3) : 1;
    s->cols = g_ov_cols ? g_ov_cols : 4;
    s->rows = g_ov_cols ? g_ov_rows : 3;
    s->tw = g_ov_tw ? g_ov_tw : 160;
    s->th = g_ov_th ? g_ov_th : 90;
    s->bpp = 4;
    s->pitch = g_ov_pitch ? g_ov_pitch : 5120;
    s->x0 = 16;
    s->y0 = 8;
    for (int k = 0; k < s->nin; k++) {
        s->surf[k] = (i * 3 + k * 7 + 1) % KZ3_GPOST_NSURF;
        s->bx[k] = s->by[k] = 0;
    }
    if (r->job == 32) {
        s->bx[0] = s->by[0] = 8;
    } /* MotionBlur item0 {surf, 8, 8} */
    if (r->job == 24) {
        s->bx[0] = s->bx[1] = 8;
        s->by[0] = s->by[1] = 7;
    }
    if (r->job == 28) s->bx[0] = 4;
    if (r->job == 34) s->by[0] = 3;
    s->dsurf = (i * 3 + 5) % KZ3_GPOST_NSURF;
    s->hiflags = r->seq == 7 ? 0x4000u : r->seq == 11 ? 0x2000u : 0;
    for (int j = 0; j < s->np; j++) s->pea[j] = PSRC + 0x1000u * (uint32_t)i + 0x100u * (uint32_t)j;
    if (r->job == 30) { /* 0x20 block, then idx9 (160x90x4, pitch 640) in 0x4000 chunks */
        s->np = 1;
        s->psz[0] = 0x20;
        s->pea[0] = PSRC + 0x1000u * (uint32_t)i;
        uint32_t rest = 640u * 90u;
        for (uint32_t k = 0; rest; k++) {
            uint32_t sz = rest > 0x4000 ? 0x4000 : rest;
            s->psz[s->np] = (sz + 15) & ~15u;
            s->pea[s->np] = SURFEA(9) + k * 0x4000;
            s->np++;
            rest -= sz;
        }
    }
}

static int ncmd_of(const spec_t* s)
{
    return 5 + 2 * (s->mode != 0) + 2 * s->rmw + 2 * s->nin + 2 * (s->extra != 0) + 2 * (s->np != 0);
}
static uint32_t inword(const spec_t* s, int k)
{
    return ((s->tw + 2 * s->bx[k]) << 24) | ((s->th + 2 * s->by[k]) << 16) | (s->bx[k] << 11) | (s->by[k] << 6) |
           s->bpp;
}
static uint32_t in_first_ea(const spec_t* s, int k, uint32_t r, uint32_t c)
{
    uint32_t pitch = s->pitch, bpp = s->bpp, b0 = s->tw + 2 * s->bx[k], b1 = s->th + 2 * s->by[k];
    return SURFEA(s->surf[k]) + s->y0 * pitch + s->x0 * bpp + (b1 - 2 * s->by[k]) * r * pitch - s->by[k] * pitch +
           ((b0 - 2 * s->bx[k]) * c - s->bx[k]) * bpp;
}

/* mirror of BuildPostStepEntry (s.3): R0 [R1] {R,U}k [R6 U6] [R7] [R8] U0 [U1] [U7] [U8] T4 T3 RUN pad D */
static void build_batch(kz3_gpost_build bld, int i, const kz3_gpost_row* r)
{
    fb_t* f = &g_b[i];
    memset(f, 0, sizeof *f);
    make_spec(i, r, &f->sp);
    const spec_t* s = &f->sp;
    f->seq = r->seq;
    f->job = r->job;
    f->ntile = s->cols * s->rows;
    f->B = BAT0 + 0x20u * (uint32_t)i;
    f->ncmd = ncmd_of(s);
    f->size = ((8u * (uint32_t)f->ncmd + 15) & ~15u) + 0x30;
    f->ent = alloc(8 * f->ntile, 16);
    for (uint32_t t = 0; t < f->ntile; t++) f->cl[t] = alloc(f->size, 16);
    /* shared region: param list, then per tile input/rmw lists */
    f->nel_p = (uint32_t)((s->np + 1) & ~1);
    if (s->np) {
        f->plist = alloc(8 * f->nel_p, 16);
        for (int j = 0; j < s->np; j++) {
            g_mem[f->plist / 4 + 2 * (uint32_t)j] = s->psz[j];
            g_mem[f->plist / 4 + 2 * (uint32_t)j + 1] = s->pea[j];
        }
    }
    g_mem[(f->B + KZ3_GPOST_B_COUNT) / 4] = f->ntile;
    g_mem[(f->B + KZ3_GPOST_B_ENTRIES) / 4] = f->ent;
    g_mem[(f->B + KZ3_GPOST_B_MASK) / 4] = r->mask;
    g_mem[(f->B + KZ3_GPOST_B_EXCL) / 4] = r->excl;
    g_mem[(f->B + KZ3_GPOST_B_CTX) / 4] = 0;
    for (uint32_t t = 0; t < f->ntile; t++) {
        uint32_t row = t / s->cols, col = t % s->cols;
        for (int k = 0; k < s->nin; k++) {
            uint32_t rows_k = s->th + 2 * s->by[k], nel = (rows_k + 1) & ~1u;
            f->inl[t][k] = alloc(8 * nel, 16);
            for (uint32_t e = 0; e < rows_k; e++) {
                g_mem[f->inl[t][k] / 4 + 2 * e] = (s->tw + 2 * s->bx[k]) * s->bpp;
                g_mem[f->inl[t][k] / 4 + 2 * e + 1] = in_first_ea(s, k, row, col) + e * s->pitch;
            }
        }
        uint32_t dst =
            SURFEA(s->dsurf) + s->y0 * s->pitch + s->x0 * s->bpp + row * s->th * s->pitch + col * s->tw * s->bpp;
        if (s->rmw) {
            uint32_t nel = (s->th + 1) & ~1u;
            f->rmwl[t] = alloc(8 * nel, 16);
            for (uint32_t e = 0; e < s->th; e++) {
                g_mem[f->rmwl[t] / 4 + 2 * e] = s->tw * s->bpp;
                g_mem[f->rmwl[t] / 4 + 2 * e + 1] = dst + e * s->pitch;
            }
        }
        /* commands */
        uint32_t ea = f->cl[t], hi, lo;
        int n = 0;
        memset(&g_mem[ea / 4], 0, f->size);
        lo = cmdR(0, 1, 0x10, 8, &hi);
        put64(ea + 8u * (uint32_t)n++, hi, lo);
        if (s->mode) {
            lo = cmdR(1, 2, 0, 4, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        for (int k = 0; k < s->nin; k++) {
            lo = cmdR(2 + k, 2, 0, 4, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
            lo = cmdU(8 * (((s->th + 2 * s->by[k]) + 1) & ~1u), 1, 1, 0, 2 + k, f->inl[t][k], &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        if (s->rmw) {
            lo = cmdR(6, 2, 0, 4, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
            lo = cmdU(8 * ((s->th + 1) & ~1u), 1, 1, 0, 6, f->rmwl[t], &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        if (s->np) {
            lo = cmdR(7, 1, 0, 2, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        if (s->extra) {
            lo = cmdR(8, 1, 0, 4, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        lo = cmdU(0x2000, 1, 0, 2, 0, modva(bld, r->job), &hi);
        put64(ea + 8u * (uint32_t)n++, hi, lo);
        if (s->mode) {
            lo = cmdU(0, 0, 0, 0, 1, 0, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        if (s->np) {
            lo = cmdU(8 * f->nel_p, 1, 1, 2, 7, f->plist, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        if (s->extra) {
            lo = cmdU(0, 0, 0, 0, 8, 0, &hi);
            put64(ea + 8u * (uint32_t)n++, hi, lo);
        }
        put64(ea + 8u * (uint32_t)n++, 0x800, f->B);
        put64(ea + 8u * (uint32_t)n++, 0x600, 0x1FFu & ((1u << 9) - 1));
        put64(ea + 8u * (uint32_t)n++, 0xA00, 0);
        CHECK(n == f->ncmd, "fixture: command count %d vs formula %d (seq %d job %d)", n, f->ncmd, r->seq, r->job);
        /* D */
        uint32_t d = ea + f->size - 0x30;
        for (int k = 0; k < s->nin; k++) g_mem[d / 4 + (uint32_t)k] = inword(s, k);
        g_mem[d / 4 + 4] = dst;
        g_mem[d / 4 + 5] = (s->tw << 16) | s->th;
        uint32_t fl =
            (col == 0 ? 1u : 0) | (col == s->cols - 1 ? 2u : 0) | (row == 0 ? 4u : 0) | (row == s->rows - 1 ? 8u : 0);
        if (s->mode) fl |= 0x10;
        for (int k = 0; k < s->nin; k++) fl |= 0x20u << k;
        if (s->np) fl |= 0x200;
        if (s->extra) fl |= 0x400;
        if (s->rmw) fl |= s->mode == 2 ? 0x800 : 0x1000;
        fl |= s->hiflags;
        g_mem[d / 4 + 6] = (s->pitch << 16) | fl;
        g_mem[d / 4 + 7] = (col << 24) | (s->cols << 16) | (row << 8) | s->rows;
        g_mem[d / 4 + 8] = s->bpp << 24;
        put64(f->ent + 8 * t, (f->size << 16) | 1u, ea);
    }
}

static uint32_t g_frame_no;
static void build_frame(kz3_gpost_build bld, uint32_t S, uint32_t frame)
{
    memset(g_mem, 0, sizeof g_mem);
    g_cur = CUR0;
    g_nb = kz3_gpost_nrows;
    g_mem[(ITEM + KZ3_GPOST_ITEM_S) / 4] = S;
    g_mem[(REND + KZ3_GPOST_R_COUNT) / 4] = (uint32_t)g_nb;
    g_mem[(REND + KZ3_GPOST_R_LIST) / 4] = LIST;
    g_mem[(REND + KZ3_GPOST_R_FRAME) / 4] = frame;
    g_frame_no = frame;
    for (int i = 0; i < KZ3_GPOST_NSURF; i++) {
        uint32_t a = REND + KZ3_GPOST_SURF_BASE + (uint32_t)i * KZ3_GPOST_SURF_STRIDE;
        uint32_t w = i == 9 ? 160 : 1280, h = i == 9 ? 90 : 720;
        g_mem[a / 4 + 0] = SURFEA(i);
        g_mem[a / 4 + 1] = w;
        g_mem[a / 4 + 2] = h;
        g_mem[a / 4 + 3] = w * 4;
        g_mem[a / 4 + 4] = (h + 7) & ~7u;
        g_mem[a / 4 + 5] = 0x8A;
        g_mem[a / 4 + 6] = 4;
    }
    for (int i = 0; i < g_nb; i++) {
        g_mem[(LIST + 4u * (uint32_t)i) / 4] = BAT0 + 0x20u * (uint32_t)i;
        build_batch(bld, i, &kz3_gpost_rows[i]);
        /* parameter source blocks: a distinctive pattern per batch/word */
        for (uint32_t w = 0; w < 0x1000 / 4; w++)
            g_mem[(PSRC + 0x1000u * (uint32_t)i) / 4 + w] = 0xB0000000u | ((uint32_t)i << 16) | w;
    }
}

static int row_of(int seq, int nth)
{
    for (int i = 0; i < kz3_gpost_nrows; i++)
        if (kz3_gpost_rows[i].seq == seq && nth-- == 0) return i;
    return -1;
}
static uint64_t bit(int seq)
{
    return 1ull << seq;
}

static kz3_gpost_plan g_plan;
static kz3_gpost_saved g_sv;
static const uint32_t S_K4 = 0x1019F | 0x100;

static void run(kz3_gpost_build b, uint64_t want)
{
    kz3_gpost_begin(&M, b, ITEM, REND, want, &g_plan, &g_sv);
    kz3_gpost_end(&M, &g_plan, &g_sv);
}

/* the comparator: returns the number of fields of a parsed step that differ from the fixture spec */
static int step_diff(const kz3_gpost_plan* p, int bi)
{
    const kz3_gpost_pstep* st = kz3_gpost_p2_step(p, (uint32_t)bi);
    const fb_t* f = &g_b[bi];
    const spec_t* s = &f->sp;
    int bad = 0;
    if (!st || !st->ok) return 100;
    bad += st->job != f->job;
    bad += st->nin != s->nin || st->rmw != s->rmw || st->extra != s->extra;
    bad += st->nparam != s->np;
    bad += st->ntiles != f->ntile || st->entry_size != f->size;
    bad += st->tw != s->tw || st->th != s->th || st->cols != s->cols || st->rows != s->rows || st->pitch != s->pitch ||
           st->bpp != s->bpp;
    bad += st->dst_ea != SURFEA(s->dsurf) + s->y0 * s->pitch + s->x0 * s->bpp;
    bad += st->brec != f->B;
    bad += st->module_ea != modva((kz3_gpost_build)p->build, f->job);
    for (int k = 0; k < s->nin; k++) {
        bad += st->inw[k] != inword(s, k);
        bad += st->in[k].first_ea != in_first_ea(s, k, 0, 0);
        bad += st->in[k].first_size != (s->tw + 2 * s->bx[k]) * s->bpp;
        bad += st->in[k].surf != s->surf[k];
    }
    for (int j = 0; j < s->np; j++) {
        bad += st->pe[j].size != s->psz[j] || st->pe[j].ea != s->pea[j];
        if (j == 0) {
            const uint32_t* w = kz3_gpost_p2_block(p, &st->pe[0]);
            if (!w) {
                bad++;
                continue;
            }
            for (uint32_t o = 0; o < s->psz[0] / 4; o++) bad += w[o] != g_mem[s->pea[0] / 4 + o];
        } else
            bad += st->pe[j].copied != 0;
    }
    return bad;
}

static void test_clean(kz3_gpost_build b, const char* tag)
{
    build_frame(b, S_K4, 0x500);
    run(b, KZ3_GPOST_VALID_STEPS);
    const kz3_gpost_plan* p = kz3_gpost_plan_get(0x500);
    CHECK(p != NULL, "%s: plan published", tag);
    if (!p) return;
    int expect = 0, bad = 0;
    for (int i = 0; i < g_nb; i++) {
        const kz3_gpost_row* r = &kz3_gpost_rows[i];
        if (!(((r->mask & S_K4) != 0) && ((r->excl & S_K4) == 0))) continue;
        expect++;
        int d = step_diff(p, i);
        if (d) {
            bad++;
            printf("  %s: batch %d seq %d job %d: %d fields differ", tag, i, r->seq, r->job, d);
            const kz3_gpost_pstep* st = kz3_gpost_p2_step(p, (uint32_t)i);
            if (st) printf(" rej=%s", kz3_gpost_rej_name((kz3_gpost_rej)st->rej));
            printf("\n");
        }
    }
    CHECK(p->p2.nstep == (uint32_t)expect && p->p2.nok == (uint32_t)expect && p->p2.nrej == 0,
          "%s: steps %u ok %u rej %u, expected %d clean", tag, p->p2.nstep, p->p2.nok, p->p2.nrej, expect);
    CHECK(bad == 0, "%s: %d steps differ from the fixture", tag, bad);
    CHECK(p->p2.frame == 0x500 && p->p2.surf_valid && p->p2.surf[9][0] == SURFEA(9) && p->p2.surf[9][3] == 640,
          "%s: surface table", tag);
    /* the sequence-table jobs are now exact: every claimed batch carries its job id from U0 */
    int ids = 0;
    for (int i = 0; i < g_nb; i++)
        if (p->b[i].claimed && p->b[i].job == kz3_gpost_rows[i].job) ids++;
    CHECK(ids == expect, "%s: exact job ids from U0 on %d of %d claimed batches", tag, ids, expect);
    /* named cases of s.11 */
    static const int seqs[] = {6, 13, 15, 17, 31, 35};
    for (unsigned q = 0; q < sizeof seqs / sizeof seqs[0]; q++) {
        int i = row_of(seqs[q], 0);
        const kz3_gpost_pstep* st = kz3_gpost_p2_step(p, (uint32_t)i);
        CHECK(st && st->ok && step_diff(p, i) == 0, "%s: S%d parses", tag, seqs[q]);
    }
    const kz3_gpost_pstep* s31 = kz3_gpost_p2_step(p, (uint32_t)row_of(31, 0));
    const kz3_gpost_pstep* s15 = kz3_gpost_p2_step(p, (uint32_t)row_of(15, 0));
    CHECK(s31 && s31->job == 30 && s31->nin == 0 && s31->nparam == 5 && s31->pe[0].size == 0x20 &&
              s31->pe[0].copied == 0x20 && s31->ilrs_chunks == 1 && s31->pe[1].ea == SURFEA(9) &&
              s31->pe[4].ea == SURFEA(9) + 0xC000 && s31->pe[4].size == 0x2100 && s31->rmw == 1 && s31->mode == 3,
          "%s: S31 ILRSharp: 5 records, 0x20 block copied, 4 idx9 chunks (ilrs_chunks=%d)", tag,
          s31 ? s31->ilrs_chunks : -9);
    const kz3_gpost_pstep* s17 = kz3_gpost_p2_step(p, (uint32_t)row_of(17, 0));
    CHECK(s17 && s17->nin == 2 && s17->pe[0].size == 0x220 && s17->pe[0].copied == 0x220,
          "%s: S17 BloomCapture 0x220 block + 2 inputs", tag);
    const kz3_gpost_pstep* s35 = kz3_gpost_p2_step(p, (uint32_t)row_of(35, 0));
    CHECK(s35 && s35->nin == 2 && s35->pe[0].size == 0x210, "%s: S35 GrainWeight 0x210 block", tag);
    const kz3_gpost_pstep* s13 = kz3_gpost_p2_step(p, (uint32_t)row_of(13, 0));
    CHECK(s13 && s13->extra == 1 && s13->nin == 2 && s13->inw[0] >> 24 == 160 + 16,
          "%s: S13 MotionBlur extra scratch, item0 border 8", tag);
    CHECK(s15 && s15->extra == 1 && s15->rmw == 1 && s15->mode == 2 && s15->rmwl.first_ea == s15->dst_ea,
          "%s: S15 DoF RMW list, extra", tag);
    const kz3_gpost_pstep* s6 = kz3_gpost_p2_step(p, (uint32_t)row_of(6, 0));
    CHECK(s6 && s6->nparam == 1 && s6->pe[0].size == 0x10 && s6->plist_bytes == 16,
          "%s: S6 one 0x10 block, list padded to two elements", tag);
    /* blob accounting: copied bytes only */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < p->p2.nstep; i++)
        for (int j = 0; j < p->p2.s[i].nparam; j++) sum += p->p2.s[i].pe[j].copied;
    CHECK(sum == p->p2.blob_used && sum <= KZ3_GPOST_P2_BLOB, "%s: blob_used %u == copied %u", tag, p->p2.blob_used,
          sum);
    /* split-tile case: a ragged one-column grid and a 1x1 grid parse, with the 1x1 tile carrying all four edge flags */
    char line[600];
    kz3_gpost_format_p2(p, line, sizeof line);
    CHECK(strstr(line, "rej=0") != NULL, "%s: log line '%s'", tag, line);
    for (uint32_t q = 0; q < p->p2.nstep; q++)
        if (p->p2.s[q].batch == (uint32_t)row_of(31, 0)) kz3_gpost_format_p2_step(p, q, line, sizeof line);
    CHECK(strstr(line, "job=30") && strstr(line, "ok=1"), "%s: step line '%s'", tag, line);
    /* negative controls for the comparator itself */
    int i35 = row_of(35, 0);
    uint32_t save = g_mem[g_b[i35].sp.pea[0] / 4 + 3];
    g_mem[g_b[i35].sp.pea[0] / 4 + 3] ^= 0x1; /* the source block changes after the snapshot (stale copy) */
    NEG("clean: a stale parameter copy is detected", step_diff(p, i35) != 0);
    g_mem[g_b[i35].sp.pea[0] / 4 + 3] = save;
    kz3_gpost_plan tmp = *p;
    tmp.p2.blob_used = 0;
    NEG("clean: a truncated blob is detected", step_diff(&tmp, i35) != 0);
}

/* ---- the unset / byte-identical proof ---- */
static uint32_t g_snap[MEMW], g_base[16384], g_runl[16384];
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
        for (uint32_t e = 0; e < cnt && k + 2 <= cap; e++) {
            out[k++] = g_mem[(en + 8 * e) / 4];
            out[k++] = g_mem[(en + 8 * e + 4) / 4];
        }
        if (k + 2 <= cap) {
            out[k++] = 2;
            out[k++] = b;
        }
    }
    return k;
}

static void test_unset(kz3_gpost_build b, const char* tag)
{
    build_frame(b, S_K4, 0x520);
    memcpy(g_snap, g_mem, sizeof g_mem);
    int nbase = run_model(g_base, 16384);
    g_writes = 0;
    g_reads = 0;
    kz3_gpost_begin(&M, b, ITEM, REND, 0, &g_plan, &g_sv); /* KZ3_GPU_POST_STEPS unset: want = 0 */
    const unsigned reads_begin = g_reads;
    int nrun = run_model(g_runl, 16384);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    const unsigned reads_end = g_reads - reads_begin;
    CHECK(g_writes == 0, "%s: unset wrote %u words", tag, g_writes);
    CHECK(memcmp(g_snap, g_mem, sizeof g_mem) == 0, "%s: guest memory changed with nothing claimed", tag);
    CHECK(nrun == nbase && memcmp(g_base, g_runl, 4u * (unsigned)nbase) == 0,
          "%s: job list changed with nothing claimed", tag);
    const kz3_gpost_plan* p = kz3_gpost_plan_get(0x520);
    CHECK(p && p->p2.nstep == 0 && p->p2.blob_used == 0 && !p->p2.surf_valid && p->p2.frame == 0x520,
          "%s: no P2 work with nothing claimed", tag);
    /* counting accessor. With nothing claimed P2 reads no guest word (end() reads 0). P1's identify() DOES read guest memory
     * (item S, renderer count/list, every batch header): the wrapper (kz3_ppu_overrides.cpp kz3_gpost_body) skips begin()/end()
     * altogether only when KZ3_GPU_POST_STEPS and KZ3_GPU_POST_LOG are both unset; with only the log switch set the P1 reads
     * happen. That wrapper gate is code, not exercised by this host test. */
    CHECK(reads_end == 0, "%s: end() with nothing claimed read %u guest words", tag, reads_end);
    CHECK(reads_begin > 0, "%s: begin() (P1 identify) is expected to read guest memory; it read %u", tag, reads_begin);
    /* claimed: memory identical after end, only the zero + restore writes happened */
    build_frame(b, S_K4, 0x521);
    memcpy(g_snap, g_mem, sizeof g_mem);
    g_writes = 0;
    run(b, bit(31) | bit(35) | bit(6));
    CHECK(g_writes == 2 * g_plan.nclaimed && g_plan.nclaimed == 3,
          "%s: claimed frame wrote %u words for %u batches (zero+restore only)", tag, g_writes, g_plan.nclaimed);
    CHECK(memcmp(g_snap, g_mem, sizeof g_mem) == 0,
          "%s: guest memory differs after a claimed frame (P2 must only read)", tag);
    CHECK(g_plan.p2.nstep == 3 && g_plan.p2.nok == 3, "%s: three steps snapshotted", tag);
    /* negative controls: a write is noticed, a changed list is noticed */
    g_mem[(PSRC + 0x1000) / 4] ^= 1;
    NEG("unset: a one-word guest change is detected by the memory compare", memcmp(g_snap, g_mem, sizeof g_mem) != 0);
    g_mem[(PSRC + 0x1000) / 4] ^= 1;
    build_frame(b, S_K4, 0x522);
    int nb0 = run_model(g_base, 16384);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(36), &g_plan, &g_sv);
    int nb1 = run_model(g_runl, 16384);
    NEG("unset: a claim does change the job list (the identity check can fail)", nb1 != nb0);
    g_reads = 0;
    kz3_gpost_end(&M, &g_plan, &g_sv);
    NEG("unset: the read counter sees P2's reads when something is claimed", g_reads > 0);
}

/* ---- negative controls: one corruption per rejection, parsed over a rebuilt frame ---- */
static uint32_t cmd_at(int bi, uint32_t tile, uint32_t type, int idx, int* found)
{
    const fb_t* f = &g_b[bi];
    for (int c = 0; c < f->ncmd; c++) {
        uint32_t a = f->cl[tile] + 8u * (uint32_t)c, h = g_mem[a / 4];
        if (((h >> 9) & 7) == type && (type != 2 || (int)((h >> 5) & 0xF) == idx)) {
            *found = 1;
            return a;
        }
    }
    *found = 0;
    return 0;
}

typedef void (*mutate_fn)(kz3_gpost_build, int bi);
static void reject_case(kz3_gpost_build b, const char* tag, const char* name, int seq, int nth, mutate_fn mut,
                        kz3_gpost_rej want)
{
    build_frame(b, S_K4, 0x530);
    int i = row_of(seq, nth);
    mut(b, i);
    memcpy(g_snap, g_mem, sizeof g_mem);
    g_writes = 0;
    run(b, bit(seq));
    const kz3_gpost_pstep* st = kz3_gpost_p2_step(&g_plan, (uint32_t)i);
    char nm[160];
    snprintf(nm, sizeof nm, "%s %s: S%d rejected as %s", tag, name, seq, kz3_gpost_rej_name(want));
    int fired = st && !st->ok && st->rej == want && g_plan.p2.nrej >= 1 && g_plan.p2.rej[want] >= 1 &&
                g_plan.p2.nok + g_plan.p2.nrej == g_plan.p2.nstep;
    if (st && !fired) printf("  (got ok=%d rej=%s)\n", st->ok, kz3_gpost_rej_name((kz3_gpost_rej)st->rej));
    NEG(nm, fired);
    /* a rejected step holds no blob bytes and the guest is untouched apart from the restored masks */
    uint32_t held = 0;
    for (uint32_t k = 0; k < g_plan.p2.nstep; k++)
        if (g_plan.p2.s[k].ok)
            for (int j = 0; j < g_plan.p2.s[k].nparam; j++) held += g_plan.p2.s[k].pe[j].copied;
    CHECK(held == g_plan.p2.blob_used, "%s %s: rejected step leaves blob bytes behind (%u vs %u)", tag, name, held,
          g_plan.p2.blob_used);
    CHECK(memcmp(g_snap, g_mem, sizeof g_mem) == 0, "%s %s: guest memory differs after a rejected step", tag, name);
    CHECK(g_writes == 2 * g_plan.nclaimed, "%s %s: wrote %u words", tag, name, g_writes);
}

static void m_size_up(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] += 0x10u << 16;
} /* tile 0's entry claims 0x10 more bytes */
static void m_size_dn(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] -= 0x10u << 16;
}
static void m_trunc80(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] = (0x80u << 16) | 1u;
}
static void m_size_odd(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] += 4u << 16;
}
static void m_type(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] = (g_mem[g_b[i].ent / 4] & ~0xFFFFu) | 2u;
}
static void m_nibble(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent) / 4] |= 0x1u << 28;
}
static void m_u0_type(kz3_gpost_build b, int i)
{
    int f;
    uint32_t a = cmd_at(i, 0, 2, 0, &f);
    if (f) g_mem[a / 4] = (g_mem[a / 4] & ~(7u << 9)) | (6u << 9);
} /* type bits flipped: U0 is no command */
static void m_u0_ea(kz3_gpost_build b, int i)
{
    int f;
    uint32_t a = cmd_at(i, 0, 2, 0, &f);
    if (f) g_mem[a / 4 + 1] ^= 0x00010000u;
    (void)b;
} /* module EA not in the table */
static void m_u0_gone(kz3_gpost_build b, int i)
{
    int f;
    uint32_t a = cmd_at(i, 0, 2, 0, &f);
    if (f) g_mem[a / 4] = (g_mem[a / 4] & ~(0xFu << 5)) | (3u << 5);
    (void)b;
} /* module GET retargeted to buffer 3 */
static void m_job_other(kz3_gpost_build b, int i)
{
    int f;
    uint32_t a = cmd_at(i, 0, 2, 0, &f);
    if (f) g_mem[a / 4 + 1] = modva(b, 31);
} /* a valid module, but not this row's */
static void m_d_zero(kz3_gpost_build b, int i)
{
    (void)b;
    uint32_t d = g_b[i].cl[0] + g_b[i].size - 0x30;
    for (int k = 0; k < 12; k++) g_mem[d / 4 + (uint32_t)k] = 0;
}
static void m_d_swap(kz3_gpost_build b, int i)
{
    (void)
        b; /* D read at `size` instead of `size-0x30`: simulated by moving the real D 0x30 bytes later and zeroing its place */
    uint32_t d = g_b[i].cl[0] + g_b[i].size - 0x30;
    for (int k = 0; k < 12; k++) {
        g_mem[(d + 0x30) / 4 + (uint32_t)k] = g_mem[d / 4 + (uint32_t)k];
        g_mem[d / 4 + (uint32_t)k] = 0xA5A5A5A5u;
    }
}
static void m_d_flags(kz3_gpost_build b, int i)
{
    (void)b;
    uint32_t d = g_b[i].cl[0] + g_b[i].size - 0x30;
    g_mem[d / 4 + 6] ^= 0x200;
} /* params flag vs commands */
static void m_d_tilew(kz3_gpost_build b, int i)
{
    (void)b;
    uint32_t d = g_b[i].cl[0] + g_b[i].size - 0x30;
    g_mem[d / 4 + 5] &= 0x0000FFFFu;
}
static void m_plist_ea(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 7, &f);
    if (f) g_mem[a / 4 + 1] = 0xD0000000u;
}
static void m_plist_ea0(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 7, &f);
    if (f) g_mem[a / 4 + 1] = 0x100;
}
static void m_plist_trunc(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 7, &f);
    if (f) g_mem[a / 4] -= 1u << 18;
} /* drops the pad and the last chunk */
static void m_pad_drop(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 2 * (g_b[i].sp.np)] = 0x10;
    g_mem[g_b[i].plist / 4 + 2 * (g_b[i].sp.np) + 1] = 0x180000;
} /* pad element made a real, undersized record */
static void m_pblk_ea(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 1] = 0xF0000000u;
}
static void m_pblk_ea0(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 1] = 0;
}
static void m_pblk_ealo(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 1] = 0x8000;
}
static void m_pblk_size(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4] = 0x30;
} /* wrong size for the job */
static void m_pblk_size0(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4] = 0;
}
static void m_pblk_big(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4] = 0x40000;
}
static void m_chunk_unal(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 3] += 4;
} /* ILRSharp chunk EA unaligned, after block 0 was copied */
static void m_pblk_unal(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].plist / 4 + 1] += 2;
}
static void m_inl_ea(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 2, &f);
    if (f) g_mem[a / 4 + 1] = 0xE0000000u;
}
static void m_inl_size(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].inl[0][0] / 4] += 16;
} /* element size != byte0*bpp */
static void m_inl_count(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 2, &f);
    if (f) g_mem[a / 4] -= 1u << 18;
}
static void m_inl_first_ea0(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].inl[0][0] / 4 + 1] = 0;
} /* even row count: no pad element, this is the first element's EA */
static void m_rmw_first(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[g_b[i].rmwl[0] / 4 + 1] += 16;
}
static void m_rmw_ea(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 2, 6, &f);
    if (f) g_mem[a / 4 + 1] = 0xE0000000u;
}
static void m_tile_dst(kz3_gpost_build b, int i)
{
    (void)b;
    uint32_t d = g_b[i].cl[5] + g_b[i].size - 0x30;
    g_mem[d / 4 + 4] += 16;
}
static void m_tile_count(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].B + KZ3_GPOST_B_COUNT) / 4] -= 1;
}
static void m_tile_order(kz3_gpost_build b, int i)
{
    (void)b;
    uint32_t t = g_mem[g_b[i].ent / 4 + 2];
    g_mem[g_b[i].ent / 4 + 2] = g_mem[g_b[i].ent / 4 + 4];
    g_mem[g_b[i].ent / 4 + 4] = t;
    uint32_t u = g_mem[g_b[i].ent / 4 + 3];
    g_mem[g_b[i].ent / 4 + 3] = g_mem[g_b[i].ent / 4 + 5];
    g_mem[g_b[i].ent / 4 + 5] = u;
}
static void m_tile_size(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent + 8 * 3) / 4] += 0x10u << 16;
}
static void m_cl_ea(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent + 4) / 4] = 0xD0000000u;
}
static void m_cl_ea0(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent + 4) / 4] = 0;
}
static void m_cl_unal(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].ent + 4) / 4] += 8;
}
static void m_brec(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 4, 0, &f);
    if (f) g_mem[a / 4 + 1] = g_b[i].B + 0x20;
} /* stale record: names another batch */
static void m_run_gone(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 5, 0, &f);
    if (f) g_mem[a / 4] = 0;
}
static void m_ent_none(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].B + KZ3_GPOST_B_ENTRIES) / 4] = 0;
}
static void m_toomany(kz3_gpost_build b, int i)
{
    (void)b;
    g_mem[(g_b[i].B + KZ3_GPOST_B_COUNT) / 4] = KZ3_GPOST_P2_MAXTILE + 1;
}
static void m_dup_cmd(kz3_gpost_build b, int i)
{
    (void)b;
    int f;
    uint32_t a = cmd_at(i, 0, 3, 0, &f);
    if (f) g_mem[a / 4] = 0x800;
} /* T3 replaced by a second T4 */

static void test_rejects(kz3_gpost_build b, const char* tag)
{
    reject_case(b, tag, "wrong size (+0x10)", 6, 0, m_size_up, KZ3_GP_REJ_SIZE);
    reject_case(b, tag, "wrong size (-0x10)", 6, 0, m_size_dn, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "truncated at 0x80", 17, 0, m_trunc80, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "size not a multiple of 16", 35, 0, m_size_odd, KZ3_GP_REJ_SIZE);
    reject_case(b, tag, "entry type 2", 6, 0, m_type, KZ3_GP_REJ_TYPE);
    reject_case(b, tag, "flag nibble set", 6, 0, m_nibble, KZ3_GP_REJ_TYPE);
    reject_case(b, tag, "U0 type bits flipped", 6, 0, m_u0_type, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "U0 module EA unknown", 31, 0, m_u0_ea, KZ3_GP_REJ_MODULE);
    reject_case(b, tag, "U0 retargeted", 31, 0, m_u0_gone, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "another job's module", 15, 0, m_job_other, KZ3_GP_REJ_JOB);
    reject_case(b, tag, "D zeroed", 6, 0, m_d_zero, KZ3_GP_REJ_D);
    reject_case(b, tag, "D read at size instead of size-0x30", 17, 0, m_d_swap, KZ3_GP_REJ_D);
    reject_case(b, tag, "D flags vs commands", 35, 0, m_d_flags, KZ3_GP_REJ_D);
    reject_case(b, tag, "D tile width 0", 15, 0, m_d_tilew, KZ3_GP_REJ_D);
    reject_case(b, tag, "param list pointer out of range", 17, 0, m_plist_ea, KZ3_GP_REJ_PLIST);
    reject_case(b, tag, "param list pointer in the null page", 17, 0, m_plist_ea0, KZ3_GP_REJ_PLIST);
    reject_case(b, tag, "param list truncated (pad dropped)", 6, 0, m_plist_trunc, KZ3_GP_REJ_PLIST);
    reject_case(b, tag, "param pad slot made a real record", 6, 0, m_pad_drop, KZ3_GP_REJ_SHAPE);
    reject_case(b, tag, "param block pointer out of range", 17, 0, m_pblk_ea, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "param block pointer 0", 35, 0, m_pblk_ea0, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "param block pointer below main memory", 35, 0, m_pblk_ealo, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "param block wrong size", 17, 0, m_pblk_size, KZ3_GP_REJ_SHAPE);
    reject_case(b, tag, "param block size 0", 35, 0, m_pblk_size0, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "param block size 0x40000", 35, 0, m_pblk_big, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "param block unaligned pointer", 31, 0, m_pblk_unal, KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "ILRSharp chunk pointer unaligned (rolls the copied block back)", 31, 0, m_chunk_unal,
                KZ3_GP_REJ_PBLK);
    reject_case(b, tag, "input list pointer out of range", 13, 0, m_inl_ea, KZ3_GP_REJ_INLIST);
    reject_case(b, tag, "input list first size", 13, 0, m_inl_size, KZ3_GP_REJ_INLIST);
    reject_case(b, tag, "input list element count", 13, 0, m_inl_count, KZ3_GP_REJ_INLIST);
    reject_case(b, tag, "input list first element EA 0 (90 rows, no pad element)", 6, 0, m_inl_first_ea0,
                KZ3_GP_REJ_INLIST);
    reject_case(b, tag, "RMW list first element", 15, 0, m_rmw_first, KZ3_GP_REJ_RMW);
    reject_case(b, tag, "RMW list pointer out of range", 15, 0, m_rmw_ea, KZ3_GP_REJ_RMW);
    reject_case(b, tag, "tile destination arithmetic", 17, 0, m_tile_dst, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "entry count != cols*rows", 17, 0, m_tile_count, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "tile order swapped", 17, 0, m_tile_order, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "tile 3 wrong size", 6, 0, m_tile_size, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "too many tiles", 6, 0, m_toomany, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "no entries", 6, 0, m_ent_none, KZ3_GP_REJ_TILES);
    reject_case(b, tag, "command list pointer out of range", 6, 0, m_cl_ea, KZ3_GP_REJ_EA);
    reject_case(b, tag, "command list pointer 0", 6, 0, m_cl_ea0, KZ3_GP_REJ_EA);
    reject_case(b, tag, "command list pointer unaligned", 6, 0, m_cl_unal, KZ3_GP_REJ_EA);
    reject_case(b, tag, "completion record names another batch", 6, 0, m_brec, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "RUN missing", 6, 0, m_run_gone, KZ3_GP_REJ_CMD);
    reject_case(b, tag, "duplicate completion command", 6, 0, m_dup_cmd, KZ3_GP_REJ_CMD);
    /* ILRSharp chunk list shortened: still a valid step, but the chunk check against idx9 reports the mismatch */
    build_frame(b, S_K4, 0x532);
    m_plist_trunc(b, row_of(31, 0));
    run(b, bit(31));
    const kz3_gpost_pstep* sx = kz3_gpost_p2_step(&g_plan, (uint32_t)row_of(31, 0));
    NEG("ILRSharp: a shortened chunk list is reported by ilrs_chunks=0",
        sx && sx->ok && sx->nparam == 4 && sx->ilrs_chunks == 0);
    /* a rejection does not poison the other claimed steps */
    build_frame(b, S_K4, 0x531);
    m_pblk_ea(b, row_of(17, 0));
    run(b, bit(6) | bit(17) | bit(35));
    CHECK(g_plan.p2.nstep == 3 && g_plan.p2.nok == 2 && g_plan.p2.nrej == 1 && g_plan.p2.rej[KZ3_GP_REJ_PBLK] == 1,
          "%s: one bad step among three (ok %u rej %u)", tag, g_plan.p2.nok, g_plan.p2.nrej);
    CHECK(step_diff(&g_plan, row_of(6, 0)) == 0 && step_diff(&g_plan, row_of(35, 0)) == 0,
          "%s: the good neighbours are intact", tag);
    char line[600];
    kz3_gpost_format_p2(&g_plan, line, sizeof line);
    CHECK(strstr(line, "pblk=1") != NULL, "%s: reject counted in the log line '%s'", tag, line);
}

/* stale frame: a plan is a snapshot of ITS frame; the next frame's guest changes do not reach it, other frames get nothing,
 * and a ring slot reused 4 frames later holds only the new frame's P2 data */
static void test_stale(kz3_gpost_build b, const char* tag)
{
    build_frame(b, S_K4, 0x600);
    run(b, bit(35) | bit(6));
    const kz3_gpost_plan* p = kz3_gpost_plan_get(0x600);
    CHECK(p && p->p2.frame == 0x600 && p->p2.nok == 2, "%s: frame 0x600 snapshot", tag);
    if (!p) return;
    int i35 = row_of(35, 0);
    uint32_t w0 = p->p2.surf[9][0];
    uint32_t blk0 = kz3_gpost_p2_block(p, &p->p2.s[1].pe[0]) ? kz3_gpost_p2_block(p, &p->p2.s[1].pe[0])[0] : 0;
    /* next frame: the game rewrites the parameter source and the surface table */
    build_frame(b, S_K4, 0x601);
    g_mem[g_b[i35].sp.pea[0] / 4] = 0x13572468;
    g_mem[(REND + KZ3_GPOST_SURF_BASE + 9 * KZ3_GPOST_SURF_STRIDE) / 4] = 0x12345678;
    NEG("stale: the next frame's guest differs from frame 0x600's snapshot", g_mem[g_b[i35].sp.pea[0] / 4] != blk0);
    CHECK(kz3_gpost_plan_get(0x601) == NULL, "%s: no plan for 0x601 before its end()", tag);
    CHECK(p->p2.surf[9][0] == w0 && w0 == SURFEA(9), "%s: surface table of 0x600 unchanged by 0x601's guest", tag);
    const uint32_t* blk = kz3_gpost_p2_block(p, &p->p2.s[1].pe[0]);
    CHECK(blk && blk[0] == 0xB0000000u + ((uint32_t)i35 << 16), "%s: 0x600's block keeps its own value (0x%08X)", tag,
          blk ? blk[0] : 0);
    run(b, bit(35));
    const kz3_gpost_plan* q = kz3_gpost_plan_get(0x601);
    CHECK(q && q->p2.frame == 0x601 && q->p2.nstep == 1 && kz3_gpost_p2_block(q, &q->p2.s[0].pe[0])[0] == 0x13572468 &&
              q->p2.surf[9][0] == 0x12345678,
          "%s: 0x601 sees its own guest", tag);
    CHECK(kz3_gpost_plan_get(0x600) != NULL && kz3_gpost_plan_get(0x600)->p2.nstep == 2,
          "%s: 0x600 still there beside 0x601", tag);
    build_frame(b, S_K4, 0x604);
    run(b, bit(6));
    CHECK(kz3_gpost_plan_get(0x600) == NULL && kz3_gpost_plan_get(0x604) && kz3_gpost_plan_get(0x604)->p2.nstep == 1,
          "%s: slot reused by 0x604 holds only its P2 data", tag);
    NEG("stale: a frame that is not in the ring has no P2 data",
        kz3_gpost_plan_get(0x608) == NULL && kz3_gpost_plan_get(0x600) == NULL);
    /* a rejected frame is stale-safe too: p2 header resets between frames (no rej counts leak) */
    build_frame(b, S_K4, 0x610);
    m_pblk_ea0(b, row_of(35, 0));
    run(b, bit(35));
    CHECK(g_plan.p2.nrej == 1, "%s: bad frame counted", tag);
    build_frame(b, S_K4, 0x611);
    run(b, bit(35));
    CHECK(g_plan.p2.nrej == 0 && g_plan.p2.nok == 1 && g_plan.p2.rej[KZ3_GP_REJ_PBLK] == 0,
          "%s: counters reset on the next frame", tag);
}

/* grid / split-tile cases: the parser walks every tile, 1x1 has all four edge flags, ragged one-column and one-row grids parse */
static void test_grids(kz3_gpost_build b, const char* tag)
{
    static const uint32_t grids[][2] = {{1, 1}, {1, 5}, {5, 1}, {8, 8}, {3, 2}};
    for (unsigned g = 0; g < sizeof grids / sizeof grids[0]; g++) {
        build_frame(b, S_K4, 0x700 + g);
        int i = row_of(31, 0);
        g_ov_cols = grids[g][0];
        g_ov_rows = grids[g][1];
        g_cur = 0x100000;
        build_batch(b, i, &kz3_gpost_rows[i]);
        g_ov_cols = g_ov_rows = 0;
        run(b, bit(31));
        const kz3_gpost_pstep* st = kz3_gpost_p2_step(&g_plan, (uint32_t)i);
        CHECK(st && st->ok && st->ntiles == grids[g][0] * grids[g][1] && st->cols == grids[g][0] &&
                  st->rows == grids[g][1] && step_diff(&g_plan, i) == 0,
              "%s: grid %ux%u parses (ok=%d rej=%s)", tag, grids[g][0], grids[g][1], st ? st->ok : -1,
              st ? kz3_gpost_rej_name((kz3_gpost_rej)st->rej) : "-");
        if (g == 0) {
            uint32_t d = g_b[i].cl[0] + g_b[i].size - 0x30;
            CHECK((g_mem[d / 4 + 6] & 0xF) == 0xF && st && (st->flags & 0xF) == 0xF,
                  "%s: 1x1 tile has all four edge flags", tag);
        }
        /* negative control: the same grid with the last tile's column changed is rejected */
        build_frame(b, S_K4, 0x7F0);
        g_ov_cols = grids[g][0];
        g_ov_rows = grids[g][1];
        g_cur = 0x100000;
        build_batch(b, i, &kz3_gpost_rows[i]);
        g_ov_cols = g_ov_rows = 0;
        uint32_t last = grids[g][0] * grids[g][1] - 1;
        uint32_t dl = g_b[i].cl[last] + g_b[i].size - 0x30;
        g_mem[dl / 4 + 7] ^= (1u << 24); /* column field */
        run(b, bit(31));
        st = kz3_gpost_p2_step(&g_plan, (uint32_t)i);
        NEG("grid: a wrong last-tile column is rejected",
            st && !st->ok && (st->rej == KZ3_GP_REJ_TILES || st->rej == KZ3_GP_REJ_D));
    }
}

static const kz3_gpost_pstep* step_of(int i)
{
    return kz3_gpost_p2_step(&g_plan, (uint32_t)i);
}

/* ---- odd row count: an input list of an odd number of rows has a {0,0} pad element behind the last row ---- */
static void odd_build(kz3_gpost_build b, int i, uint32_t th)
{
    build_frame(b, S_K4, 0x540);
    g_ov_th = th;
    g_cur = 0x100000;
    build_batch(b, i, &kz3_gpost_rows[i]);
    g_ov_th = 0;
}

static void odd_case(kz3_gpost_build b, const char* tag, const char* name, uint32_t byte_off, uint32_t val)
{
    const int i = row_of(6, 0);
    odd_build(b, i, 91); /* 91 rows: pad element at row 91, list of 92 elements */
    g_mem[(g_b[i].inl[0][0] + byte_off) / 4] = val;
    run(b, bit(6));
    const kz3_gpost_pstep* st = step_of(i);
    char nm[200];
    snprintf(nm, sizeof nm, "%s odd rows: %s rejected as inlist", tag, name);
    NEG(nm, st && !st->ok && st->rej == KZ3_GP_REJ_INLIST);
}

static void test_odd_rows(kz3_gpost_build b, const char* tag)
{
    const int i = row_of(6, 0);
    odd_build(b, i, 91);
    run(b, bit(6));
    const kz3_gpost_pstep* st = step_of(i);
    CHECK(g_b[i].sp.th == 91, "%s: fixture has 91 rows", tag);
    CHECK(st && st->ok && st->in[0].nel == 92 && st->in[0].list_bytes == 92 * 8 && step_diff(&g_plan, i) == 0,
          "%s: an odd row count (91 rows + pad element) parses (ok=%d rej=%s)", tag, st ? st->ok : -1,
          st ? kz3_gpost_rej_name((kz3_gpost_rej)st->rej) : "-");
    odd_case(b, tag, "pad word 0 not zero", 8 * 91, 0x10);
    odd_case(b, tag, "pad word 1 not zero", 8 * 91 + 4, 0x1234);
    odd_case(b, tag, "pad element is a copy of the last row", 8 * 91,
             0x2A0); /* a plausible size: only the pad rule can refuse it */
    /* the 90-row (even) fixture has no pad element: its first element past the last row is not looked at */
    build_frame(b, S_K4, 0x541);
    g_mem[(g_b[i].inl[0][0] + 8 * 90) / 4] =
        0xFFFF; /* element 90 = the first slot past the 90 rows: nothing may read it as a pad */
    run(b, bit(6));
    st = step_of(i);
    CHECK(st && st->ok, "%s: an even row count has no pad element to check", tag);
}

/* ---- range checks at the main-memory edge (0xC0000000): a pointer that starts below it and a length that crosses it ---- */
enum { E_PBLK, E_CL, E_ENT, E_INL, E_PLIST, E_RMW, E_N };
static const char* const edge_name[E_N] = {"parameter block", "command list",   "entry array",
                                           "input list",      "parameter list", "RMW list"};
static const kz3_gpost_rej edge_rej[E_N] = {KZ3_GP_REJ_PBLK,   KZ3_GP_REJ_EA,    KZ3_GP_REJ_EA,
                                            KZ3_GP_REJ_INLIST, KZ3_GP_REJ_PLIST, KZ3_GP_REJ_RMW};
static const int edge_seq[E_N] = {6, 6, 6, 6, 31, 15};
static const uint32_t edge_over[E_N] = {
    4, 0x10, 8, 0x10, 0x10, 0x10}; /* how far past 0xC0000000 the crossing range ends (keeps the alignment) */

static void edge_site(int site, int i, uint32_t* len, uint32_t* src, uint32_t** field)
{
    const fb_t* f = &g_b[i];
    int found;
    uint32_t a;
    switch (site) {
    case E_PBLK:
        *len = f->sp.psz[0];
        *src = f->sp.pea[0];
        *field = &g_mem[f->plist / 4 + 1];
        break;
    case E_CL:
        *len = f->size;
        *src = f->cl[0];
        *field = &g_mem[f->ent / 4 + 1];
        break;
    case E_ENT:
        *len = 8 * f->ntile;
        *src = f->ent;
        *field = &g_mem[(f->B + KZ3_GPOST_B_ENTRIES) / 4];
        break;
    case E_INL:
        *len = 8 * ((f->sp.th + 2 * f->sp.by[0] + 1) & ~1u);
        *src = f->inl[0][0];
        a = cmd_at(i, 0, 2, 2, &found);
        *field = &g_mem[a / 4 + 1];
        break;
    case E_PLIST:
        *len = 8 * f->nel_p;
        *src = f->plist;
        a = cmd_at(i, 0, 2, 7, &found);
        *field = &g_mem[a / 4 + 1];
        break;
    default:
        *len = 8 * ((f->sp.th + 1) & ~1u);
        *src = f->rmwl[0];
        a = cmd_at(i, 0, 2, 6, &found);
        *field = &g_mem[a / 4 + 1];
        break;
    }
}

static void edge_case(kz3_gpost_build b, const char* tag, int site, int cross)
{
    build_frame(b, S_K4, 0x550);
    const int i = row_of(edge_seq[site], 0);
    uint32_t len, src, *field;
    edge_site(site, i, &len, &src, &field);
    const uint32_t ea = KZ3_GPOST_EA_HI - len + (cross ? edge_over[site] : 0);
    *field = ea;
    if (!cross) /* exact fit: the last byte of the range is the last byte below 0xC0000000 */
        for (uint32_t o = 0; o < len; o += 4) g_hi[(ea - HIWIN + o) / 4] = g_mem[(src + o) / 4];
    run(b, bit(edge_seq[site]));
    const kz3_gpost_pstep* st = step_of(i);
    char nm[200];
    snprintf(nm, sizeof nm, "%s %s: range ending %u bytes past 0xC0000000 rejected", tag, edge_name[site],
             edge_over[site]);
    if (cross)
        NEG(nm, st && !st->ok && st->rej == edge_rej[site]);
    else
        CHECK(st && st->ok, "%s %s: a range ending exactly at 0xC0000000 is accepted (ok=%d rej=%s)", tag,
              edge_name[site], st ? st->ok : -1, st ? kz3_gpost_rej_name((kz3_gpost_rej)st->rej) : "-");
    if (!cross && site == E_PBLK && st && st->ok) {
        const uint32_t* w = kz3_gpost_p2_block(&g_plan, &st->pe[0]);
        CHECK(w && memcmp(w, &g_mem[src / 4], len) == 0, "%s: the block at the edge was copied", tag);
    }
}

static void test_ea_edges(kz3_gpost_build b, const char* tag)
{
    for (int site = 0; site < E_N; site++) {
        edge_case(b, tag, site, 0);
        edge_case(b, tag, site, 1);
    }
    /* the low edge: 0x10000 is main memory, one word below is not */
    for (int k = 0; k < 2; k++) {
        build_frame(b, S_K4, 0x560);
        const int i = row_of(6, 0);
        g_mem[g_b[i].plist / 4 + 1] = k ? KZ3_GPOST_EA_LO - 4 : KZ3_GPOST_EA_LO;
        run(b, bit(6));
        const kz3_gpost_pstep* st = step_of(i);
        if (k)
            NEG("low edge: a block one word below 0x10000 is rejected", st && !st->ok && st->rej == KZ3_GP_REJ_PBLK);
        else
            CHECK(st && st->ok, "%s: a block at exactly 0x10000 is accepted", tag);
    }
}

/* ---- capacities: the parameter-block blob (4096 bytes) and the step table (64 steps) ---- */
/* claim `ncopy` copies of the batch of sequence `seq` in a frame: begin() claims it once, the plan's batch list is then
 * rewritten so that `ncopy` entries are claimed copies (the real table has 56 rows, so the 64-step cap cannot be reached by
 * a real frame; P2 must still hold at it). Leaves g_plan ready for kz3_gpost_end(). */
static int claim_copies(kz3_gpost_build b, int seq, uint32_t ncopy)
{
    build_frame(b, S_K4, 0x570);
    const int bi = row_of(seq, 0);
    kz3_gpost_begin(&M, b, ITEM, REND, bit(seq), &g_plan, &g_sv);
    const kz3_gpost_batch src = g_plan.b[bi];
    const uint32_t nb = ncopy > g_plan.nb ? ncopy : g_plan.nb;
    for (uint32_t k = 0; k < nb; k++) {
        g_plan.b[k] = src;
        g_plan.b[k].claimed = k < ncopy;
    }
    g_plan.nb = nb;
    g_plan.nclaimed = ncopy;
    return bi;
}

static void test_capacity(kz3_gpost_build b, const char* tag)
{
    /* blob: 20 BloomCapture steps of 0x220 bytes: 7 fit (3808 of 4096), the other 13 are rejected as blob and leave nothing behind */
    claim_copies(b, 17, 20);
    kz3_gpost_end(&M, &g_plan, &g_sv);
    const kz3_gpost_p2* q = &g_plan.p2;
    CHECK(q->nstep == 20 && q->nok == 7 && q->nrej == 13 && q->rej[KZ3_GP_REJ_BLOB] == 13 && q->blob_used == 7 * 0x220u,
          "%s: blob capacity: steps %u ok %u rej %u blob-rej %u used %u", tag, q->nstep, q->nok, q->nrej,
          q->rej[KZ3_GP_REJ_BLOB], q->blob_used);
    int blobrej = 0;
    for (uint32_t k = 0; k < q->nstep; k++) blobrej += !q->s[k].ok && q->s[k].rej == KZ3_GP_REJ_BLOB;
    CHECK(blobrej == 13 && q->blob_used <= KZ3_GPOST_P2_BLOB, "%s: the 13 rejected steps carry the blob reason (%d)",
          tag, blobrej);
    /* exact fit and one word over, through kz3_gpost_p2_parse */
    const int bi = claim_copies(b, 17, 1);
    kz3_gpost_pstep st;
    g_plan.p2.surf_valid = 0;
    g_plan.p2.blob_used = KZ3_GPOST_P2_BLOB - 0x220;
    kz3_gpost_rej r = kz3_gpost_p2_parse(&M, b, &g_plan, (uint32_t)bi, &st);
    CHECK(r == KZ3_GP_REJ_NONE && st.ok && g_plan.p2.blob_used == KZ3_GPOST_P2_BLOB,
          "%s: a block that fills the blob exactly is accepted (rej=%s used %u)", tag, kz3_gpost_rej_name(r),
          g_plan.p2.blob_used);
    g_plan.p2.blob_used = KZ3_GPOST_P2_BLOB - 0x220 + 4;
    r = kz3_gpost_p2_parse(&M, b, &g_plan, (uint32_t)bi, &st);
    NEG("capacity: a block that is 4 bytes too big is rejected as blob and the blob is left alone",
        r == KZ3_GP_REJ_BLOB && !st.ok && st.rej == KZ3_GP_REJ_BLOB &&
            g_plan.p2.blob_used == KZ3_GPOST_P2_BLOB - 0x220 + 4);
    /* steps: 64 fit, the 65th and later are counted as blob rejects and get no step entry (kz3_gpost_p2_step returns NULL) */
    static const uint32_t ncs[] = {63, 64, 65, 70};
    for (unsigned k = 0; k < sizeof ncs / sizeof ncs[0]; k++) {
        const uint32_t n = ncs[k], held = n > KZ3_GPOST_P2_MAXSTEP ? KZ3_GPOST_P2_MAXSTEP : n, over = n - held;
        claim_copies(b, 6, n);
        kz3_gpost_end(&M, &g_plan, &g_sv);
        const kz3_gpost_p2* p2 = &g_plan.p2;
        CHECK(p2->nstep == held && p2->nok == held && p2->nrej == over && p2->rej[KZ3_GP_REJ_BLOB] == over,
              "%s: %u claimed batches: steps %u ok %u rej %u (blob-rej %u), expected steps %u rej %u", tag, n,
              p2->nstep, p2->nok, p2->nrej, p2->rej[KZ3_GP_REJ_BLOB], held, over);
        CHECK(kz3_gpost_p2_step(&g_plan, held - 1) != NULL, "%s: %u claimed: the last held step is reachable", tag, n);
        if (over)
            NEG("capacity: a batch past the 64-step cap has no step entry (NULL)",
                kz3_gpost_p2_step(&g_plan, held) == NULL);
        else
            CHECK(kz3_gpost_p2_step(&g_plan, held) == NULL, "%s: nothing past the last claimed batch", tag);
    }
}

/* ---- 64-bit products: values from guest memory must not wrap in 32 bits ---- */
static void test_wrap(kz3_gpost_build b, const char* tag)
{
    /* tile destinations: row*th*pitch with th = pitch = 0x8000 is 2^30 per row; at row 4 a 32-bit sum wraps back onto tile 0's address */
    const int iz = row_of(28, 0); /* Zero: no lists, nothing but the tile arithmetic */
    for (uint32_t rows = 3; rows <= 5; rows += 2) {
        build_frame(b, S_K4, 0x580);
        g_ov_cols = 1;
        g_ov_rows = rows;
        g_ov_th = 0x8000;
        g_ov_pitch = 0x8000;
        g_ov_tw = 1;
        g_cur = 0x100000;
        build_batch(b, iz, &kz3_gpost_rows[iz]);
        g_ov_cols = g_ov_rows = g_ov_th = g_ov_pitch = g_ov_tw = 0;
        run(b, bit(28));
        const kz3_gpost_pstep* st = step_of(iz);
        if (rows == 3)
            CHECK(st && st->ok && st->ntiles == 3, "%s: 3 rows of 2^30 bytes parse (ok=%d rej=%s)", tag,
                  st ? st->ok : -1, st ? kz3_gpost_rej_name((kz3_gpost_rej)st->rej) : "-");
        else
            NEG("wrap: row 4's destination that wraps 2^32 back onto tile 0 is rejected",
                st && !st->ok && st->rej == KZ3_GP_REJ_TILES);
    }
    /* surface lookup: pitch*h = 2^32 is a real span, not 0 */
    for (int k = 0; k < 4; k++) {
        const int i6 =
            row_of(k == 3 ? 13 : 6, 0); /* the MotionBlur row has a border of 8: surf_of adds back by*pitch */
        build_frame(b, S_K4, 0x581);
        const int sj = g_b[i6].sp.surf[0];
        const uint32_t sa = REND + KZ3_GPOST_SURF_BASE + (uint32_t)sj * KZ3_GPOST_SURF_STRIDE;
        if (k == 1) {
            g_mem[(sa + 8) / 4] = 0x10000;
            g_mem[(sa + 12) / 4] = 0x10000;
        }
        if (k == 2) {
            g_mem[(sa + 8) / 4] = 1;
            g_mem[(sa + 12) / 4] = 640;
        }
        /* 8 * 0x20001400 = 2^32 + 8 * 5120: in 32 bits the border shift looks like the real one, in 64 bits it is far outside a 1-row span */
        if (k == 3) {
            g_mem[(sa + 8) / 4] = 1;
            g_mem[(sa + 12) / 4] = 0x20001400u;
        }
        run(b, bit(k == 3 ? 13 : 6));
        const kz3_gpost_pstep* st = step_of(i6);
        if (k < 2)
            CHECK(st && st->ok && st->in[0].surf == sj, "%s: surface %d found with %s span (surf=%d)", tag, sj,
                  k ? "a 2^32" : "the normal", st ? st->in[0].surf : -9);
        else if (k == 2)
            NEG("wrap: a surface whose span ends before the first element is not matched",
                st && st->ok && st->in[0].surf == -1);
        else
            NEG("wrap: a border shift of 8 * pitch that wraps 2^32 does not match the surface",
                st && st->ok && st->in[0].surf == -1);
    }
    /* ILRSharp chunk check: pitch*h of surface 9 = 57600 only modulo 2^32 (pitch 65537, h = 57600/65537 mod 2^32) */
    const int i31 = row_of(31, 0);
    for (int k = 0; k < 3; k++) {
        build_frame(b, S_K4, 0x582);
        const uint32_t s9 = REND + KZ3_GPOST_SURF_BASE + 9 * KZ3_GPOST_SURF_STRIDE;
        if (k == 0) {
            g_mem[(s9 + 12) / 4] = 65537u;
            g_mem[(s9 + 8) / 4] = 57600u * 0xFFFF0001u;
        }
        if (k ==
            2) { /* idx9 at 0xFFFF9000: chunk k at idx9 + k*0x4000 wraps past 2^32 at the third chunk; the guest lists the wrapped addresses */
            g_mem[s9 / 4] = 0xFFFF9000u;
            for (uint32_t j = 1; j < 5; j++) g_mem[g_b[i31].plist / 4 + 2 * j + 1] = 0xFFFF9000u + (j - 1) * 0x4000u;
        }
        run(b, bit(31));
        const kz3_gpost_pstep* st = step_of(i31);
        if (k == 0)
            NEG("wrap: ILRSharp chunks against a pitch*h that only equals 57600 modulo 2^32 are reported as not "
                "matching",
                st && st->ok && st->ilrs_chunks == 0);
        else if (k == 2)
            NEG("wrap: ILRSharp chunk addresses that wrap past 2^32 are reported as not matching",
                st && st->ok && st->ilrs_chunks == 0);
        else
            CHECK(st && st->ok && st->ilrs_chunks == 1, "%s: ILRSharp chunks against the real pitch*h match", tag);
    }
}

int main(void)
{
    printf("table rows %d\n", kz3_gpost_nrows);
    kz3_gpost_build bs[2] = {KZ3_GP_BUILD_114, KZ3_GP_BUILD_100};
    const char* tags[2] = {"1.14", "1.00"};
    for (int k = 0; k < 2; k++) {
        printf("== %s\n", tags[k]);
        test_clean(bs[k], tags[k]);
        test_unset(bs[k], tags[k]);
        test_rejects(bs[k], tags[k]);
        test_stale(bs[k], tags[k]);
        test_grids(bs[k], tags[k]);
        test_odd_rows(bs[k], tags[k]);
        test_ea_edges(bs[k], tags[k]);
        test_capacity(bs[k], tags[k]);
        test_wrap(bs[k], tags[k]);
    }
    printf(g_fail ? "FAIL %d\n" : "PASS\n", g_fail);
    return g_fail != 0;
}
