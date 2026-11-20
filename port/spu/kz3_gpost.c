/* kz3_gpost.c -- GPU post plan P1 core, see kz3_gpost.h. Pure C, no host memory access of its own. */
#include "kz3_gpost.h"
#include "kz3_gpost_modva.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Job ids (Gfx.Post.*, re/data/post_jobs.tsv). */
enum { J_BCAP = 22, J_BINT = 23, J_DOF = 24, J_FUZZ = 25, J_DOWN = 26, J_GRAIN = 27, J_HBLUR = 28, J_ILR = 29, J_ILRS = 30,
       J_MOD = 31, J_MB = 32, J_UP = 33, J_VBLUR = 34, J_VIG = 35, J_ZERO = 37, J_TRANSPOSE = 39 };

#define BL(seq, m, x) {seq, J_HBLUR, m, x, 0}, {seq, J_VBLUR, m, x, 0}
#define BL2(seq, m) BL(seq, m, 0), BL(seq, m, 0)
#define R(seq, job, m, x) {seq, job, m, x, 0}

/* One 720p guest, one viewport, host AA on (no MLAA/Transpose batches), pyramid depth 5 (idx11..idx19), re/data/post_jobs.tsv
 * "CHAIN" expanded one row per batch. ASSUMPTION (flagged in the RE notes "Pass 163"): each wrapper call (AddPostStep)
 * is one batch, blur = H then V; the pyramid has five levels at 1280x720 (idx11/13/15/17/19). The first K4 run with
 * KZ3_GPU_POST_LOG dumps the real list on a mismatch ([gpost-list] lines), which settles both. */
const kz3_gpost_row kz3_gpost_rows[] = {
    R(6, J_FUZZ, 0x2, 0),
    BL(7, 0x2, 0),
    R(8, J_MOD, 0x2, 0),
    R(9, J_ZERO, 0x1019F, 0x10000),
    R(10, J_UP, 0x2, 0),
    BL2(11, 0x1),
    R(12, J_UP, 0x1, 0),
    {13, J_MB, 0x1, 0, 1}, /* optional: not queued when Options > Display > Motion blur is off (rsx patch 2107) */
    R(14, J_ZERO, 0x1019F, 1),
    R(15, J_DOF, 0x2, 0),
    R(16, J_DOWN, 0x14, 0),
    R(17, J_BCAP, 0x14, 0),
    BL(18, 0x14, 0),
    R(19, J_DOWN, 0x14, 0),
    BL(20, 0x14, 0),
    R(20, J_DOWN, 0x14, 0), BL(20, 0x14, 0),
    R(20, J_DOWN, 0x14, 0), BL(20, 0x14, 0),
    R(20, J_DOWN, 0x14, 0), BL(20, 0x14, 0),
    R(21, J_MOD, 0x14, 0), R(21, J_MOD, 0x14, 0), R(21, J_MOD, 0x14, 0), R(21, J_MOD, 0x14, 0), R(21, J_MOD, 0x14, 0),
    R(22, J_UP, 0x14, 0), R(22, J_UP, 0x14, 0), R(22, J_UP, 0x14, 0), R(22, J_UP, 0x14, 0),
    R(23, J_BINT, 0x14, 0),
    R(24, J_DOWN, 0x10, 0),
    R(25, J_VIG, 0x10, 0),
    R(26, J_ILR, 0x10, 0),
    BL2(27, 0x10),
    R(28, J_ZERO, 0x1019F, 0),
    R(29, J_ZERO, 0x1019F, 0),
    R(30, J_UP, 0x10, 0),
    R(31, J_ILRS, 0x10, 0),
    R(32, J_UP, 0x14, 0),
    R(33, J_VIG, 0x80, 0),
    R(34, J_DOWN, 0x8, 0),
    R(35, J_GRAIN, 0x8, 0),
    R(36, J_UP, 0x100, 0),
};
const int kz3_gpost_nrows = (int)(sizeof(kz3_gpost_rows) / sizeof(kz3_gpost_rows[0]));

uint64_t kz3_gpost_parse_steps(const char* s)
{
    if (!s || !*s) return 0;
    return (uint64_t)strtoull(s, NULL, 16) & KZ3_GPOST_VALID_STEPS;
}

static uint8_t job_of_module(kz3_gpost_build build, uint32_t va)
{
    const kz3_gpost_modva* t = build == KZ3_GP_BUILD_100 ? kz3_gpost_modva_100 : kz3_gpost_modva_114;
    for (int i = 0; i < KZ3_GPOST_NMODVA; i++)
        if (t[i].va == va) return t[i].id;
    return 0;
}

static void p2_snapshot(const kz3_gpost_mem* m, kz3_gpost_build build, kz3_gpost_plan* p);
static int runs(uint32_t mask, uint32_t excl, uint32_t S) { return (mask & S) != 0 && (excl & S) == 0; }

