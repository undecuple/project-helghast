/* Host test and harness for KZ3 patch 2171: the Level 1 replay of the walker's FIFO capture,
 * no GPU, no game data. One binary, several modes (argv[1]):
 *
 *   gen DIR            builds a SYNTHETIC capture: the real walker (cellGcmSys.c with stubs, pattern of test_sync_hist.c)
 *                      walks a scripted FIFO with LD_FIFO_CAPTURE on while a stub "renderer" reads, writes and defers
 *                      reads of a fake guest memory. DIR/fifo-capture.kz3fc (DIR needs a 'work' segment).
 *   replay CAP OUT [adv]  feeds CAP through the CURRENT walker path (cellGcm_rsx_process_fifo) with the stub recorder and
 *                      re-captures what the path did into OUT/fifo-capture.kz3fc. With "adv" the adversarial mode runs.
 *   cmp A B            compares the event streams of two capture files (ev_compare, the API step 1 plugs its path into).
 *   check CAP          reader-side check: RANGE records whose digest does not match their bytes (gap 6).
 *   mutate IN OUT KIND copies a capture with one damage: drop-word, flip-range, swap-words, swap-cmds (valid checksums).
 *   (no mode, LD_FIFO_REPLAY=dir set)   replays a REAL capture dir/fifo-capture.kz3fc into dir/replay-out; LD_FIFO_REPLAY_ADV=1
 *                      adds the adversarial mode. Exit 0 = the path reproduced the capture, 1 = divergence, 4 = adversarial
 *                      hazards only. (Real captures are game data and stay under work/; this has not been run on one.)
 *
 * How the replay works (lock step). The walker is the "current path": it consumes the ring rebuilt from the capture's CMD
 * records and emits its own capture records into the re-capture. Whatever the walker does not produce (the renderer's reads
 * and write-backs, SPU/PPU notes, drains, ring-callback writes) is injected by the harness at the capture's positions through
 * rsx_fc_hook, the writer's observer: after each record the walker emits, the harness matches it against the next walker-type
 * record of the capture and then replays the injected records that follow it. A renderer read (RD_RENDERER) is re-read from
 * the harness's own guest-memory model A and re-recorded, so the re-capture holds what the current path would have read.
 * Level 1 compares the event streams of the capture and the re-capture: (seq, method, arg) per command word, a register-file
 * digest per draw, every range with its bytes digest, every guest word, copy geometry, flips, notes (report timestamps,
 * WORD tags 5 and 10 words 0 and 1, and the frame/thread fields are ignored).
 *
 * Adversarial mode (design correctness m4): the capture's reads are attributed to draws (a read inside sink_end follows its
 * draw end; a read outside any draw end belongs to the last draw end before it, a best effort: the capture carries no per-read
 * draw id). A draw's ranges are put into A when the replay reaches its draw end; after every get publication, label, ref
 * publication and flip the harness overwrites every range read so far with a pattern. A read recorded after such an event then
 * reads the pattern: its digest changes = the current path reads guest memory later than the FIFO position of its draw
 * (a B1-class hazard, step 1 must snapshot those bytes). The harness reports them; it does not fix the walker.
 *   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_log_replay.sh */
#include "cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 1; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf(" (line %d)\n", __LINE__); fails++; } } while (0)
static int fails;

/* ======================= event streams (Level 1's currency) ======================= */
enum { EV_METHOD = 1, EV_DRAW, EV_RANGE, EV_WORD, EV_COPY, EV_FLIP, EV_NOTE, EV_DRAIN, EV_NOP, EV_CTL };
typedef struct { uint32_t kind, a, b, c, d, pad; uint64_t dig; } ev_t;   /* pad is always 0: fields are compared one by one */
static int ev_eq(const ev_t* x, const ev_t* y) { return x->kind == y->kind && x->a == y->a && x->b == y->b && x->c == y->c && x->d == y->d && x->dig == y->dig; }
typedef struct { ev_t* e; size_t n, cap; } evs_t;
typedef struct { uint32_t reg[0x4000]; uint32_t draws; int nodraw; } evctx;
static void evs_add(evs_t* s, ev_t e)
{
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; s->e = (ev_t*)realloc(s->e, s->cap * sizeof(ev_t)); }
    s->e[s->n++] = e;
}
static void evs_free(evs_t* s) { free(s->e); memset(s, 0, sizeof *s); }
/* Patch 2190: WORD tags >= 15 are version-2 annotation words (the guest-signal kind of an effect, rsx_guest_signal.h). They are not
 * part of the event identity: the comparison ignores them, the replay neither matches nor injects them (a version-1 capture has none,
 * a version-2 one has them from the original walker, and the path under test emits its own). tools/smoke/test_guest_signal_order.sh
 * reads them from the re-capture directly. */
#define FC_ANNOT_TAG 15u
static int is_annot_head(uint32_t type, const uint8_t* p) { return type == RSX_FC_WORD && rsx_fc_get32(p + 4) >= FC_ANNOT_TAG; }
static int is_structural(uint32_t t) { return t == RSX_FC_CONFIG || t == RSX_FC_IMAGE || t == RSX_FC_PASS || t == RSX_FC_PASS_END || t == RSX_FC_FRAME || t == RSX_FC_END; }

/* One capture record -> events. p/len: the payload (a RANGE's head alone is enough). */
static void ev_from_rec(uint32_t type, const uint8_t* p, uint32_t len, evctx* cx, evs_t* out)
{
    ev_t e; memset(&e, 0, sizeof e);
    switch (type) {
    case RSX_FC_CMD: {
        const uint32_t hdr = rsx_fc_get32(p + 4), n = rsx_fc_get32(p + 8);
        if (len < 12) break;
        if (hdr == 0) { e.kind = EV_NOP; e.a = rsx_fc_get32(p + 12); evs_add(out, e); break; }
        if (n == 0) { e.kind = EV_CTL; e.a = hdr; evs_add(out, e); break; }
        const uint32_t method = hdr & 0x1FFCu, subch = (hdr >> 13) & 7u, noninc = (hdr >> 29) == 2u;
        for (uint32_t i = 0; i < n && 12 + i * 4 + 4 <= len; i++) {
            const uint32_t m = noninc ? method : method + i * 4u, mfull = (subch << 13) | m, arg = rsx_fc_get32(p + 12 + i * 4);
            e.kind = EV_METHOD; e.a = mfull; e.b = arg; evs_add(out, e);
            cx->reg[(mfull >> 2) & 0x3FFF] = arg;
            if (mfull == 0x1808u && arg == 0u && !cx->nodraw) {
                ev_t d; memset(&d, 0, sizeof d);
                d.kind = EV_DRAW; d.a = cx->draws++; d.dig = rsx_fc_fnv(RSX_FC_FNV_SEED, cx->reg, sizeof cx->reg);
                evs_add(out, d);
            }
        }
        break; }
    case RSX_FC_RANGE:
        e.kind = EV_RANGE; e.a = rsx_fc_get32(p) | ((rsx_fc_get32(p + 16) & RSX_FC_F_NULLPTR) << 8) | (rsx_fc_get32(p + 20) << 16);
        e.b = rsx_fc_get32(p + 4); e.c = rsx_fc_get32(p + 8); e.d = rsx_fc_get32(p + 12); e.dig = rsx_fc_get64(p + 24); evs_add(out, e); break;
    case RSX_FC_WORD: {
        const uint32_t dir = rsx_fc_get32(p), tag = rsx_fc_get32(p + 4), ea = rsx_fc_get32(p + 8);
        if (tag >= FC_ANNOT_TAG) break;   /* patch 2190: annotation word, not an event */
        e.kind = EV_WORD; e.a = dir | (tag << 8); e.b = ea; e.c = rsx_fc_get32(p + 12);
        if (dir == 1 && (tag == 5 || tag == 10) && (ea & 8u) == 0) e.c = 0;   /* gap 5: report timestamp words (ps3_ms_now) */
        evs_add(out, e); break; }
    case RSX_FC_COPY: e.kind = EV_COPY; e.dig = rsx_fc_fnv(RSX_FC_FNV_SEED, p, len); evs_add(out, e); break;
    case RSX_FC_FLIP: e.kind = EV_FLIP; e.a = rsx_fc_get32(p); e.b = rsx_fc_get32(p + 4); evs_add(out, e); break;   /* frame ignored */
    case RSX_FC_SPU_NOTE: e.kind = EV_NOTE; e.a = rsx_fc_get32(p); e.b = rsx_fc_get32(p + 4); e.c = rsx_fc_get32(p + 8); e.d = rsx_fc_get32(p + 12); evs_add(out, e); break;   /* thread ignored */
    case RSX_FC_DRAIN: e.kind = EV_DRAIN; e.a = rsx_fc_get32(p); e.b = rsx_fc_get32(p + 4); e.c = rsx_fc_get32(p + 8); evs_add(out, e); break;
    default: break;
    }
}
static uint64_t ev_key(uint32_t type, const uint8_t* p, uint32_t len)
{
    static evctx cx = { .nodraw = 1 };
    evs_t s = {0};
    ev_from_rec(type, p, len, &cx, &s);
    uint64_t h = rsx_fc_fnv(RSX_FC_FNV_SEED, &type, 4);
    for (size_t i = 0; i < s.n; i++) { const uint32_t f[5] = { s.e[i].kind, s.e[i].a, s.e[i].b, s.e[i].c, s.e[i].d }; h = rsx_fc_fnv(h, f, sizeof f); h = rsx_fc_fnv(h, &s.e[i].dig, 8); }
    evs_free(&s);
    return h;
}

/* ======================= capture in memory ======================= */
typedef struct { uint32_t type, len; uint8_t* p; } crec;
typedef struct { crec* r; size_t n, cap; int bad_range; uint32_t frame_start, frame_count; } cap_t;
static int cap_load(cap_t* c, const char* path)
{
    rsx_fc_reader rd; rsx_fc_rec rec; int rc;
    memset(c, 0, sizeof *c);
    if ((rc = rsx_fc_r_open(&rd, path)) != 0) return rc;
    c->frame_start = rd.frame_start; c->frame_count = rd.frame_count;
    while ((rc = rsx_fc_r_next(&rd, &rec)) == 1) {
        if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 4096; c->r = (crec*)realloc(c->r, c->cap * sizeof(crec)); }
        crec* x = &c->r[c->n++];
        x->type = rec.type; x->len = rec.len; x->p = (uint8_t*)malloc(rec.len + 1); memcpy(x->p, rec.p, rec.len);
        if (rec.type == RSX_FC_RANGE && rsx_fc_range_check(&rec) != 0) c->bad_range++;
    }
    rsx_fc_r_close(&rd);
    return rc == 0 ? 0 : -100 + rc;
}
static void cap_free(cap_t* c) { for (size_t i = 0; i < c->n; i++) free(c->r[i].p); free(c->r); memset(c, 0, sizeof *c); }
static void cap_events(const cap_t* c, evs_t* out)
{
    evctx* cx = (evctx*)calloc(1, sizeof *cx);
    for (size_t i = 0; i < c->n; i++) ev_from_rec(c->r[i].type, c->r[i].p, c->r[i].len, cx, out);
    free(cx);
}

/* The comparison API: two event streams in, a result out. Step 1 produces its own stream and calls this. */
typedef struct { size_t n_a, n_b, mismatches, first; char text[600]; } cmp_t;
static const char* ev_name(uint32_t k) { static const char* n[] = { "?", "method", "draw", "range", "word", "copy", "flip", "note", "drain", "nop", "ctl" }; return k < 11 ? n[k] : "?"; }
static void ev_compare(const evs_t* a, const evs_t* b, cmp_t* r)
{
    memset(r, 0, sizeof *r);
    r->n_a = a->n; r->n_b = b->n; r->first = (size_t)-1;
    const size_t m = a->n < b->n ? a->n : b->n;
    for (size_t i = 0; i < m; i++)
        if (!ev_eq(&a->e[i], &b->e[i])) {
            if (!r->mismatches) {
                r->first = i;
                snprintf(r->text, sizeof r->text, "event %zu: %s %x,%x,%x,%x,%llx vs %s %x,%x,%x,%x,%llx", i,
                         ev_name(a->e[i].kind), a->e[i].a, a->e[i].b, a->e[i].c, a->e[i].d, (unsigned long long)a->e[i].dig,
                         ev_name(b->e[i].kind), b->e[i].a, b->e[i].b, b->e[i].c, b->e[i].d, (unsigned long long)b->e[i].dig);
            }
            r->mismatches++;
        }
    r->mismatches += a->n > b->n ? a->n - b->n : b->n - a->n;
    if (!r->text[0] && r->mismatches) { r->first = m; snprintf(r->text, sizeof r->text, "length differs: %zu vs %zu events", a->n, b->n); }
}

/* ======================= the harness's guest-memory model A (renderer reads) ======================= */
/* Open-addressed page table that doubles at 50 % load. It was a fixed 16384 slots (64 MiB of pages): the first in-level
 * capture (183.6 MB of guest bytes) filled it and sm_page probed a full table forever. */
enum { SM_PAGE = 4096, SM_SLOTS0 = 1 << 14 };
typedef struct { uint64_t key; uint8_t* d; } sm_slot;
static sm_slot* g_sm;
static uint32_t g_sm_cap, g_sm_used;
static int g_sm_fixed, g_sm_overflow;   /* control only (smtest old): the pass 156 fixed table, probes bounded so the old hang is reported, not entered */
static uint32_t sm_hash(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32); }
static void sm_grow(void)
{
    const uint32_t ocap = g_sm_cap; sm_slot* const old = g_sm;
    g_sm_cap = ocap ? ocap * 2 : SM_SLOTS0;
    g_sm = (sm_slot*)calloc(g_sm_cap, sizeof(sm_slot));
    if (!g_sm) { fprintf(stderr, "sm_grow: out of memory at %u slots\n", g_sm_cap); exit(2); }
    for (uint32_t i = 0; i < ocap; i++) if (old[i].key) {
        uint32_t h = sm_hash(old[i].key) & (g_sm_cap - 1);
        while (g_sm[h].key) h = (h + 1) & (g_sm_cap - 1);
        g_sm[h] = old[i];
    }
    free(old);
}
static uint8_t* sm_page(uint32_t space, uint32_t addr, int create)
{
    const uint64_t key = ((uint64_t)space << 40) | (addr / SM_PAGE) | (1ull << 62);
    if (!g_sm_cap) { if (!create) return NULL; sm_grow(); }
    uint32_t h = sm_hash(key) & (g_sm_cap - 1);
    for (uint32_t probes = 0;; probes++) {
        if (g_sm_fixed && probes > g_sm_cap) { g_sm_overflow = 1; return NULL; }
        if (g_sm[h].key == key) return g_sm[h].d;
        if (!g_sm[h].key) break;
        h = (h + 1) & (g_sm_cap - 1);
    }
    if (!create) return NULL;
    if (g_sm_fixed) { if (g_sm_used + 1 > g_sm_cap) { g_sm_overflow = 1; return NULL; } }
    else if (2 * (g_sm_used + 1) > g_sm_cap) { sm_grow(); return sm_page(space, addr, create); }
    g_sm[h].key = key; g_sm[h].d = (uint8_t*)calloc(1, SM_PAGE); g_sm_used++;
    return g_sm[h].d;
}
static void sm_write(uint32_t space, uint32_t addr, const uint8_t* b, uint32_t n)
{
    while (n) {                                         /* one page lookup per page, not per byte */
        const uint32_t o = addr % SM_PAGE, k = SM_PAGE - o < n ? SM_PAGE - o : n;
        uint8_t* pg = sm_page(space, addr, 1); if (pg) memcpy(pg + o, b, k);
        addr += k; b += k; n -= k;
    }
}
static void sm_read(uint32_t space, uint32_t addr, uint8_t* b, uint32_t n)
{
    while (n) {
        const uint32_t o = addr % SM_PAGE, k = SM_PAGE - o < n ? SM_PAGE - o : n;
        const uint8_t* p = sm_page(space, addr, 0);
        if (p) memcpy(b, p + o, k); else memset(b, 0, k);
        addr += k; b += k; n -= k;
    }
}
static void sm_reset(void)
{
    for (uint32_t i = 0; i < g_sm_cap; i++) free(g_sm[i].d);
    free(g_sm); g_sm = NULL; g_sm_cap = g_sm_used = 0;
}

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00300000u
#define CMD_SIZE  0x00300000u
static u32 host_alloc(u32 size, u32 align);
static void host_w32(u32 ea, u32 v);
/* ======================= the replay driver ======================= */
unsigned rsx_live_draw_report_value(unsigned off, unsigned dflt);
static int g_gen_mode;
static int is1(const char* e) { return e && *e == '1'; }

typedef struct { uint32_t space, addr, len; } rng_t;
static cap_t C;
static size_t g_cur;                    /* next capture record not yet matched or injected */
static int g_in_inject, g_adv, g_replaying;
static uint64_t* g_key;                 /* ev_key of every capture record */
static int32_t* g_draw;                 /* draw-end ordinal a RD_RENDERER record is attributed to (-1 = none) */
static int32_t* g_src;                  /* record that holds the bytes of a RD_RENDERER record (itself or the last same-range one) */
static int32_t* g_draw_cmd;             /* draw-end ordinal of a CMD record, else -1 */
static rng_t* g_inst; static size_t g_ninst, g_capinst;
static uint32_t g_epoch;
static unsigned g_diverged, g_extra, g_injected, g_hazards, g_prefix, g_report_miss, g_async_ref_empty, g_open_pass;
static unsigned g_haz_cause[8];         /* 1 get, 2 label, 3 ref, 4 flip */
static char g_haz_first[8][160];
static unsigned g_haz_n;
static const char* const k_cause[] = { "none", "get publication", "label write", "ref publication", "flip" };
static FILE* g_hazf;                    /* LD_FIFO_REPLAY_HAZ=1 (adv): every hazard as one TSV row in OUT/hazards.tsv (pass 157) */
static int32_t* g_draw_end_rec;         /* capture record of each draw end's install (ordinal -> record) */
static size_t* g_pois_rec; static int* g_pois_cause; static size_t g_npois;   /* poisoning events: record index (g_cur at the event), cause */
static int g_cause_now;                 /* last poison cause (for classification) */

/* A ref publication (WORD tag 6) is the walker's own only right after its pass-end get word (tag 4: gcm_ref_publish_one at the end
 * of cellGcm_rsx_process_fifo). Every other one comes from a guest thread polling the ref register (the paced read-driven
 * publisher) or the present-thread ticker: asynchronous to the walker, so the replay calls gcm_ref_publish_one at that
 * position (pass 156). */