void kz3_gpost_identify(const kz3_gpost_mem* m, kz3_gpost_build build, uint32_t item, uint32_t renderer, kz3_gpost_plan* p)
{
    p->item = item;
    p->build = (uint8_t)build;
    p->renderer = renderer;
    p->frame = m->r32(m->u, renderer + KZ3_GPOST_R_FRAME);
    p->S = m->r32(m->u, item + KZ3_GPOST_ITEM_S);
    p->match = 0;
    p->nomatch_at = -1;
    p->reason = "none";
    uint32_t n = m->r32(m->u, renderer + KZ3_GPOST_R_COUNT);
    uint32_t list = m->r32(m->u, renderer + KZ3_GPOST_R_LIST);
    if (n > KZ3_GPOST_MAXB) { p->nb = 0; p->reason = "toomany"; return; }
    p->nb = n;
    int multi_ctx = 0;
    for (uint32_t i = 0; i < n; i++) {
        kz3_gpost_batch* b = &p->b[i];
        memset(b, 0, sizeof *b);
        b->ea = m->r32(m->u, list + 4 * i);
        b->count = m->r32(m->u, b->ea + KZ3_GPOST_B_COUNT);
        b->entries = m->r32(m->u, b->ea + KZ3_GPOST_B_ENTRIES);
        b->mask = m->r32(m->u, b->ea + KZ3_GPOST_B_MASK);
        b->excl = m->r32(m->u, b->ea + KZ3_GPOST_B_EXCL);
        b->ctx = m->r32(m->u, b->ea + KZ3_GPOST_B_CTX);
        if (b->ctx) multi_ctx = 1;
        if (b->count && b->entries) {
            /* the entry is (size<<48)|(type<<32)|EA of the job descriptor; big-endian guest: high word first */
            uint32_t ea = m->r32(m->u, b->entries + 4);
            if (ea) {
                b->w0 = m->r32(m->u, ea);
                b->w1 = m->r32(m->u, ea + 4);
                b->job = job_of_module(build, b->w0);
            }
        }
        uint32_t S = m->r32(m->u, item + KZ3_GPOST_ITEM_S + 4 * b->ctx);
        b->runs = (uint8_t)runs(b->mask, b->excl, S);
    }
    /* greedy match against the flat table; an unknown job id (0) matches any row, mask and excl must be equal, ctx 0 */
    int j = 0, ok = 1;
    for (int i = 0; i < kz3_gpost_nrows && ok; i++) {
        const kz3_gpost_row* r = &kz3_gpost_rows[i];
        int hit = (uint32_t)j < n && p->b[j].ctx == 0 && p->b[j].mask == r->mask && p->b[j].excl == r->excl &&
                  (p->b[j].job == 0 || p->b[j].job == r->job);
        if (hit) {
            p->b[j].seq = r->seq;
            p->b[j].rowjob = r->job;
            j++;
        } else if (!r->optional) {
            ok = 0;
            p->nomatch_at = j;
        }
    }
    if (ok && (uint32_t)j != n) { ok = 0; p->nomatch_at = j; }
    p->match = ok;
    if (ok) { p->reason = "claim"; return; }
    for (uint32_t i = 0; i < n; i++) p->b[i].seq = p->b[i].rowjob = 0;
    if (multi_ctx) p->reason = "viewports";
    else if (n && p->b[0].job == J_TRANSPOSE) p->reason = "aa-off";
    else p->reason = "nomatch";
}

uint32_t kz3_gpost_begin(const kz3_gpost_mem* m, kz3_gpost_build build, uint32_t item, uint32_t renderer, uint64_t want,
                         kz3_gpost_plan* p, kz3_gpost_saved* sv)
{
    sv->n = 0;
    p->want = want & KZ3_GPOST_VALID_STEPS;
    p->claim = p->pred_skip = 0;
    p->nclaimed = p->blob_used = p->blob_trunc = 0;
    kz3_gpost_identify(m, build, item, renderer, p);
    if (!p->match) return 0;
    if ((p->S & 0x100) == 0) { p->reason = "s0x100"; return 0; } /* claim condition: the final composite runs (s.3.4) */
    for (uint32_t i = 0; i < p->nb; i++) {
        kz3_gpost_batch* b = &p->b[i];
        if (!((p->want >> b->seq) & 1)) continue;
        if (!b->runs) { p->pred_skip |= 1ull << b->seq; continue; }
        sv->ea[sv->n] = b->ea;
        sv->mask[sv->n] = b->mask;
        sv->n++;
        b->claimed = 1;
        p->claim |= 1ull << b->seq;
        m->w32(m->u, b->ea + KZ3_GPOST_B_MASK, 0);
    }
    p->nclaimed = sv->n;
    return sv->n;
}