static int ref_pub_async(const crec* r)
{
    const size_t i = (size_t)(r - C.r);
    if (i == 0) return 1;
    const crec* q = r - 1;
    return !(q->type == RSX_FC_WORD && rsx_fc_get32(q->p) == 1 && rsx_fc_get32(q->p + 4) == 4);
}
static int is_input(const crec* r)
{
    switch (r->type) {
    case RSX_FC_RANGE: { const uint32_t k = rsx_fc_get32(r->p); return k == RSX_FC_RD_RENDERER || k == RSX_FC_WR_RENDERER; }
    case RSX_FC_WORD: {
        const uint32_t t = rsx_fc_get32(r->p + 4);
        if (t == 6 && rsx_fc_get32(r->p) == 1) return is1(getenv("LD_REPLAY_OLD_REF")) ? 0 : ref_pub_async(r);
        return t >= 11;
    }
    case RSX_FC_SPU_NOTE: return rsx_fc_get32(r->p + 12) != 0;   /* async 0 = the walker's own NV0039 note */
    case RSX_FC_DRAIN: return 1;
    default: return 0;
    }
}
static int event_cause(const crec* r)    /* what a matched walker record publishes to the guest (adversarial poison) */
{
    if (r->type == RSX_FC_FLIP) return 4;
    if (r->type == RSX_FC_WORD && rsx_fc_get32(r->p) == 1) {
        const uint32_t t = rsx_fc_get32(r->p + 4);
        if (t == 1 || t == 2 || t == 4) return 1;
        if (t == 3 || t == 9) return 2;
        if (t == 6) return 3;
    }
    return 0;
}
static void inst_add(uint32_t space, uint32_t addr, uint32_t len)
{
    for (size_t i = 0; i < g_ninst; i++) if (g_inst[i].space == space && g_inst[i].addr == addr && g_inst[i].len == len) return;
    if (g_ninst == g_capinst) { g_capinst = g_capinst ? g_capinst * 2 : 256; g_inst = (rng_t*)realloc(g_inst, g_capinst * sizeof(rng_t)); }
    g_inst[g_ninst++] = (rng_t){ space, addr, len };
}
static void install_rec(size_t i)       /* put the bytes a RD_RENDERER record saw into A */
{
    const int32_t s = g_src[i];
    if (s < 0) return;
    const uint32_t space = rsx_fc_get32(C.r[i].p + 4), addr = rsx_fc_get32(C.r[i].p + 8), len = rsx_fc_get32(C.r[i].p + 12);
    sm_write(space, addr, C.r[s].p + RSX_FC_RANGE_HEAD, len);
    inst_add(space, addr, len);
}
static void poison_all(int cause)
{
    g_epoch++;
    if (g_hazf) { g_pois_rec = (size_t*)realloc(g_pois_rec, (g_npois + 1) * sizeof(size_t)); g_pois_cause = (int*)realloc(g_pois_cause, (g_npois + 1) * sizeof(int)); g_pois_rec[g_npois] = g_cur; g_pois_cause[g_npois++] = cause; }
    for (size_t i = 0; i < g_ninst; i++) {
        uint8_t* tmp = (uint8_t*)malloc(g_inst[i].len);
        for (uint32_t k = 0; k < g_inst[i].len; k++) tmp[k] = (uint8_t)((g_inst[i].addr + k) * 0x9Eu + g_epoch * 0x35u + 0xC3u);
        sm_write(g_inst[i].space, g_inst[i].addr, tmp, g_inst[i].len);
        free(tmp);
    }
    g_cause_now = cause;
}
static void inject(size_t i)
{
    const crec* r = &C.r[i];
    if (s_fc_state != 1) return;
    g_injected++;
#ifdef RSX_PUB_H
    rsx_pub_drain();   /* patch 2197: an injected renderer / guest event is ordered after every record the walker made so far (a held P record retires first) */
#endif
    if (r->type == RSX_FC_RANGE && rsx_fc_get32(r->p) == RSX_FC_RD_RENDERER && !(rsx_fc_get32(r->p + 16) & RSX_FC_F_NULLPTR) && g_src[i] >= 0) {
        const uint32_t space = rsx_fc_get32(r->p + 4), addr = rsx_fc_get32(r->p + 8), len = rsx_fc_get32(r->p + 12);
        if (!g_adv || g_draw[i] < 0) install_rec(i);       /* current path, no adversary: A holds what the guest had at the read */
        uint8_t* tmp = (uint8_t*)malloc(len);
        sm_read(space, addr, tmp, len);
        const uint64_t dig = rsx_fc_fnv(RSX_FC_FNV_SEED, tmp, len);
        if (dig != rsx_fc_get64(r->p + 24)) {
            g_hazards++;
            if (g_cause_now) g_haz_cause[g_cause_now]++;
            if (g_hazf) {      /* the first poisoning event after this read's draw end is its real cause (g_cause_now is only the latest) */
                const size_t de = (size_t)g_draw_end_rec[g_draw[i]]; size_t k = 0;
                while (k < g_npois && g_pois_rec[k] <= de) k++;
                fprintf(g_hazf, "%zu\t%u\t%u\t0x%X\t0x%X\t%d\t%zu\t%s\t%zu\t%s\t%zu\n", i, rsx_fc_get32(r->p + 20), space, addr, len, g_draw[i], de,
                        k < g_npois ? k_cause[g_pois_cause[k]] : "none", k < g_npois ? g_pois_rec[k] : 0, k_cause[g_cause_now], g_npois ? g_pois_rec[g_npois - 1] : 0);
            }
            if (g_haz_n < 8) {
                snprintf(g_haz_first[g_haz_n], sizeof g_haz_first[0], "record %zu: %s 0x%X+0x%X read after %s (inside draw end: %u)", i,
                         space == 1 ? "local" : "main", addr, len, k_cause[g_cause_now], rsx_fc_get32(r->p + 20));
                g_haz_n++;
            }
        }
        if (g_rsx_fp_on && rsx_fc_get32(r->p + 20) == 1) rsx_fp_snap_tex(g_rsx_fp, space == 2u ? 1u : 0u, addr, len, NULL);   /* patch 2172: a draw's read as a span under the generation rule */
        s_fc.range_tag = rsx_fc_get32(r->p + 20);
        rsx_fc_w_range(&s_fc, RSX_FC_RD_RENDERER, space, addr, tmp, len, 1);
        s_fc.range_tag = 0;
        free(tmp);
        return;
    }
    if (r->type == RSX_FC_WORD && rsx_fc_get32(r->p) == 1 && rsx_fc_get32(r->p + 4) == 6) {   /* asynchronous ref publication */
        const unsigned before = (unsigned)g_gcm_ref_pub_count;
        ReleaseSRWLockExclusive(&s_fc_lock);       /* the hook runs under the writer lock; rsx_fcap_word takes it again */
        gcm_ref_publish_one();                     /* emits the tag 6 word itself, from the walker's own queue */
        AcquireSRWLockExclusive(&s_fc_lock);
        if ((unsigned)g_gcm_ref_pub_count == before) g_async_ref_empty++;   /* the queue was empty: the capture's word has no counterpart */
        if (g_adv) poison_all(3);
        return;
    }
    if (g_rsx_fp_on) {   /* patch 2172: renderer write-backs and SPU / PPU notes bump the write generations */
        if (r->type == RSX_FC_RANGE && rsx_fc_get32(r->p) == RSX_FC_WR_RENDERER) rsx_fp_note_write(g_rsx_fp, rsx_fc_get32(r->p + 4) == 2u ? 1u : 0u, rsx_fc_get32(r->p + 8), rsx_fc_get32(r->p + 12));
        if (r->type == RSX_FC_SPU_NOTE) rsx_fp_note_write(g_rsx_fp, rsx_fc_get32(r->p), rsx_fc_get32(r->p + 4), rsx_fc_get32(r->p + 8));
    }
    rsx_fc_w_rec(&s_fc, r->type, r->p, r->len);       /* echo: write-backs, notes, drains, report values, ring callbacks */
    if (r->type == RSX_FC_WORD && rsx_fc_get32(r->p) == 1) {
        const uint32_t t = rsx_fc_get32(r->p + 4);
        if (t == 12 || t == 13 || t == 14) vm_write32(rsx_fc_get32(r->p + 8), rsx_fc_get32(r->p + 12));   /* the guest's ring/context write */
    }
}
static void flush_inputs(void)
{
    g_in_inject++;
    while (g_cur < C.n) {
        const crec* r = &C.r[g_cur];
        if (is_structural(r->type) || is_annot_head(r->type, r->p)) { g_cur++; continue; }
        if (!is_input(r)) break;
        inject(g_cur); g_cur++;
    }
    g_in_inject--;
}
/* The ZPASS value the live renderer returns for a GET_REPORT (rsx_live_draw_report_value) is a renderer INPUT to the walker. The
 * replay has no renderer; it takes the value from the capture: the next walker WORD record (tag 5) that wrote this report's
 * value word (ea = report ea + 8). Pass 156: without it the harness answered 0 and every report diverged. */
unsigned rsx_live_draw_report_value(unsigned off, unsigned dflt)
{
    if (g_gen_mode) return 0xA5A50000u | (off & 0xFFFFu);      /* the stub renderer's own answer while the capture is made */
    if (g_replaying && g_rsx_fp_on) rsx_fp_report(g_rsx_fp, RSX_FP_RC_DFLT, -1, off, 0, 0);   /* patch 2172: count and position only (no GPU: class, has_val and lag are model-only here) */
    if (!g_replaying || is1(getenv("LD_REPLAY_OLD_REPORT"))) return dflt;
    const u32 want = gcm_io2ea(off) + 8u;
    for (size_t i = g_cur; i < C.n && i < g_cur + 64; i++)
        if (C.r[i].type == RSX_FC_WORD && rsx_fc_get32(C.r[i].p) == 1 && rsx_fc_get32(C.r[i].p + 4) == 5 && rsx_fc_get32(C.r[i].p + 8) == want)
            return rsx_fc_get32(C.r[i].p + 12);
    if (s_fc_state != 1) return dflt;      /* after the window closed (the unterminated last pass keeps walking): nothing to match */
    if (g_report_miss++ < 3) fprintf(stderr, "[replay] report miss: off 0x%X ea 0x%X g_cur %zu\n", off, want, g_cur);
    return dflt;
}
/* The NV0039 write-back count (rsx_live_draw_writeback_range: how many surface bytes/pages the renderer wrote back) is the second
 * renderer INPUT to the walker, recorded as word 8 of the COPY record (kind 0). Pass 157: the stub answered 4096 while replaying,
 * the live renderer had answered 1, so the re-emitted COPY record differed (play-through capture records 23024 and 68025). Taken from the
 * next NV0039 COPY record of the capture, like the report value. LD_REPLAY_OLD_WB=1 puts the 4096 answer back (control). */
static unsigned g_wb_miss;
u32 rsx_live_draw_writeback_range(u32 l, u32 o, u32 n)
{
    (void)l; (void)o; (void)n;
    if (g_gen_mode) return 1;                    /* the stub renderer's answer while the capture is made (the live one answered 1 in a play-through, not the old stub's 4096) */
    if (!g_replaying || is1(getenv("LD_REPLAY_OLD_WB"))) return 4096;
    for (size_t i = g_cur; i < C.n && i < g_cur + 64; i++)
        if (C.r[i].type == RSX_FC_COPY && rsx_fc_get32(C.r[i].p) == 0 && C.r[i].len >= 36) return rsx_fc_get32(C.r[i].p + 32);
    if (s_fc_state == 1) g_wb_miss++;
    return 4096;
}
static void replay_hook(uint32_t type, const void* head, uint32_t hlen, uint32_t tlen)
{
    (void)tlen;
    if (g_in_inject || !g_replaying || is_structural(type) || is_annot_head(type, (const uint8_t*)head)) return;
    const uint64_t k = ev_key(type, (const uint8_t*)head, hlen);
    while (g_cur < C.n && (is_structural(C.r[g_cur].type) || is_annot_head(C.r[g_cur].type, C.r[g_cur].p) || is_input(&C.r[g_cur]))) {   /* inputs missed before this record */
        g_in_inject++; if (!is_annot_head(C.r[g_cur].type, C.r[g_cur].p) && is_input(&C.r[g_cur])) inject(g_cur); g_in_inject--; g_cur++;
    }
    if (g_cur >= C.n) { g_extra++; return; }
    if (g_key[g_cur] != k) { if (g_diverged == 0) {
            fprintf(stderr, "[replay] first divergence at capture record %zu (type %u), the path emitted type %u\n  capture:", g_cur, C.r[g_cur].type, type);
            for (uint32_t w = 0; w < 8 && w * 4 + 4 <= C.r[g_cur].len; w++) fprintf(stderr, " %X", rsx_fc_get32(C.r[g_cur].p + w * 4));
            fprintf(stderr, "\n  path   :");
            for (uint32_t w = 0; w < 8 && w * 4 + 4 <= hlen; w++) fprintf(stderr, " %X", rsx_fc_get32((const uint8_t*)head + w * 4));
            fprintf(stderr, "\n");
        }
        g_diverged++; }
    const crec* m = &C.r[g_cur];
    if (g_adv && g_draw_cmd[g_cur] >= 0)       /* the draw end: this draw's reads become the guest's state at its FIFO position */
        { g_draw_end_rec[g_draw_cmd[g_cur]] = (int32_t)g_cur; for (size_t j = 0; j < C.n; j++) if (g_draw[j] == g_draw_cmd[g_cur]) install_rec(j); }
    const int cause = event_cause(m);
    g_cur++;
    if (g_rsx_fp_on && type == RSX_FC_FLIP) rsx_sync_hist_event(RSX_SH_FLIP, 0);   /* patch 2172: the flip the walker re-emits ends a probe flip (no LD present here) */
    if (g_adv && cause) poison_all(cause);
    flush_inputs();
}

static void apply_cmd(const crec* r)
{
    const uint32_t off = rsx_fc_get32(r->p), hdr = rsx_fc_get32(r->p + 4), n = rsx_fc_get32(r->p + 8);
    if (hdr == 0) { const uint32_t w = rsx_fc_get32(r->p + 12); for (uint32_t i = 0; i < w; i++) { const u32 ea = gcm_io2ea(off + i * 4); if (ea) vm_write32(ea, 0); } return; }
    u32 ea = gcm_io2ea(off);
    if (ea) vm_write32(ea, hdr);
    for (uint32_t i = 0; i < n; i++) { ea = gcm_io2ea(off + 4 + i * 4); if (ea) vm_write32(ea, rsx_fc_get32(r->p + 12 + i * 4)); }
}

/* Pass 156: the guest address layout comes from the CONFIG record. ppu_hle_inject_base (the label page at +0, the control page
 * at +0x2000) is assigned by the game's boot code, 0x7F000000 in a real capture and 0x20000000 in the test default; the walker
 * addresses labels and the control register through it, so it must be set BEFORE cellGcmSetupContext and the guest memory
 * must reach it (word 2 = the capture's vm size). Rounds 155's harness took the default and wrote labels outside its memory. */
static uint64_t g_vm_len;
static int vm_map(uint64_t len)
{
    void* m = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);   /* lazily committed */
    if (m == MAP_FAILED) { fprintf(stderr, "[replay] cannot reserve 0x%llX bytes of guest memory\n", (unsigned long long)len); return 1; }
    vm_base = (uint8_t*)m; g_vm_len = len;
    return 0;
}
/* Negative controls (pass 156): LD_REPLAY_OLD_VM / _PREFIX / _REF / _REPORT put back what the pass 155 harness did, so the test
 * can show each real-capture failure comes back without its fix. They are test switches, never set outside test_log_replay.sh. */
static int replay_vm_from_config(void)
{
    const crec* cfg = NULL;
    for (size_t i = 0; i < C.n; i++) if (C.r[i].type == RSX_FC_CONFIG) { cfg = &C.r[i]; break; }
    if (!cfg || cfg->len < 36) { fprintf(stderr, "[replay] no usable CONFIG record\n"); return 1; }
    const uint32_t vmsz = rsx_fc_get32(cfg->p + 8), lab = rsx_fc_get32(cfg->p + 24), ctl = rsx_fc_get32(cfg->p + 28);
    /* word 2 is (u32)(ppu_vm_size ? ppu_vm_size : VM_TOTAL_SIZE): 0 = the full 4 GiB reservation (the game never sets ppu_vm_size) */
    g_vm_len = vmsz ? vmsz : 0x100000000ull;
    if (is1(getenv("LD_REPLAY_OLD_VM"))) {     /* pass 155: the toolkit's default base and a memory of base + 64 KiB, whatever the capture says */
        const uint32_t def = ppu_hle_inject_base;
        vm_base = (uint8_t*)calloc(1, def + 0x10000u); ppu_vm_size = def + 0x10000u; g_vm_len = def + 0x10000u;
        return 0;
    }
    if (ctl != lab + 0x2000u || g_vm_len < (uint64_t)lab + 0x10000u) { fprintf(stderr, "[replay] CONFIG layout unexpected: vm size 0x%X labels 0x%X control 0x%X\n", vmsz, lab, ctl); return 1; }
    ppu_hle_inject_base = lab;
    if (vm_map(g_vm_len)) return 1;
    ppu_vm_size = vmsz;
    return 0;
}
/* Replays C. out_dir gets the re-capture. Returns 0 clean, 1 divergence/extra/left over. */
/* Patch 2190: LD_REPLAY_MEMDIGEST=file writes one line "page-address fnv1a64" per non-zero 4 KB page of the guest memory at the end of
 * a replay (test_guest_mem_diff.sh compares the digest of the pre-funnel tree with the funnel's). The report / notify timestamp
 * words (WORD tags 5 and 10 whose ea has bit 3 clear, ps3_ms_now) are zeroed first: they are wall clock. */