void kz3_gpost_end(const kz3_gpost_mem* m, kz3_gpost_plan* p, const kz3_gpost_saved* sv)
{
    for (uint32_t i = 0; i < sv->n; i++) m->w32(m->u, sv->ea[i] + KZ3_GPOST_B_MASK, sv->mask[i]);
    for (uint32_t i = 0; i < p->nb; i++) {
        const kz3_gpost_batch* b = &p->b[i];
        if (!b->claimed) continue;
        /* per claimed batch: {u32 index, u32 n, n x (u64 entry, descriptor bytes)}; the descriptor length is the entry's
         * size field (bits 48..63), clamped to 0x100 */
        uint32_t need = 8;
        uint32_t used0 = p->blob_used;
        if (used0 + need > KZ3_GPOST_BLOB) { p->blob_trunc = 1; continue; }
        uint32_t* hdr = (uint32_t*)(p->blob + used0);
        hdr[0] = i;
        hdr[1] = 0;
        p->blob_used += 8;
        for (uint32_t e = 0; e < b->count; e++) {
            uint32_t hi = m->r32(m->u, b->entries + 8 * e), lo = m->r32(m->u, b->entries + 8 * e + 4);
            uint32_t sz = (hi >> 16) & 0xFFFF, type = hi & 0xFFFF;
            if (sz > 0x100) sz = 0x100;
            if (type != 1) sz = 0;
            sz = (sz + 3) & ~3u;
            if (p->blob_used + 8 + sz > KZ3_GPOST_BLOB) { p->blob_trunc = 1; break; }
            uint32_t* w = (uint32_t*)(p->blob + p->blob_used);
            w[0] = hi;
            w[1] = lo;
            for (uint32_t k = 0; k < sz; k += 4) w[2 + k / 4] = m->r32(m->u, lo + k);
            p->blob_used += 8 + sz;
            hdr[1]++;
        }
    }
    p2_snapshot(m, (kz3_gpost_build)p->build, p);
    kz3_gpost_publish(p);
}

/* ---------------- P2: command-list / user-block / parameter-block snapshot ---------------- */

/* ASSUMPTION: the command lists, entry arrays, input/RMW lists and parameter blocks of the claimed steps are not recycled or
 * rewritten by the game between the start of the wrapped runner and kz3_gpost_end() reading them: end() runs after the lifted
 * runner returned, and nothing here proves the runner (or another PPU thread) leaves that memory alone. A recycled list would
 * read as garbage and be rejected by the checks below, but a recycled list that is still well formed would be snapshotted
 * silently. First K4 run: [gpost-p2] rej=0 and the entry sizes of the layout is the evidence for the layout; no run shows the
 * lifetime. */

static const char* const k_rej_names[KZ3_GP_REJ_N] = {"none",  "type",  "size", "ea",     "cmd", "module", "job", "d",
                                                      "shape", "plist", "pblk", "inlist", "rmw", "tiles",  "blob"};
const char* kz3_gpost_rej_name(kz3_gpost_rej r)
{
    return (unsigned)r < KZ3_GP_REJ_N ? k_rej_names[r] : "?";
}

/* Expected shape per job (checked on the wrapper arguments): inputs, parameter records, first block
 * size (0 = no parameters), extra scratch. ILRSharp: 0x20 block then idx9 chunks (>= 1 chunk). */
static const struct {
    uint8_t job, nin, nparam, extra;
    uint16_t p0;
} k_shape[] = {
    {J_BCAP, 2, 1, 0, 0x220}, {J_BINT, 1, 1, 0, 0x10},   {J_DOF, 2, 0, 1, 0},   {J_FUZZ, 1, 1, 0, 0x10},
    {J_DOWN, 1, 0, 0, 0},     {J_GRAIN, 2, 1, 0, 0x210}, {J_HBLUR, 1, 0, 0, 0}, {J_ILR, 1, 0, 0, 0},
    {J_ILRS, 0, 0, 0, 0x20},  {J_MOD, 1, 1, 0, 0x10},    {J_MB, 2, 0, 1, 0},    {J_UP, 1, 0, 0, 0},
    {J_VBLUR, 1, 0, 0, 0},    {J_VIG, 1, 1, 0, 0x10},    {J_ZERO, 0, 0, 0, 0},
};

typedef struct {
    uint32_t size, modea, modebytes, tmask, brec, ncmd;
    int nin, rmw, params, extra, mode, have_mod, have_t3, have_t4;
    uint32_t inl_ea[4], inl_bytes[4], rmw_ea, rmw_bytes, pl_ea, pl_bytes;
    uint32_t inw[4], dst, tw, th, pitch, flags, col, cols, row, rows, bpp;
} kz3_gp_tile;

/* A followed range [ea, ea+len) lies in main memory: ea inside [LO, HI) and len <= HI - ea, so ea+len never wraps and a range
 * that starts below HI but crosses it is refused (the end is compared as a distance, not as a sum). */
static int ea_ok(uint32_t ea, uint32_t len)
{
    return ea >= KZ3_GPOST_EA_LO && ea < KZ3_GPOST_EA_HI && len <= KZ3_GPOST_EA_HI - ea;
}