static void memdigest_dump(void)
{
    const char* path = getenv("LD_REPLAY_MEMDIGEST");
    if (!path || !*path || !vm_base) return;
    uint8_t* tmp = (uint8_t*)malloc(4096);
    FILE* f = fopen(path, "w");
    if (!f || !tmp) { fprintf(stderr, "[replay] LD_REPLAY_MEMDIGEST: cannot open %s\n", path); free(tmp); if (f) fclose(f); return; }
    static const uint8_t z[4096];
    size_t nmask = 0, cmask = 0; uint32_t* mask = NULL;
    for (size_t i = 0; i < C.n; i++)
        if (C.r[i].type == RSX_FC_WORD && rsx_fc_get32(C.r[i].p) == 1) {
            const uint32_t t = rsx_fc_get32(C.r[i].p + 4), ea = rsx_fc_get32(C.r[i].p + 8);
            if ((t == 5 || t == 10) && (ea & 8u) == 0) {
                if (nmask == cmask) { cmask = cmask ? cmask * 2 : 256; mask = (uint32_t*)realloc(mask, cmask * 4); }
                mask[nmask++] = ea;
            }
        }
    unsigned pages = 0;
    for (uint64_t a = 0; a < g_vm_len; a += 4096) {
        memcpy(tmp, vm_base + a, 4096);
        for (size_t m = 0; m < nmask; m++) if ((uint64_t)mask[m] >= a && (uint64_t)mask[m] + 4 <= a + 4096) memset(tmp + (mask[m] - a), 0, 4);
        if (!memcmp(tmp, z, 4096)) continue;
        fprintf(f, "%llx %016llx\n", (unsigned long long)a, (unsigned long long)rsx_fc_fnv(RSX_FC_FNV_SEED, tmp, 4096));
        pages++;
    }
    fclose(f); free(tmp); free(mask);
    printf("[replay] memory digest: %u non-zero pages -> %s\n", pages, path);
}
#ifdef RSX_GUEST_SIGNAL_H
/* patch 2190: the funnel's counters of this replay, "[replay-gs] kind=on/offr ..." */
static void gs_report(void)
{
    printf("[replay-gs]");
    for (unsigned k = 1; k < RSX_GS_N; k++) if (g_rsx_gs_n[k][0] || g_rsx_gs_n[k][1]) printf(" %u=%llu/%llu", k, g_rsx_gs_n[k][0], g_rsx_gs_n[k][1]);
    printf("\n");
}
#else
static void gs_report(void) {}
#endif
#ifdef RSX_PUB_H
/* patch 2197: LD_REPLAY_PUBTRACE=file writes "seq kind ea val stream_off" per walker-thread guest signal at its store (the retire with RSX_PUB=1|verify,
 * the direct store with RSX_PUB unset or 0). The report / notify timestamp words (WORD tags 5 and 10 with bit 3 of the ea clear) are wall clock and
 * written as 0, like the memory digest does. RSX_PUB and RSX_PUB_LAG are read from the environment by the walker's first pass. */
static FILE* g_pubtrace;
static void pubtrace_cb(uint64_t seq, unsigned kind, uint32_t ea, uint32_t val, uint32_t so)
{
    if (!g_pubtrace) return;
    if ((kind == 5u || kind == 10u) && (ea & 8u) == 0) val = 0;
    fprintf(g_pubtrace, "%llu %u %08x %08x %08x\n", (unsigned long long)seq, kind, ea, val, so);
}
static void pub_trace_open(void)
{
    const char* p = getenv("LD_REPLAY_PUBTRACE");
    if (!p || !*p) return;
    g_pubtrace = fopen(p, "w");
    if (g_pubtrace) g_rsx_pub_trace = pubtrace_cb; else fprintf(stderr, "[replay] LD_REPLAY_PUBTRACE: cannot open %s\n", p);
}
static void pub_report(void)
{
    rsx_pub_stats s; rsx_pub_stats_snapshot(&s);
    printf("[replay-pub] mode=%d lag=%u rec=%llu retired=%llu stores=%llu barriers=%llu held_max=%llu bad=%llu empty_bad=%llu owner_bad=%llu rep=%llu repcalc=%llu pending=%u passes=%llu\n",
           rsx_pub_mode(), rsx_pub_lag(), s.rec, s.retired, s.stores, s.barriers, s.held_max, s.bad, s.empty_bad, s.owner_bad, s.rep, s.repcalc, rsx_pub_pending(), s.passes);
    g_rsx_pub_trace = NULL;
    if (g_pubtrace) { fclose(g_pubtrace); g_pubtrace = NULL; }
}
#else
static void pub_trace_open(void) {}
static void pub_report(void) {}
#endif
static int replay_run(const char* out_dir, int adv)
{
    int rc = 0;
    g_adv = adv;
    pub_trace_open();
    if (adv && is1(getenv("LD_FIFO_REPLAY_HAZ"))) { char hp[760]; snprintf(hp, sizeof hp, "%s/hazards.tsv", out_dir); g_hazf = fopen(hp, "w"); }
    const size_t n = C.n;
    g_key = (uint64_t*)calloc(n + 1, sizeof(uint64_t)); g_draw = (int32_t*)malloc((n + 1) * 4); g_src = (int32_t*)malloc((n + 1) * 4);
    g_draw_cmd = (int32_t*)malloc((n + 1) * 4); g_draw_end_rec = (int32_t*)calloc((n + 1), 4);
    int32_t cur_draw = -1, ndraw = 0;
    for (size_t i = 0; i < n; i++) {
        g_key[i] = ev_key(C.r[i].type, C.r[i].p, C.r[i].len); g_draw[i] = -1; g_src[i] = -1; g_draw_cmd[i] = -1;
        if (C.r[i].type == RSX_FC_CMD && rsx_fc_get32(C.r[i].p + 4) != 0) {   /* draw end = a command that writes 0x1808 with 0 */
            const uint32_t hdr = rsx_fc_get32(C.r[i].p + 4), cnt = rsx_fc_get32(C.r[i].p + 8);
            const uint32_t method = hdr & 0x1FFCu, subch = (hdr >> 13) & 7u, noninc = (hdr >> 29) == 2u;
            for (uint32_t q = 0; q < cnt; q++)
                if ((subch << 13 | (noninc ? method : method + q * 4u)) == 0x1808u && rsx_fc_get32(C.r[i].p + 12 + q * 4) == 0) { g_draw_cmd[i] = ndraw; cur_draw = ndraw++; break; }
        }
        if (C.r[i].type == RSX_FC_RANGE && rsx_fc_get32(C.r[i].p) == RSX_FC_RD_RENDERER) g_draw[i] = cur_draw;
    }
    for (size_t i = 0; i < n; i++) {        /* bytes source of each renderer read: itself, or the last record of the same range that had them */
        if (!(C.r[i].type == RSX_FC_RANGE && rsx_fc_get32(C.r[i].p) == RSX_FC_RD_RENDERER)) continue;
        if (rsx_fc_get32(C.r[i].p + 16) & RSX_FC_F_BYTES) { g_src[i] = (int32_t)i; continue; }
        for (size_t j = i; j-- > 0;)
            if (C.r[j].type == RSX_FC_RANGE && rsx_fc_get32(C.r[j].p) == RSX_FC_RD_RENDERER && (rsx_fc_get32(C.r[j].p + 16) & RSX_FC_F_BYTES) &&
                !memcmp(C.r[j].p + 4, C.r[i].p + 4, 12) && rsx_fc_get64(C.r[j].p + 24) == rsx_fc_get64(C.r[i].p + 24)) { g_src[i] = (int32_t)j; break; }
    }
    /* walker state from the CONFIG record, memory from the IMAGE records */
    const crec* cfg = NULL;
    for (size_t i = 0; i < n; i++) if (C.r[i].type == RSX_FC_CONFIG) { cfg = &C.r[i]; break; }
    if (!cfg) { fprintf(stderr, "[replay] no CONFIG record\n"); return 1; }
    { u32 ctx_out = host_alloc(4, 4); (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32); }   /* walker state; the tables below come from CONFIG */
    memset(s_ea_address_table, 0xFF, sizeof s_ea_address_table); memset(s_io_address_table, 0xFF, sizeof s_io_address_table);
    s_config.localAddress = rsx_fc_get32(cfg->p); s_config.localSize = rsx_fc_get32(cfg->p + 4);
    const uint32_t n_io = rsx_fc_get32(cfg->p + 32);
    for (uint32_t i = 0; i < n_io; i++) { const uint32_t v = rsx_fc_get32(cfg->p + 36 + i * 4); s_ea_address_table[v >> 16] = (u16)(v & 0xFFFF); s_io_address_table[v & 0xFFFF] = (u16)(v >> 16); }
    for (size_t i = 0; i < n; i++)
        if (C.r[i].type == RSX_FC_IMAGE) { const uint32_t a = rsx_fc_get32(C.r[i].p + 4), l = rsx_fc_get32(C.r[i].p + 8); if ((uint64_t)a + l > g_vm_len) { fprintf(stderr, "[replay] IMAGE 0x%X+0x%X outside the guest memory 0x%llX\n", a, l, (unsigned long long)g_vm_len); return 1; } memcpy(vm_base + a, C.r[i].p + 12, l); }
    /* the walker's frame counter starts where the capture's window did, so the re-capture window opens at once */
    char dir[700]; snprintf(dir, sizeof dir, "%s", out_dir);
    setenv("LD_FIFO_CAPTURE", dir, 1);
    { char fr[64]; snprintf(fr, sizeof fr, "%u,%u", C.frame_start, C.frame_count); setenv("LD_FIFO_CAPTURE_FRAME", fr, 1); }
    (void)rsx_fcap_configured();
    s_fc_frames = C.frame_start;
    s_fifo_getoff = rsx_fc_get32(cfg->p + 16); s_fifo_calloff = rsx_fc_get32(cfg->p + 20);
    rsx_fc_hook = replay_hook;
    AcquireSRWLockExclusive(&s_fc_lock); gcm_fcap_update_locked(); ReleaseSRWLockExclusive(&s_fc_lock);
    if (s_fc_state != 1) { fprintf(stderr, "[replay] the re-capture did not open\n"); return 1; }
    g_replaying = 1;
    /* Pass 156: a real window opens in the middle of a pass. Walker records before the first PASS record (the tail of the pass
     * that was running: its get publication, PASS_END) have no commands the walker could re-run; they are counted as the
     * PREFIX and reported, never matched. Inputs between them (renderer reads, write-backs) are still injected. */
    {
        size_t first_pass = n;
        for (size_t i = 0; i < n; i++) if (C.r[i].type == RSX_FC_PASS) { first_pass = i; break; }
        if (is1(getenv("LD_REPLAY_OLD_PREFIX"))) first_pass = 0;      /* pass 155: every record is matched against the walker */
        AcquireSRWLockExclusive(&s_fc_lock);
        g_in_inject++;
        for (; g_cur < first_pass; g_cur++) {
            if (is_structural(C.r[g_cur].type) || is_annot_head(C.r[g_cur].type, C.r[g_cur].p)) continue;
            if (is_input(&C.r[g_cur])) inject(g_cur); else g_prefix++;
        }
        g_in_inject--;
        flush_inputs();
        ReleaseSRWLockExclusive(&s_fc_lock);
    }
    for (size_t i = 0; i < n; i++) {
        if (C.r[i].type != RSX_FC_PASS) continue;
        const uint32_t put = rsx_fc_get32(C.r[i].p), getoff = rsx_fc_get32(C.r[i].p + 4), calloff = rsx_fc_get32(C.r[i].p + 8);
        uint32_t end_get = getoff;
        size_t j = i + 1;
        for (; j < n && C.r[j].type != RSX_FC_PASS && C.r[j].type != RSX_FC_PASS_END; j++) ;
        const int terminated = j < n && C.r[j].type == RSX_FC_PASS_END;
        if (terminated) end_get = rsx_fc_get32(C.r[j].p);
        for (size_t q = i + 1; q < j; q++) {            /* what the walker finds in guest memory in this pass */
            const crec* r = &C.r[q];
            if (r->type == RSX_FC_CMD) apply_cmd(r);
            else if (r->type == RSX_FC_RANGE && rsx_fc_get32(r->p) == RSX_FC_RD_WALKER && (rsx_fc_get32(r->p + 16) & RSX_FC_F_BYTES)) {
                const uint32_t a = rsx_fc_get32(r->p + 8), l = rsx_fc_get32(r->p + 12);
                if ((uint64_t)a + l > g_vm_len) { fprintf(stderr, "[replay] walker RANGE 0x%X+0x%X outside the guest memory 0x%llX\n", a, l, (unsigned long long)g_vm_len); return 1; }
                memcpy(vm_base + a, r->p + RSX_FC_RANGE_HEAD, l);
            } else if (r->type == RSX_FC_WORD && rsx_fc_get32(r->p) == 0) vm_write32(rsx_fc_get32(r->p + 8), rsx_fc_get32(r->p + 12));
        }
        s_fifo_getoff = getoff; s_fifo_calloff = calloff;
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
        for (int tries = 0; tries < 8; tries++) { cellGcm_rsx_process_fifo(); if (terminated && s_fifo_getoff == end_get) break; if (!terminated) break; }
        if (!terminated) g_open_pass++;      /* the window closed inside this pass: no end get to check, the record match is the check */
        else if (s_fifo_getoff != end_get) { fprintf(stderr, "[replay] pass at capture record %zu stopped at 0x%X, the capture's pass ended at 0x%X\n", i, s_fifo_getoff, end_get); rc = 1; }
    }
    AcquireSRWLockExclusive(&s_fc_lock); flush_inputs(); ReleaseSRWLockExclusive(&s_fc_lock);   /* trailing inputs (a ring callback after the last pass) */
    g_replaying = 0;
    gcm_fcap_atexit();
    rsx_fc_hook = NULL;
    {   /* patch 2197: the direct (default-off) H-store of the funnel, which no replayed record reaches (the report writes come from the live renderer):
         * called here off the walker thread, with RSX_PUB whatever it is, it must store the value big-endian; test_pub_replay.sh checks the line */
        extern void gcm_guest_signal_hostbe(unsigned kind, uint8_t* p, uint32_t tag_ea, uint32_t val);
        uint8_t hb[4] = {0xEE, 0xEE, 0xEE, 0xEE};
        gcm_guest_signal_hostbe(RSX_GS_REPORT_RB, hb, 0x1000u, 0x11223344u);
        printf("[replay-hbe] %02x%02x%02x%02x\n", hb[0], hb[1], hb[2], hb[3]);
    }
    memdigest_dump(); gs_report(); pub_report();   /* patch 2190, 2197 */
    size_t left = 0;
    for (size_t i = g_cur; i < C.n; i++) if (!is_structural(C.r[i].type) && !is_annot_head(C.r[i].type, C.r[i].p)) left++;
    printf("[replay] asynchronous ref publications whose queue was empty %u\n", g_async_ref_empty);
    printf("[replay] report values not found in the capture %u\n", g_report_miss);
    printf("[replay] NV0039 write-back counts not found in the capture %u\n", g_wb_miss);
    printf("[replay] passes the window closed inside (no PASS_END) %u\n", g_open_pass);
    printf("[replay] prefix %u (walker records of the pass that was running when the window opened, not replayable)\n", g_prefix);
    printf("[replay] records %zu injected %u diverged %u extra %u left %zu hazards %u (adversarial %d)\n", C.n, g_injected, g_diverged, g_extra, left, g_hazards, adv);
    printf("[replay-lgs] m3d=%llu ran=%llu skipped=%llu readers=%llu\n", s_lgs.m3d, s_lgs.ran, s_lgs.skipped, s_lgs.readers);   /* patch 2189 */
    for (unsigned i = 1; i <= 4; i++) if (g_haz_cause[i]) printf("[replay] hazard: %u read(s) after %s\n", g_haz_cause[i], k_cause[i]);
    for (unsigned i = 0; i < g_haz_n; i++) printf("[replay]   %s\n", g_haz_first[i]);
    if (g_diverged || g_extra || left) rc = 1;
    if (g_hazf) { fclose(g_hazf); g_hazf = NULL; } free(g_pois_rec); free(g_pois_cause); g_pois_rec = NULL; g_pois_cause = NULL; g_npois = 0; free(g_draw_end_rec); g_draw_end_rec = NULL;
    free(g_key); free(g_draw); free(g_src); free(g_draw_cmd); free(g_inst);
    g_key = NULL; g_draw = g_src = g_draw_cmd = NULL; g_inst = NULL; g_ninst = g_capinst = 0;
    sm_reset();
    return rc;
}

/* ======================= synthetic capture ("gen") ======================= */
#define SRC_OFF 0x100000u
#define DST_OFF 0x180000u
static uint32_t g_heap = 0x00500000u;
static u32 host_alloc(u32 size, u32 align) { if (!align) align = 4; g_heap = (g_heap + align - 1u) & ~(align - 1u); u32 r = g_heap; g_heap += size; return r; }
static void gw32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }
static uint32_t g_io;
static void emit(uint32_t hdr, int n, const uint32_t* w)
{
    gw32(IO_ADDR + g_io, hdr);
    for (int i = 0; i < n; i++) gw32(IO_ADDR + g_io + 4 + (u32)i * 4, w[i]);
    g_io += 4 + (u32)n * 4;
}
static void m1(u32 subch, u32 method, u32 v) { emit((1u << 18) | (subch << 13) | method, 1, &v); }
static void draw(int k) { for (int i = 0; i < k; i++) m1(0, 0x1808u, 0u); }
static void setreg(u32 reg, u32 v) { m1(0, reg, v); }
static int g_rel_n;
static void rel(void) { m1(0, 0x64u, 0x4000u); m1(0, 0x6Cu, 0xC0DE0001u + (u32)g_rel_n++); }
static void nv0039(void)
{
    m1(1, 0x0184u, 0xFEED0001u); m1(1, 0x0188u, 0xFEED0001u);
    m1(1, 0x030Cu, SRC_OFF); m1(1, 0x0310u, DST_OFF); m1(1, 0x0314u, 32); m1(1, 0x0318u, 32);
    m1(1, 0x031Cu, 32); m1(1, 0x0320u, 2); m1(1, 0x0324u, 0x101u); m1(1, 0x0328u, 0u);
}
static void nops(int k) { for (int i = 0; i < k; i++) { gw32(IO_ADDR + g_io, 0); g_io += 4; } }
static void gen_frame(int idx)
{
    setreg(0x1E94u, 0x100u + (u32)idx);                      /* some register state that changes per draw group */
    if ((idx & 1) == 0) {   /* A: draws, a release, a copy, a report, a ref, a release, and a release right before the flip */
        draw(3); setreg(0x1D64u, 7u); rel(); draw(2); nv0039(); draw(4);
        setreg(0x01A8u, 0xFEED0001u); m1(0, 0x1800u, (1u << 24) | 0x40u); setreg(0x1D6Cu, 0x490u); setreg(0x1D74u, 0x1FFFFEFFu);
        m1(0, 0x50u, 7u);
        draw(1); rel(); draw(5); rel(); m1(7, 0x0944u, 0x100u | 1u);
    } else {                /* B */
        setreg(0x1D64u, 9u); draw(4); rel(); draw(2); nops(5); m1(7, 0x0944u, 2u);
    }
}
/* The stub renderer's guest memory (what its reads see) and the scripted guest writes. */
static uint8_t g_main[0x10000], g_local[0x40000];
static int g_draw_k, g_pending_late;
/* Patch 2172: LD_FRONT_PROBE=1 over the replay. The harness has no dispatcher, so the surface registers are tracked here from the
 * command words (ASSUMPTION: DMA handles 0xFEED0001 / 0xBAD68000 name main memory, as the report path in cellGcmSys.c assumes), the
 * draw end is the 0x1808 word with arg 0, a draw's guest reads (RD_RENDERER records) are fed to the generation rule as spans (the capture
 * does not say texture / vertex / index / FP, so they are all "reads"), writes (WR_RENDERER, SPU/PPU notes, the NV0039 destination)
 * bump the generations. Counters only: no GPU, so has_val / latency rows are model-only here. */