/* walk one tile's command list and read its D. hi = the entry's first word, lo = the command-list EA. */
static kz3_gpost_rej walk_tile(const kz3_gpost_mem* m, uint32_t hi, uint32_t lo, kz3_gp_tile* t)
{
    memset(t, 0, sizeof *t);
    uint32_t size = (hi >> 16) & 0xFFF, type = hi & 0xFFFF;
    if (type != 1 || (hi >> 28) != 0) return KZ3_GP_REJ_TYPE;
    if (size < 0x60 || size > 0x100 || (size & 0xF)) return KZ3_GP_REJ_SIZE;
    if ((lo & 0xF) || !ea_ok(lo, size)) return KZ3_GP_REJ_EA;
    t->size = size;
    uint32_t cap = (size - 0x30) / 8;
    uint32_t i;
    int done = 0;
    for (i = 0; i < cap && !done; i++) {
        uint32_t ch = m->r32(m->u, lo + 8 * i), cl = m->r32(m->u, lo + 8 * i + 4);
        uint32_t ct = (ch >> 9) & 7;
        if (ct == 1) {
            if ((ch & ~0x1E0u) != 0x200 || ((ch >> 5) & 0xF) > 8) return KZ3_GP_REJ_CMD;
        } else if (ct == 2) {
            uint32_t idx = (ch >> 5) & 0xF, a = (ch >> 15) & 1, b = (ch >> 14) & 1, sz = (ch >> 18) * 16;
            if (idx == 0 && a && !b) {
                if (t->have_mod) return KZ3_GP_REJ_CMD;
                t->have_mod = 1;
                t->modea = cl & ~0xFu;
                t->modebytes = sz;
            } else if (idx == 1 && !a && !b) {
                if (t->mode) return KZ3_GP_REJ_CMD;
                t->mode = 1;
            } else if (idx >= 2 && idx <= 5 && a && b) {
                if ((int)idx != 2 + t->nin) return KZ3_GP_REJ_CMD; /* inputs in order, no gaps */
                t->inl_ea[t->nin] = cl & ~0xFu;
                t->inl_bytes[t->nin] = sz;
                t->nin++;
            } else if (idx == 6 && a && b) {
                if (t->rmw) return KZ3_GP_REJ_CMD;
                t->rmw = 1;
                t->rmw_ea = cl & ~0xFu;
                t->rmw_bytes = sz;
            } else if (idx == 7 && a && b) {
                if (t->params) return KZ3_GP_REJ_CMD;
                t->params = 1;
                t->pl_ea = cl & ~0xFu;
                t->pl_bytes = sz;
            } else if (idx == 8 && !a && !b) {
                if (t->extra) return KZ3_GP_REJ_CMD;
                t->extra = 1;
            } else
                return KZ3_GP_REJ_CMD;
        } else if (ct == 3) {
            if (ch != 0x600 || t->have_t3) return KZ3_GP_REJ_CMD;
            t->have_t3 = 1;
            t->tmask = cl;
        } else if (ct == 4) {
            if (ch != 0x800 || t->have_t4) return KZ3_GP_REJ_CMD;
            t->have_t4 = 1;
            t->brec = cl;
        } else if (ct == 5) {
            if (ch != 0xA00 || cl != 0) return KZ3_GP_REJ_CMD;
            done = 1;
        } else
            return KZ3_GP_REJ_CMD;
    }
    if (!done) return KZ3_GP_REJ_CMD;
    t->ncmd = i;
    if (((8 * t->ncmd + 15) & ~15u) + 0x30 != size) return KZ3_GP_REJ_SIZE;
    if (!t->have_mod) return KZ3_GP_REJ_MODULE;
    if (!t->have_t3 || !t->have_t4) return KZ3_GP_REJ_CMD;
    if (t->ncmd != 5u + 2u * (uint32_t)(t->mode + t->rmw + t->nin + t->extra + t->params)) return KZ3_GP_REJ_CMD;
    /* D */
    uint32_t d = lo + size - 0x30;
    for (int k = 0; k < 4; k++) t->inw[k] = m->r32(m->u, d + 4 * (uint32_t)k);
    t->dst = m->r32(m->u, d + 0x10);
    uint32_t w14 = m->r32(m->u, d + 0x14), w18 = m->r32(m->u, d + 0x18), w1c = m->r32(m->u, d + 0x1C),
             w20 = m->r32(m->u, d + 0x20);
    t->tw = w14 >> 16;
    t->th = w14 & 0xFFFF;
    t->pitch = w18 >> 16;
    t->flags = w18 & 0xFFFF;
    t->col = (w1c >> 24) & 0xFF;
    t->cols = (w1c >> 16) & 0xFF;
    t->row = (w1c >> 8) & 0xFF;
    t->rows = w1c & 0xFF;
    t->bpp = w20 >> 24;
    uint32_t fl = t->flags;
    int fin = 0;
    for (int k = 0; k < 4; k++) fin += (fl >> (5 + k)) & 1;
    if (!t->tw || !t->th || !t->cols || !t->rows || t->col >= t->cols || t->row >= t->rows) return KZ3_GP_REJ_D;
    if (!(t->bpp == 1 || t->bpp == 2 || t->bpp == 4 || t->bpp == 8 || t->bpp == 16) ||
        t->pitch < (uint64_t)t->tw * t->bpp)
        return KZ3_GP_REJ_D;
    if (fin != t->nin || (fl & ((1u << (5 + t->nin)) - 0x20u)) != ((1u << (5 + t->nin)) - 0x20u)) return KZ3_GP_REJ_D;
    if (((fl >> 9) & 1) != (uint32_t)t->params || ((fl >> 10) & 1) != (uint32_t)t->extra ||
        ((fl >> 4) & 1) != (uint32_t)t->mode)
        return KZ3_GP_REJ_D;
    if ((((fl >> 11) & 1) | ((fl >> 12) & 1)) != (uint32_t)t->rmw) return KZ3_GP_REJ_D;
    if (!t->dst) return KZ3_GP_REJ_D;
    for (int k = 0; k < 4; k++) {
        if (k < t->nin && (((t->inw[k] >> 24) == 0) || (((t->inw[k] >> 16) & 0xFF) == 0) || ((t->inw[k] & 0x3F) == 0)))
            return KZ3_GP_REJ_D;
        if (k >= t->nin && t->inw[k] != 0) return KZ3_GP_REJ_D;
    }
    return KZ3_GP_REJ_NONE;
}