static u32 g_fpr[0x2000 / 4];
static int fp_main_dma(u32 h) { return h == 0xFEED0001u || h == 0xBAD68000u; }
unsigned rsx_live_draw_note_guest_write(unsigned l, unsigned o, unsigned n) { if (g_rsx_fp_on) rsx_fp_note_write(g_rsx_fp, l, o, n); return 0; }
static void fp_replay_word(u32 method, u32 arg)
{
    const u32 m = method & 0x1FFCu;
    g_fpr[m >> 2] = arg;
    if (m == 0x1808u && arg == 0u) {   /* draw end */
        const u32 tgt = g_fpr[0x208 >> 2];
        const u32 n = tgt == 0x13 ? 2 : tgt == 0x17 ? 3 : tgt == 0x1F ? 4 : 1;
        static const u32 cr[4] = { 0x194, 0x198, 0x1B8, 0x1BC };
        int main_rt = fp_main_dma(g_fpr[0x19C >> 2]);
        for (u32 i = 0; i < n; i++) main_rt |= fp_main_dma(g_fpr[cr[i] >> 2]);
        rsx_fp_draw_rt(g_rsx_fp, main_rt);
        rsx_fp_draw_begin(g_rsx_fp, RSX_FP_CLOCK()); rsx_fp_draw_end(g_rsx_fp, RSX_FP_CLOCK());
    }
}
u32 rsx_live_draw_before_guest_sync(void)
{
    static const unsigned dirty[] = { 0, 1, 1, 1, 0, 1 };
    static unsigned i;
    if (g_replaying) return 0;
    if (g_pending_late == 1) { for (int q = 0; q < 128; q++) g_main[0x3000 + q] ^= 0x5Au; g_pending_late = 2; }   /* the guest rewrote the deferred read's data right after the release */
    rsx_fcap_range(3 /* WR_RENDERER */, 1, 0x20000, g_local + 0x20000, 128, 0);       /* a write-back the renderer stored */
    rsx_fcap_word(1, 11, 0x1230, 0xBEEF0000u + i);                                     /* a report value the query drain wrote */
    return dirty[i++ % 6];
}
void rsx_live_draw_method(u32 method, u32 arg)
{
    if (g_replaying && g_rsx_fp_on) fp_replay_word(method, arg);
    if (method == 0x1808u && arg == 0u) {
        const int k = g_draw_k++;
        if (!g_replaying) {
            g_rsx_fcap_in_draw = g_rsx_fcap_on;                                         /* sink_end's flag */
            const u32 off = (u32)(k % 3) * 0x400u;
            rsx_fcap_range(0, 2, off, g_main + off, 256, 1);
            if (k % 4 == 0) { const u32 lo = 0x2000u + (u32)(k % 8) * 0x100u; rsx_fcap_range(0, 1, lo, g_local + lo, 512, 1); }
            g_rsx_fcap_in_draw = 0;
            if (k == 2) rsx_fcap_spu_note(1, 0x4000, 256, 1);
            if (k == 7) rsx_fcap_spu_note(0, 0x20000, 64, 2);
            if (k == 5) g_pending_late = 1;
            if (k == 9) gcm_ref_publish_one();   /* a guest thread's read-driven publish, mid pass */
        }
    }
    if (method == 0xE944u) {                       /* flip: the deferred read happens now, outside any draw end */
        if (!g_replaying && g_pending_late) { rsx_fcap_range(0, 2, 0x3000, g_main + 0x3000, 128, 1); g_pending_late = 0; }
        if (!g_replaying) rsx_fcap_drain(1, 2, 0);
        if (g_rsx_fcap_cfg) rsx_fcap_flip(arg & 7u, 0);
    }
}

static int gen_run(const char* dir)
{
    setenv("LD_FIFO_CAPTURE", dir, 1); setenv("LD_FIFO_CAPTURE_FRAME", "0,100", 1);
    g_gen_mode = 1;
    ppu_hle_inject_base = 0x7F000000u;      /* the game's value (a real capture's CONFIG says so), not the toolkit default 0x20000000 */
    if (vm_map(0x100000000ull)) return 1;   /* the game's full 4 GiB reservation: CONFIG then carries vm size 0 */
    ppu_vm_size = 0;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    for (int i = 0; i < 64; i++) vm_base[IO_ADDR + SRC_OFF + i] = (uint8_t)(0x40 + i);
    for (size_t i = 0; i < sizeof g_main; i++) g_main[i] = (uint8_t)(i * 7u + (i >> 8));
    for (size_t i = 0; i < sizeof g_local; i++) g_local[i] = (uint8_t)(i * 13u + (i >> 9) + 1u);
    for (int f = 0; f < 4; f++) gen_frame(f);
    s_fifo_getoff = 0; vm_write32(ctrl + 4, 0); vm_write32(ctrl + 0, g_io);
    for (int pass = 0; pass < 400 && s_fifo_getoff < g_io; pass++) cellGcm_rsx_process_fifo();
    if (s_fifo_getoff < g_io) { puts("FAIL: gen walked only part of the stream"); return 1; }
    /* a ring-callback write from the guest side (gap 3): a segment-next call on a made-up context */
    s_ring_begin = 0x00200000u; s_ring_end = 0x00280000u; s_seg_size = 0x10000u;
    { const u32 ctx = host_alloc(16, 4); gw32(ctx + 8, 0x00200100u); gw32(ctx + 4, 0x0020FFFCu); gw32(ctx, 0x00200000u);
      gcm_fifo_next_segment(ctx); }
    gcm_fcap_atexit();
    printf("[gen] ok: cmds walked to 0x%X, draws %d\n", g_io, g_draw_k);
    return 0;
}

/* ======================= mutations (negative controls) ======================= */
static int mutate_run(const char* in, const char* out, const char* kind)
{
    cap_t c; if (cap_load(&c, in)) { puts("FAIL: mutate: cannot load input"); return 1; }
    int a = -1, b = -1;
    if (!strcmp(kind, "drop-word")) { for (size_t i = 0; i < c.n; i++) if (c.r[i].type == RSX_FC_WORD && rsx_fc_get32(c.r[i].p + 4) == 3) { a = (int)i; break; } }
    else if (!strcmp(kind, "flip-range")) { for (size_t i = 0; i < c.n; i++) if (c.r[i].type == RSX_FC_RANGE && (rsx_fc_get32(c.r[i].p + 16) & RSX_FC_F_BYTES) && rsx_fc_get32(c.r[i].p) == RSX_FC_RD_RENDERER) { a = (int)i; break; } }
    else if (!strcmp(kind, "swap-words")) {   /* the first two consecutive walker WORD writes */
        for (size_t i = 0; i + 1 < c.n; i++) if (c.r[i].type == RSX_FC_WORD && c.r[i + 1].type == RSX_FC_WORD && rsx_fc_get32(c.r[i].p + 4) != rsx_fc_get32(c.r[i + 1].p + 4) && rsx_fc_get32(c.r[i].p + 4) < 11) { a = (int)i; b = (int)i + 1; break; } }
    else if (!strcmp(kind, "swap-cmds")) {    /* two consecutive method commands */
        for (size_t i = 0; i + 1 < c.n; i++) if (c.r[i].type == RSX_FC_CMD && c.r[i + 1].type == RSX_FC_CMD && c.r[i].len > 12 && c.r[i + 1].len > 12 &&
                                                    memcmp(c.r[i].p + 4, c.r[i + 1].p + 4, 4) != 0) { a = (int)i; b = (int)i + 1; break; } }
    if (!strcmp(kind, "prefix")) { for (size_t i = 0; i < c.n; i++) if (c.r[i].type == RSX_FC_PASS) { a = (int)i; break; } }   /* insert before the first PASS */
    if (a < 0) { printf("FAIL: mutate %s: nothing to mutate\n", kind); return 1; }
    if (b >= 0) { crec t = c.r[a]; c.r[a] = c.r[b]; c.r[b] = t; }
    if (!strcmp(kind, "flip-range")) c.r[a].p[RSX_FC_RANGE_HEAD] ^= 0x01;
    rsx_fc_writer w;
    if (rsx_fc_w_open(&w, out, c.frame_start, c.frame_count)) { puts("FAIL: mutate: cannot open output"); return 1; }
    for (size_t i = 0; i < c.n; i++) {
        if ((int)i == a && !strcmp(kind, "prefix")) {     /* the tail of a pass that was running when the window opened: its get word and PASS_END */
            const uint32_t get = rsx_fc_get32(c.r[i].p + 4), wv[4] = { 1u, 4u, GCM_CONTROL_GUEST_ADDR + 4u, get };
            rsx_fc_w_words(&w, RSX_FC_WORD, wv, 4);
            const uint32_t pe[6] = { get, 0u, 0x70696C66u, 0u, 0u, 0u };
            rsx_fc_w_words(&w, RSX_FC_PASS_END, pe, 6);
        }
        if ((int)i == a && !strcmp(kind, "drop-word")) continue; rsx_fc_w_rec(&w, c.r[i].type, c.r[i].p, c.r[i].len); }
    const int rc = rsx_fc_w_close(&w);
    cap_free(&c);
    return rc ? 1 : 0;
}