static const kz3_gpost_pstep* find_step_by_batch(const kz3_gpost_plan* p, uint32_t batch)
{
    for (uint32_t i = 0; i < p->p2.nstep; i++)
        if (p->p2.s[i].batch == batch) return &p->p2.s[i];
    return NULL;
}
const kz3_gpost_pstep* kz3_gpost_p2_step(const kz3_gpost_plan* p, uint32_t batch)
{
    return find_step_by_batch(p, batch);
}

const uint32_t* kz3_gpost_p2_block(const kz3_gpost_plan* p, const kz3_gpost_pelem* e)
{
    if (!e->copied || (uint32_t)e->off + e->copied > p->p2.blob_used) return NULL;
    return (const uint32_t*)(p->p2.blob + e->off);
}

/* which surface does a list's first element belong to (tile 0, column 0, row 0: undo the border shift) */
static int surf_of(const kz3_gpost_p2* q, uint32_t first_ea, uint32_t inw)
{
    uint32_t bx = (inw >> 11) & 0x1F, by = (inw >> 6) & 0x1F, bpp = inw & 0x3F;
    int hit = -1, n = 0;
    if (!q->surf_valid) return -1;
    for (int i = 0; i < KZ3_GPOST_NSURF; i++) {
        uint32_t ea = q->surf[i][0], pitch = q->surf[i][3], h = q->surf[i][2];
        if (!ea || !pitch) continue;
        /* 64-bit: pitch, h and the border shift come from guest memory, their products must not wrap */
        uint64_t e = (uint64_t)first_ea + (uint64_t)by * pitch + (uint64_t)bx * bpp;
        if (e >= ea && e - ea < (uint64_t)pitch * h) {
            hit = i;
            n++;
        }
    }
    return n == 1 ? hit : -1;
}

kz3_gpost_rej kz3_gpost_p2_parse(const kz3_gpost_mem* m, kz3_gpost_build build, kz3_gpost_plan* p, uint32_t bi,
                                 kz3_gpost_pstep* st)
{
    memset(st, 0, sizeof *st);
    st->batch = bi;
    st->ilrs_chunks = -1;
    for (int k = 0; k < 4; k++) st->in[k].surf = -1;
    st->rmwl.surf = -1;
    const kz3_gpost_batch* b = &p->b[bi];
    kz3_gpost_p2* q = &p->p2;
    kz3_gpost_rej r;
    kz3_gp_tile t0, t;
    uint32_t blob0 = q->blob_used; /* restored on a reject so a bad step holds no blob bytes */
#define FAIL(code)                                                                                                     \
    do {                                                                                                               \
        q->blob_used = blob0;                                                                                          \
        st->ok = 0;                                                                                                    \
        st->rej = (uint8_t)(code);                                                                                     \
        return (code);                                                                                                 \
    } while (0)

    if (!b->count || !b->entries) FAIL(KZ3_GP_REJ_TILES);
    if (b->count > KZ3_GPOST_P2_MAXTILE) FAIL(KZ3_GP_REJ_TILES);
    /* the entry array itself (8 bytes per tile) is guest memory too: validate it before the first read */
    if (!ea_ok(b->entries, 8u * b->count)) FAIL(KZ3_GP_REJ_EA);
    uint32_t e0hi = m->r32(m->u, b->entries), e0lo = m->r32(m->u, b->entries + 4);
    if ((r = walk_tile(m, e0hi, e0lo, &t0)) != 0) FAIL(r);
    st->ntiles = b->count;
    st->entry_size = t0.size;
    st->module_ea = t0.modea;
    st->module_bytes = t0.modebytes;
    st->tmask = t0.tmask;
    st->brec = t0.brec;
    st->nin = (uint8_t)t0.nin;
    st->rmw = (uint8_t)t0.rmw;
    st->extra = (uint8_t)t0.extra;
    st->mode = (uint8_t)((t0.flags & 0x800)    ? 2
                         : (t0.flags & 0x1000) ? 3
                                               : t0.mode); /* D flags: 1 write, 2/3 read-modify-write (s.4) */
    memcpy(st->inw, t0.inw, sizeof st->inw);
    st->dst_ea = t0.dst;
    st->tw = t0.tw;
    st->th = t0.th;
    st->pitch = t0.pitch;
    st->flags = t0.flags;
    st->cols = t0.cols;
    st->rows = t0.rows;
    st->bpp = t0.bpp;
    st->plist_ea = t0.pl_ea;
    st->plist_bytes = t0.pl_bytes;

    /* job: exact from the module GET, must equal the matched table row */
    st->job = job_of_module(build, t0.modea);
    if (!st->job) FAIL(KZ3_GP_REJ_MODULE);
    if (b->rowjob && b->rowjob != st->job) FAIL(KZ3_GP_REJ_JOB);
    if (t0.brec != b->ea) FAIL(KZ3_GP_REJ_CMD); /* the completion record names this batch */

    /* shape per job */
    int si = -1;
    for (unsigned i = 0; i < sizeof k_shape / sizeof k_shape[0]; i++)
        if (k_shape[i].job == st->job) si = (int)i;
    if (si < 0) FAIL(KZ3_GP_REJ_SHAPE);
    if (t0.nin != k_shape[si].nin || t0.extra != k_shape[si].extra) FAIL(KZ3_GP_REJ_SHAPE);
    if (t0.params != (k_shape[si].p0 != 0)) FAIL(KZ3_GP_REJ_SHAPE);

    /* every tile: same shape, row-major order, destination arithmetic (tile 0 is (0,0) by the order check) */
    if (b->count != t0.cols * t0.rows) FAIL(KZ3_GP_REJ_TILES);
    for (uint32_t e = 0; e < b->count; e++) {
        const kz3_gp_tile* tp = &t0;
        if (e) {
            uint32_t hi = m->r32(m->u, b->entries + 8 * e), lo = m->r32(m->u, b->entries + 8 * e + 4);
            if ((r = walk_tile(m, hi, lo, &t)) != 0) FAIL(KZ3_GP_REJ_TILES);
            tp = &t;
        }
        if (tp->row != e / t0.cols || tp->col != e % t0.cols) FAIL(KZ3_GP_REJ_TILES);
        if (tp->cols != t0.cols || tp->rows != t0.rows || tp->tw != t0.tw || tp->th != t0.th || tp->pitch != t0.pitch ||
            tp->bpp != t0.bpp || (tp->flags & 0xFFF0u) != (t0.flags & 0xFFF0u) || tp->nin != t0.nin ||
            tp->rmw != t0.rmw || tp->params != t0.params || tp->extra != t0.extra || tp->mode != t0.mode ||
            tp->modea != t0.modea || tp->brec != t0.brec || tp->tmask != t0.tmask ||
            memcmp(tp->inw, t0.inw, sizeof t0.inw) != 0)
            FAIL(KZ3_GP_REJ_TILES);
        /* 64-bit: row*th*pitch reaches 2^40; in 32 bits it wraps and a bad tile could land on the expected address */
        if ((uint64_t)tp->dst !=
            (uint64_t)t0.dst + (uint64_t)tp->row * t0.th * t0.pitch + (uint64_t)tp->col * t0.tw * t0.bpp)
            FAIL(KZ3_GP_REJ_TILES);
        /* first-column/row/last flags */
        uint32_t want = (tp->col == 0 ? 1u : 0u) | (tp->col == t0.cols - 1 ? 2u : 0u) | (tp->row == 0 ? 4u : 0u) |
                        (tp->row == t0.rows - 1 ? 8u : 0u);
        if ((tp->flags & 0xFu) != want) FAIL(KZ3_GP_REJ_D);
    }

    /* input lists of tile 0: size/count/first element against the input word */
    for (int k = 0; k < t0.nin; k++) {
        uint32_t rows = (t0.inw[k] >> 16) & 0xFF, nel = (rows + 1) & ~1u;
        uint32_t bytes = t0.inl_bytes[k];
        kz3_gpost_inlist* il = &st->in[k];
        il->list_ea = t0.inl_ea[k];
        il->list_bytes = bytes;
        il->nel = nel;
        if (bytes != nel * 8 || (il->list_ea & 7) || !ea_ok(il->list_ea, bytes)) FAIL(KZ3_GP_REJ_INLIST);
        il->first_size = m->r32(m->u, il->list_ea);
        il->first_ea = m->r32(m->u, il->list_ea + 4);
        uint64_t want = (uint64_t)(t0.inw[k] >> 24) * (t0.inw[k] & 0x3F);
        if (il->first_size != want || il->first_size > 0x4000 || !il->first_ea) FAIL(KZ3_GP_REJ_INLIST);
        if (rows & 1) { /* odd count: the pad element is {0,0} */
            if (m->r32(m->u, il->list_ea + 8 * rows) || m->r32(m->u, il->list_ea + 8 * rows + 4))
                FAIL(KZ3_GP_REJ_INLIST);
        }
    }
    if (t0.rmw) {
        uint32_t nel = (t0.th + 1) & ~1u;
        kz3_gpost_inlist* il = &st->rmwl;
        il->list_ea = t0.rmw_ea;
        il->list_bytes = t0.rmw_bytes;
        il->nel = nel;
        if (t0.rmw_bytes != nel * 8 || (il->list_ea & 7) || !ea_ok(il->list_ea, t0.rmw_bytes)) FAIL(KZ3_GP_REJ_RMW);
        il->first_size = m->r32(m->u, il->list_ea);
        il->first_ea = m->r32(m->u, il->list_ea + 4);
        if ((uint64_t)il->first_size != (uint64_t)t0.tw * t0.bpp || il->first_ea != t0.dst) FAIL(KZ3_GP_REJ_RMW);
    }

    /* surface table (per frame, once) then map each input list to a surface */
    for (int k = 0; k < t0.nin; k++) st->in[k].surf = (int8_t)surf_of(q, st->in[k].first_ea, t0.inw[k]);

    /* parameter list and blocks (shared by all tiles) */
    if (t0.params) {
        uint32_t bytes = t0.pl_bytes, nel = bytes / 8;
        if (bytes == 0 || bytes > 8 * KZ3_GPOST_P2_MAXPARAM || (bytes & 0xF) || (t0.pl_ea & 7) ||
            !ea_ok(t0.pl_ea, bytes))
            FAIL(KZ3_GP_REJ_PLIST);
        uint32_t np = nel;
        if (m->r32(m->u, t0.pl_ea + 8 * (nel - 1)) == 0 && m->r32(m->u, t0.pl_ea + 8 * (nel - 1) + 4) == 0)
            np = nel - 1;
        st->nparam = (uint8_t)np;
        if (np == 0) FAIL(KZ3_GP_REJ_SHAPE);
        if (k_shape[si].job == J_ILRS) {
            if (np < 2) FAIL(KZ3_GP_REJ_SHAPE);
        } else if (np != 1)
            FAIL(KZ3_GP_REJ_SHAPE);
        for (uint32_t j = 0; j < np; j++) {
            kz3_gpost_pelem* pe = &st->pe[j];
            pe->size = m->r32(m->u, t0.pl_ea + 8 * j);
            pe->ea = m->r32(m->u, t0.pl_ea + 8 * j + 4);
            if (!pe->size || (pe->size & 0xF) || pe->size > 0x4000 || !pe->ea) FAIL(KZ3_GP_REJ_PBLK);
            if (j == 0 && pe->size != k_shape[si].p0) FAIL(KZ3_GP_REJ_SHAPE);
            if (j > 0 && k_shape[si].job != J_ILRS) FAIL(KZ3_GP_REJ_SHAPE);
            if (j > 0 && (pe->ea & 0xF)) FAIL(KZ3_GP_REJ_PBLK);
            if (j == 0) { /* the parameter block proper: copied (chunks of ILRSharp are idx9 slices, EA/size only) */
                if (pe->size > KZ3_GPOST_P2_BLK_MAX || (pe->ea & 3) || !ea_ok(pe->ea, pe->size)) FAIL(KZ3_GP_REJ_PBLK);
                if (q->blob_used + pe->size > KZ3_GPOST_P2_BLOB) FAIL(KZ3_GP_REJ_BLOB);
                uint32_t* w = (uint32_t*)(q->blob + q->blob_used);
                for (uint32_t o = 0; o < pe->size; o += 4) w[o / 4] = m->r32(m->u, pe->ea + o);
                pe->off = (uint16_t)q->blob_used;
                pe->copied = (uint16_t)pe->size;
                q->blob_used += pe->size;
            }
        }
        if (k_shape[si].job == J_ILRS) {
            /* chunks (j >= 1): EA = idx9.EA + k*0x4000, size = min(rest, 0x4000), total = pitch*h of idx9 */
            if (q->surf_valid && q->surf[9][0]) {
                uint64_t total = (uint64_t)q->surf[9][3] * q->surf[9][2],
                         rest = total; /* 64-bit: pitch*h must not wrap */
                int ok = (uint64_t)(np - 1) == (total + 0x3FFF) / 0x4000;
                for (uint32_t j = 1; j < np && ok; j++) {
                    uint64_t sz = rest > 0x4000 ? 0x4000 : rest;
                    if ((uint64_t)st->pe[j].ea != (uint64_t)q->surf[9][0] + (uint64_t)(j - 1) * 0x4000 ||
                        st->pe[j].size != ((sz + 15) & ~15u))
                        ok = 0;
                    rest -= sz;
                }
                st->ilrs_chunks = (int8_t)ok;
            }
        }
    }
    st->ok = 1;
    st->rej = 0;
#undef FAIL
    return KZ3_GP_REJ_NONE;
}