/* ======================= modes ======================= */
static int cmp_files(const char* fa, const char* fb, int quiet, int drop_prefix)
{
    cap_t a, b; evs_t ea = {0}, eb = {0}; cmp_t r;
    int rc;
    if ((rc = cap_load(&a, fa))) { printf("FAIL: cannot load %s (%d)\n", fa, rc); return -1; }
    if ((rc = cap_load(&b, fb))) { printf("FAIL: cannot load %s (%d)\n", fb, rc); return -1; }
    if (drop_prefix) {      /* the unreplayable walker records before the first PASS (see replay_run); C is the same capture */
        size_t first_pass = a.n, w = 0;
        for (size_t i = 0; i < a.n; i++) if (a.r[i].type == RSX_FC_PASS) { first_pass = i; break; }
        for (size_t i = 0; i < a.n; i++) {
            if (i < first_pass && !is_structural(a.r[i].type) && !is_annot_head(a.r[i].type, a.r[i].p) && !is_input(&C.r[i])) { free(a.r[i].p); continue; }
            a.r[w++] = a.r[i];
        }
        a.n = w;
    }
    cap_events(&a, &ea); cap_events(&b, &eb);
    ev_compare(&ea, &eb, &r);
    if (!quiet) printf("[cmp] events %zu vs %zu, mismatches %zu%s%s\n", r.n_a, r.n_b, r.mismatches, r.mismatches ? " first: " : "", r.text);
    cap_free(&a); cap_free(&b); evs_free(&ea); evs_free(&eb);
    return (int)r.mismatches;
}

/* Regression for the pass 157 table bug: more distinct pages than the old fixed 16384-slot table, written with a per-page
 * pattern and read back, plus an untouched page (reads 0). old=1 is the negative control (fixed table, bounded probes). */
static int smtest_run(int old)
{
    enum { N = 40000 };
    sm_reset(); g_sm_fixed = old; g_sm_overflow = 0;
    if (old) { g_sm_cap = SM_SLOTS0; g_sm = (sm_slot*)calloc(g_sm_cap, sizeof(sm_slot)); }
    uint8_t b[16];
    for (uint32_t i = 0; i < N; i++) {
        for (int k = 0; k < 16; k++) b[k] = (uint8_t)(i * 7 + k);
        sm_write(1, 0x10000000u + i * SM_PAGE + 4090, b, 16);          /* straddles into the next page */
    }
    int bad = 0;
    for (uint32_t i = 0; i < N && !g_sm_overflow; i++) {
        uint8_t r[16]; sm_read(1, 0x10000000u + i * SM_PAGE + 4090, r, 16);
        for (int k = 0; k < 16; k++) if (r[k] != (uint8_t)(i * 7 + k)) { bad++; break; }
    }
    uint8_t z[4] = {1, 1, 1, 1}; sm_read(1, 0x30000000u, z, 4);
    if (z[0] | z[1] | z[2] | z[3]) bad++;
    printf("[smtest] %s: pages %u, mismatches %d, overflow %d, table slots %u used %u\n", old ? "old fixed table" : "growing table", N, bad, g_sm_overflow, g_sm_cap, g_sm_used);
    const int fail = bad || g_sm_overflow;
    g_sm_fixed = 0; sm_reset();
    return fail ? 1 : 0;
}

#ifdef RSX_GUEST_SIGNAL_H
#include <pthread.h>
/* patch 2190: the funnel from the walker thread and from another thread. Off the walker the write happens directly (visible at once,
 * as before the funnel) and is counted as offr; inside rsx_gs_walker_enter/leave (nesting-safe) it is counted on. */
static void* gs_thr(void* a) { gcm_guest_signal(RSX_GS_REF, 0x2008, 0xAAAA5555u); gcm_guest_signal_note(RSX_GS_FLIP, 1, 2); return a; }
static int gsthread_run(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
    memset(g_rsx_gs_n, 0, sizeof g_rsx_gs_n);
    gcm_guest_signal(RSX_GS_LABEL, 0x1000, 0x11u);                 /* main thread, not in a pass: offr */
    const int p0 = rsx_gs_walker_enter();
    gcm_guest_signal(RSX_GS_LABEL, 0x1004, 0x22u);                 /* on */
    const int p1 = rsx_gs_walker_enter();                          /* nested */
    gcm_guest_signal(RSX_GS_LABEL, 0x1008, 0x33u);                 /* on */
    rsx_gs_walker_leave(p1);
    gcm_guest_signal(RSX_GS_LABEL, 0x100C, 0x44u);                 /* still on after the inner leave */
    pthread_t t; pthread_create(&t, NULL, gs_thr, NULL); pthread_join(t, NULL);   /* another thread while this one is in a pass: offr */
    rsx_gs_walker_leave(p0);
    gcm_guest_signal(RSX_GS_LABEL, 0x1010, 0x55u);                 /* offr again */
    const unsigned long long *L = g_rsx_gs_n[RSX_GS_LABEL], *R = g_rsx_gs_n[RSX_GS_REF], *F = g_rsx_gs_n[RSX_GS_FLIP];
    int bad = 0;
    if (p0 != 0 || p1 != 1) { printf("FAIL: enter returned %d %d (want 0 1)\n", p0, p1); bad++; }
    if (L[0] != 3 || L[1] != 2) { printf("FAIL: label on/offr %llu/%llu (want 3/2)\n", L[0], L[1]); bad++; }
    if (R[0] != 0 || R[1] != 1 || F[0] != 0 || F[1] != 1) { printf("FAIL: ref %llu/%llu flip %llu/%llu (want 0/1 each: the helper thread is never R)\n", R[0], R[1], F[0], F[1]); bad++; }
    if (vm_read32(0x1000) != 0x11u || vm_read32(0x1004) != 0x22u || vm_read32(0x1008) != 0x33u || vm_read32(0x100C) != 0x44u ||
        vm_read32(0x1010) != 0x55u || vm_read32(0x2008) != 0xAAAA5555u) { printf("FAIL: a funnel write did not land\n"); bad++; }
    if (!bad) printf("[gsthread] ok: label on/offr %llu/%llu, ref off-walker %llu, writes landed directly\n", L[0], L[1], R[1]);
    return bad ? 1 : 0;
}
#endif
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsetenv("RSX_TRACE"); unsetenv("LD_SYNC_HIST"); unsetenv("LD_FIFO_CAPTURE");
    const char* mode = argc > 1 ? argv[1] : "";
    const char* real = getenv("LD_FIFO_REPLAY");
    if (!*mode && real && *real) {
        char in[700], out[700];
        snprintf(in, sizeof in, "%s/fifo-capture.kz3fc", real); snprintf(out, sizeof out, "%s/replay-out", real);
        const char* adv = getenv("LD_FIFO_REPLAY_ADV");
        const int a = adv && adv[0] && adv[0] != '0';
        int rc = cap_load(&C, in);
        if (rc) { printf("FAIL: cannot read %s (%d)\n", in, rc); return 1; }
        printf("[replay] %s: %zu records, %d RANGE record(s) whose digest does not match their bytes\n", in, C.n, C.bad_range);
        if (replay_vm_from_config()) return 1;
        rc = replay_run(out, a);
        char outf[720]; snprintf(outf, sizeof outf, "%s/fifo-capture.kz3fc", out);
        const int mism = cmp_files(in, outf, 0, 1);
        if (mism != (a ? (int)g_hazards : 0)) rc = 1;      /* adversarial: each hazard changes exactly one range digest */
        return rc ? 1 : (g_hazards ? 4 : 0);
    }
    if (!strcmp(mode, "dump") && argc > 4) {      /* dump CAP FROM COUNT: one line per record, first words (pass 156 diagnostics) */
        const int rc = cap_load(&C, argv[2]);
        if (rc) { printf("FAIL: cannot read %s (%d)\n", argv[2], rc); return 1; }
        const size_t from = (size_t)strtoul(argv[3], NULL, 0), cnt = (size_t)strtoul(argv[4], NULL, 0);
        for (size_t i = from; i < C.n && i < from + cnt; i++) {
            printf("%zu t%u len%u:", i, C.r[i].type, C.r[i].len);
            for (uint32_t w = 0; w < 8 && w * 4 + 4 <= C.r[i].len; w++) printf(" %X", rsx_fc_get32(C.r[i].p + w * 4));
            printf("\n");
        }
        return 0;
    }
#ifdef RSX_GUEST_SIGNAL_H
    if (!strcmp(mode, "gsthread")) return gsthread_run();
#endif
    if (!strcmp(mode, "smtest")) return smtest_run(argc > 2 && !strcmp(argv[2], "old"));
    if (!strcmp(mode, "gen") && argc > 2) return gen_run(argv[2]);
    if (!strcmp(mode, "replay") && argc > 3) {
        int rc = cap_load(&C, argv[2]);
        if (rc) { printf("FAIL: cannot read %s (%d)\n", argv[2], rc); return 1; }
        if (replay_vm_from_config()) return 1;
        rc = replay_run(argv[3], argc > 4 && !strcmp(argv[4], "adv"));
        return rc ? 1 : (g_hazards ? 4 : 0);
    }
    if (!strcmp(mode, "cmp") && argc > 3) { const int r = cmp_files(argv[2], argv[3], 0, 0); return r ? 1 : 0; }
    if (!strcmp(mode, "check") && argc > 2) {
        cap_t c; const int rc = cap_load(&c, argv[2]);
        if (rc) { printf("FAIL: cannot read %s (%d)\n", argv[2], rc); return 2; }
        printf("[check] %zu records, %d RANGE record(s) whose digest does not match their bytes\n", c.n, c.bad_range);
        const int bad = c.bad_range; cap_free(&c); return bad ? 1 : 0;
    }
    if (!strcmp(mode, "mutate") && argc > 4) return mutate_run(argv[2], argv[3], argv[4]);
    puts("usage: gen DIR | replay CAP OUTDIR [adv] | cmp A B | check CAP | mutate IN OUT KIND | LD_FIFO_REPLAY=dir (no mode)");
    return 2;
}