static void p2_snapshot(const kz3_gpost_mem* m, kz3_gpost_build build, kz3_gpost_plan* p)
{
    kz3_gpost_p2* q = &p->p2;
    q->frame = p->frame;
    q->nstep = q->nok = q->nrej = q->blob_used = 0;
    memset(q->rej, 0, sizeof q->rej);
    q->surf_valid = 0;
    if (!p->nclaimed) return;
    if (ea_ok(p->renderer, KZ3_GPOST_SURF_BASE + KZ3_GPOST_NSURF * KZ3_GPOST_SURF_STRIDE)) {
        for (int i = 0; i < KZ3_GPOST_NSURF; i++)
            for (int k = 0; k < 7; k++)
                q->surf[i][k] = m->r32(m->u, p->renderer + KZ3_GPOST_SURF_BASE + (uint32_t)i * KZ3_GPOST_SURF_STRIDE +
                                                 4u * (uint32_t)k);
        q->surf_valid = 1;
    }
    for (uint32_t i = 0; i < p->nb; i++) {
        kz3_gpost_batch* b = &p->b[i];
        if (!b->claimed) continue;
        /* step capacity: the batch is counted as a BLOB reject but gets no q->s[] entry, so kz3_gpost_p2_step() returns NULL for it */
        if (q->nstep >= KZ3_GPOST_P2_MAXSTEP) {
            q->rej[KZ3_GP_REJ_BLOB]++;
            q->nrej++;
            continue;
        }
        kz3_gpost_pstep* st = &q->s[q->nstep];
        kz3_gpost_rej r = kz3_gpost_p2_parse(m, build, p, i, st);
        q->nstep++;
        if (r == KZ3_GP_REJ_NONE) {
            q->nok++;
            /* writes into the P1 part of the plan (documented at kz3_gpost_p2_parse in the header): the exact id from the module GET
             * (w0 is not the module) replaces P1's unknown 0 */
            if (!b->job) b->job = st->job;
        } else {
            q->nrej++;
            q->rej[r]++;
        }
    }
}

int kz3_gpost_format_p2(const kz3_gpost_plan* p, char* out, int n)
{
    const kz3_gpost_p2* q = &p->p2;
    int k = snprintf(out, (size_t)n, "[gpost-p2] frame=%u steps=%u ok=%u rej=%u blob=%u surf=%u", q->frame, q->nstep,
                     q->nok, q->nrej, q->blob_used, q->surf_valid);
    for (int r = 1; r < KZ3_GP_REJ_N && k > 0 && k < n; r++)
        if (q->rej[r]) k += snprintf(out + k, (size_t)(n - k), " %s=%u", k_rej_names[r], q->rej[r]);
    return k;
}

int kz3_gpost_format_p2_step(const kz3_gpost_plan* p, uint32_t i, char* out, int n)
{
    const kz3_gpost_pstep* s = &p->p2.s[i];
    return snprintf(out, (size_t)n,
                    "[gpost-p2-step] batch=%u seq=%u job=%u ok=%u rej=%s tiles=%u size=0x%X nin=%u np=%u rmw=%u "
                    "mod=0x%08X D: dst=0x%08X tile=%ux%u grid=%ux%u pitch=%u bpp=%u flags=0x%X",
                    s->batch, p->b[s->batch].seq, s->job, s->ok, k_rej_names[s->rej < KZ3_GP_REJ_N ? s->rej : 0],
                    s->ntiles, s->entry_size, s->nin, s->nparam, s->rmw, s->module_ea, s->dst_ea, s->tw, s->th, s->cols,
                    s->rows, s->pitch, s->bpp, s->flags);
}

int kz3_gpost_format(const kz3_gpost_plan* p, char* out, int n)
{
    int k = snprintf(out, (size_t)n,
                     "[gpost] frame=%u S=0x%X nb=%u match=%d reason=%s want=0x%llX claim=0x%llX predskip=0x%llX zeroed=%u",
                     p->frame, p->S, p->nb, p->match, p->reason, (unsigned long long)p->want, (unsigned long long)p->claim,
                     (unsigned long long)p->pred_skip, p->nclaimed);
    if (p->nomatch_at >= 0 && k > 0 && k < n) k += snprintf(out + k, (size_t)(n - k), " nomatch_at=%d", p->nomatch_at);
    return k;
}

int kz3_gpost_format_batch(const kz3_gpost_plan* p, uint32_t i, char* out, int n)
{
    const kz3_gpost_batch* b = &p->b[i];
    return snprintf(out, (size_t)n, "[gpost-list] i=%u ea=0x%08X job=%u seq=%u mask=0x%X excl=0x%X ctx=%u n=%u runs=%u w0=0x%08X w1=0x%08X",
                    i, b->ea, b->job, b->seq, b->mask, b->excl, b->ctx, b->count, b->runs, b->w0, b->w1);
}

static kz3_gpost_plan g_ring[4];
static volatile uint32_t g_ring_frame[4];
static volatile uint32_t g_ring_valid[4];

void kz3_gpost_publish(const kz3_gpost_plan* p)
{
    uint32_t s = p->frame & 3;
    g_ring_valid[s] = 0;
    memcpy(&g_ring[s], p, sizeof *p);
    g_ring_frame[s] = p->frame;
    g_ring_valid[s] = 1;
}

const kz3_gpost_plan* kz3_gpost_plan_get(uint32_t frame)
{
    uint32_t s = frame & 3;
    return (g_ring_valid[s] && g_ring_frame[s] == frame) ? &g_ring[s] : NULL;
}
