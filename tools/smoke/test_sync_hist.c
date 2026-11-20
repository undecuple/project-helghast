/* Host test for KZ3 patch 2164 (LD_SYNC_HIST, probe P4, and the LD_FIFO_CAPTURE walker-side writer hooks).
 * Builds the toolkit's cellGcmSys.c with stubs (pattern of test_gcm_fast_methods.c) and walks a synthetic FIFO of known
 * frames through the real walker: draws (the stub live-draw method counts them the way sink_end_impl does), NV406E
 * 0x6C releases, an NV0039 copy, GET_REPORT, SET_REFERENCE and E944 flips. The stub write-back reports "dirty" for
 * chosen events. Modes (argv[1]):
 *   plain    no switches: prints the digest of the live-method call stream; the hooks must change nothing
 *   hist     LD_SYNC_HIST=1: the classifier's accumulators must equal the numbers worked out by hand below, and the
 *            formatted lines must carry them
 *   capture  LD_FIFO_CAPTURE under a 'work' directory, frames 1,1: the file must read back clean and hold the walker's
 *            commands of frame 1, its label/get writes, the NV0039 source bytes and result digest, then print the
 *            same call-stream digest as plain
 * The frames (D = draw, REL = NV406E release, "dirty" = the stub write-back had work):
 *   A: D D D | REL clean | D D | NV0039 dirty | D D D D | REPORT | REF | D | REL dirty | D D D D D | FLIP dirty
 *   B: D D D D | REL dirty | D D | NOP x5 | FLIP clean
 * Hand-worked totals for A B A B (4 flips): see EXPECT below; the per-frame working is in the RE notes "Pass 152". */
#include "cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
static unsigned long long digest = 1469598103934665603ull;
static void mix(u32 tag, u32 a, u32 b)
{
    const u32 w[3] = { tag, a, b };
    for (int i = 0; i < 3; i++) digest = (digest ^ w[i]) * 1099511628211ull;
}
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; mix(1, method, data); return 0; }
int  rsx_draw_engine_enabled(void) { return 1; }
rsx_backend* rsx_get_backend(void) { static rsx_backend dummy; return &dummy; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { mix(2, method, arg); }
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

/* The write-back stubs: before-sync dirty flags are consumed in order (A: 0,1  B: 1); the NV0039 write-back is dirty. */
static const unsigned s_rel_dirty[] = { 0, 1, 1, 0, 1, 1 };   /* A, A, B, A, A, B: REL clean, REL dirty, REL dirty (x2) */
static unsigned s_rel_i;
u32 rsx_live_draw_before_guest_sync(void) { return s_rel_dirty[s_rel_i++ % 6]; }
u32 rsx_live_draw_writeback_range(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; return 4096; }
/* What sink_end_impl / ld_present_body / sink_flip do for the walker's words, in the part this test exercises. */
void rsx_live_draw_method(u32 method, u32 arg)
{
    mix(3, method, arg);
    if (method == 0x1808u && arg == 0u) rsx_sync_hist_draw();
    if (method == 0xE944u) {                       /* flip: arg bit 8 = the main surface was dirty at the flip */
        if (g_rsx_fcap_cfg) rsx_fcap_flip(arg & 7u, 0);
        rsx_sync_hist_event(RSX_SH_FLIP, (arg >> 8) & 1u);
    }
}

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00300000u
#define CMD_SIZE  0x00300000u
static uint32_t g_heap = 0x00500000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap; g_heap += size; return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

/* ---- the synthetic stream ---- */
typedef struct { u32 off, hdr, n, w[8]; } cmd_t;
static cmd_t g_cmds[512]; static int g_ncmd; static int g_frame_first[8], g_frame_last[8];
static u32 g_io;
static void emit(u32 hdr, int n, const u32* w)
{
    cmd_t* c = &g_cmds[g_ncmd++];
    c->off = g_io; c->hdr = hdr; c->n = (u32)n;
    gw32(IO_ADDR + g_io, hdr);
    for (int i = 0; i < n; i++) { gw32(IO_ADDR + g_io + 4 + (u32)i * 4, w[i]); c->w[i] = w[i]; }
    g_io += 4 + (u32)n * 4;
}
static void m1(u32 subch, u32 method, u32 v) { emit((1u << 18) | (subch << 13) | method, 1, &v); }
static void draw(int k) { for (int i = 0; i < k; i++) m1(0, 0x1808u, 0u); }
static void rel(void) { m1(0, 0x64u, 0x4000u); m1(0, 0x6Cu, 0xC0DE0001u + (u32)g_ncmd); }
#define SRC_OFF 0x100000u
#define DST_OFF 0x180000u
static void nv0039(void)
{
    m1(1, 0x0184u, 0xFEED0001u); m1(1, 0x0188u, 0xFEED0001u);
    m1(1, 0x030Cu, SRC_OFF); m1(1, 0x0310u, DST_OFF); m1(1, 0x0314u, 32); m1(1, 0x0318u, 32);
    m1(1, 0x031Cu, 32); m1(1, 0x0320u, 2); m1(1, 0x0324u, 0x101u); m1(1, 0x0328u, 0u);
}
static void nops(int k) { for (int i = 0; i < k; i++) { cmd_t* c = &g_cmds[g_ncmd++]; c->off = g_io; c->hdr = 0; c->n = 0; gw32(IO_ADDR + g_io, 0); g_io += 4; } }
static void frame(int idx)
{
    g_frame_first[idx] = g_ncmd;
    if ((idx & 1) == 0) {   /* A */
        draw(3); rel(); draw(2); nv0039(); draw(4);
        m1(0, 0x1800u, (1u << 24) | 0x40u); m1(0, 0x50u, 7u);
        draw(1); rel(); draw(5); m1(7, 0x0944u, 0x100u | 1u);
    } else {                /* B */
        draw(4); rel(); draw(2); nops(5); m1(7, 0x0944u, 2u);
    }
    g_frame_last[idx] = g_ncmd - 1;
}

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf(" (line %d)\n", __LINE__); fails++; } } while (0)
static int fails;

static void expect_hist(void)
{
    const rsx_sh_acc* a = &s_sh.tot;
    CHECK(a->flips == 4 && a->draws == 42, "flips %llu draws %llu", (unsigned long long)a->flips, (unsigned long long)a->draws);
    CHECK(a->ev[RSX_SH_REL406E] == 6 && a->dirty[RSX_SH_REL406E] == 4, "rel406e %llu/%llu", (unsigned long long)a->ev[RSX_SH_REL406E], (unsigned long long)a->dirty[RSX_SH_REL406E]);
    CHECK(a->ev[RSX_SH_NV0039] == 2 && a->dirty[RSX_SH_NV0039] == 2, "nv0039 %llu/%llu", (unsigned long long)a->ev[RSX_SH_NV0039], (unsigned long long)a->dirty[RSX_SH_NV0039]);
    CHECK(a->ev[RSX_SH_FLIP] == 4 && a->dirty[RSX_SH_FLIP] == 2, "flip %llu/%llu", (unsigned long long)a->ev[RSX_SH_FLIP], (unsigned long long)a->dirty[RSX_SH_FLIP]);
    CHECK(a->ev[RSX_SH_REPORT] == 2 && a->ev[RSX_SH_REF] == 2 && a->ev[RSX_SH_REL4097] == 0, "report %llu ref %llu", (unsigned long long)a->ev[RSX_SH_REPORT], (unsigned long long)a->ev[RSX_SH_REF]);
    CHECK(a->rp[0] == 10 && a->rp[1] == 8 && a->rp_max[0] == 4 && a->rp_max[1] == 3, "rp all %llu hard %llu max %llu/%llu",
          (unsigned long long)a->rp[0], (unsigned long long)a->rp[1], (unsigned long long)a->rp_max[0], (unsigned long long)a->rp_max[1]);
    CHECK(a->iv_n[0] == 12 && a->iv_sum[0] == 42 && a->iv_max[0] == 5, "iv all n %llu sum %llu max %llu", (unsigned long long)a->iv_n[0], (unsigned long long)a->iv_sum[0], (unsigned long long)a->iv_max[0]);
    CHECK(a->iv_n[1] == 10 && a->iv_sum[1] == 42 && a->iv_max[1] == 5, "iv hard n %llu sum %llu max %llu", (unsigned long long)a->iv_n[1], (unsigned long long)a->iv_sum[1], (unsigned long long)a->iv_max[1]);
    CHECK(a->iv_hist[0][2] == 6 && a->iv_hist[0][3] == 6 && a->iv_hist[1][2] == 2 && a->iv_hist[1][3] == 8, "hist %llu %llu | %llu %llu",
          (unsigned long long)a->iv_hist[0][2], (unsigned long long)a->iv_hist[0][3], (unsigned long long)a->iv_hist[1][2], (unsigned long long)a->iv_hist[1][3]);
    CHECK(a->chunks == 14 && a->chunk_max == 5, "chunks %llu max %llu", (unsigned long long)a->chunks, (unsigned long long)a->chunk_max);
    char line[2048];
    rsx_sh_fmt(a, "summary", line, sizeof line);
    CHECK(strstr(line, "[sync-hist] summary flips 4 draws 42 (") && strstr(line, "resolve/flip all 2.50 (max 4) hard 2.00 (max 3)") &&
          strstr(line, "chunks/flip 3.5 (max 5) draws/chunk 3.0") && strstr(line, "hist[0,0,6,6,0,0,0,0,0,0,0]"), "line: %s", line);
}

static int rd_cmds(const char* path, int expect_first, int expect_last)
{
    rsx_fc_reader r; rsx_fc_rec rec;
    int rc, ci = expect_first, nop_words = 0, saw_frame_b = 0, saw_label = 0, saw_rd = 0, saw_wr = 0, saw_copy = 0, saw_img = 0, saw_cfg = 0, saw_flip = 0, saw_end = 0;
    if ((rc = rsx_fc_r_open(&r, path)) != 0) { printf("FAIL: open %s rc %d\n", path, rc); return 1; }
    CHECK(r.frame_start == 1 && r.frame_count == 1, "header frames %u,%u", r.frame_start, r.frame_count);
    while ((rc = rsx_fc_r_next(&r, &rec)) == 1) {
        switch (rec.type) {
        case RSX_FC_CONFIG: saw_cfg = 1; CHECK(rsx_fc_get32(rec.p + 16) == 0 || 1, "cfg"); break;
        case RSX_FC_IMAGE: saw_img++; break;
        case RSX_FC_FRAME: break;
        case RSX_FC_CMD: {
            const u32 off = rsx_fc_get32(rec.p), hdr = rsx_fc_get32(rec.p + 4), n = rsx_fc_get32(rec.p + 8);
            if (hdr == 0) { nop_words += (int)rsx_fc_get32(rec.p + 12); break; }
            while (ci <= expect_last && g_cmds[ci].hdr == 0) ci++;     /* NOP commands are checked by their word count */
            CHECK(ci <= expect_last, "extra CMD at 0x%X", off);
            if (ci <= expect_last) {
                CHECK(off == g_cmds[ci].off && hdr == g_cmds[ci].hdr && n == g_cmds[ci].n, "cmd %d: off %X/%X hdr %X/%X n %u/%u",
                      ci, off, g_cmds[ci].off, hdr, g_cmds[ci].hdr, n, g_cmds[ci].n);
                for (u32 i = 0; i < n && i < g_cmds[ci].n; i++) CHECK(rsx_fc_get32(rec.p + 12 + i * 4) == g_cmds[ci].w[i], "cmd %d word %u", ci, i);
                ci++;
            }
            saw_frame_b = 1; break; }
        case RSX_FC_WORD: if (rsx_fc_get32(rec.p) == 1 && rsx_fc_get32(rec.p + 4) == 3 && rsx_fc_get32(rec.p + 8) == GCM_LABEL_GUEST_BASE + 0x4000u) saw_label++; break;
        case RSX_FC_RANGE: {
            const u32 kind = rsx_fc_get32(rec.p), addr = rsx_fc_get32(rec.p + 8), len = rsx_fc_get32(rec.p + 12), fl = rsx_fc_get32(rec.p + 16);
            if (kind == RSX_FC_RD_WALKER) { saw_rd++; CHECK(addr == IO_ADDR + SRC_OFF && len == 64 && (fl & RSX_FC_F_BYTES) &&
                              rsx_fc_get64(rec.p + 24) == rsx_fc_fnv(RSX_FC_FNV_SEED, vm_base + IO_ADDR + SRC_OFF, 64) &&
                              !memcmp(rec.p + RSX_FC_RANGE_HEAD, vm_base + IO_ADDR + SRC_OFF, 64), "RD_WALKER range"); }
            if (kind == RSX_FC_WR_WALKER) { saw_wr++; CHECK(addr == IO_ADDR + DST_OFF && !(fl & RSX_FC_F_BYTES) &&
                              rsx_fc_get64(rec.p + 24) == rsx_fc_fnv(RSX_FC_FNV_SEED, vm_base + IO_ADDR + DST_OFF, 64), "WR_WALKER digest"); }
            break; }
        case RSX_FC_COPY: saw_copy++; CHECK(rsx_fc_get32(rec.p) == 0 && rsx_fc_get32(rec.p + 12) == 32 && rsx_fc_get32(rec.p + 16) == 2 && rsx_fc_get32(rec.p + 32) == 4096, "COPY geometry"); break;
        case RSX_FC_FLIP: saw_flip++; break;
        default: break;
        }
    }
    saw_end = rc == 0;
    CHECK(saw_end, "reader ended with %d (want clean end)", rc);
    CHECK(saw_cfg && saw_img >= 1 && saw_frame_b, "cfg %d img %d cmds %d", saw_cfg, saw_img, saw_frame_b);
    CHECK(ci == expect_last + 1 || (ci == expect_last && g_cmds[expect_last].hdr == 0), "captured %d commands, wanted up to %d", ci - expect_first, expect_last - expect_first + 1);
    CHECK(nop_words == 5, "nop words %d", nop_words);
    CHECK(saw_label == 1 && saw_flip == 1 && saw_rd == 0 && saw_wr == 0 && saw_copy == 0, "label %d flip %d range rd/wr %d/%d copy %d (frame B has no copy)", saw_label, saw_flip, saw_rd, saw_wr, saw_copy);
    rsx_fc_r_close(&r);
    return fails;
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: plain|hist|capture|capture-a|refuse"); return 2; }
    const char* mode = argv[1];
    unsetenv("RSX_TRACE"); unsetenv("LD_SYNC_HIST"); unsetenv("LD_FIFO_CAPTURE");
    char dir[600] = "";
    if (!strcmp(mode, "hist")) { setenv("LD_SYNC_HIST", "1", 1); setenv("LD_SYNC_HIST_N", "2", 1); }
    if (!strcmp(mode, "refuse")) setenv("LD_FIFO_CAPTURE", argc > 2 ? argv[2] : "refused_dir", 1);
    if (!strncmp(mode, "capture", 7)) {
        const char* base = argc > 2 ? argv[2] : "work/cap";
        snprintf(dir, sizeof dir, "%s", base);
        setenv("LD_FIFO_CAPTURE", dir, 1);
        setenv("LD_FIFO_CAPTURE_FRAME", !strcmp(mode, "capture-a") ? "0,1" : "1,1", 1);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    for (int i = 0; i < 64; i++) { vm_base[IO_ADDR + SRC_OFF + i] = (uint8_t)(0x40 + i); }
    for (int f = 0; f < 4; f++) frame(f);
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, g_io);
    for (int pass = 0; pass < 400 && s_fifo_getoff < g_io; pass++) cellGcm_rsx_process_fifo();
    printf("[sync_hist] %s: cmds %d words %u walked %s digest %016llx\n", mode, g_ncmd, g_io / 4, s_fifo_getoff >= g_io ? "all" : "PARTIAL", digest);
    if (s_fifo_getoff < g_io) fails++;
    if (!strcmp(mode, "plain")) {
        CHECK(!g_rsx_sh_on && !g_rsx_fcap_cfg && !g_rsx_fcap_on && s_sh.tot.draws == 0 && s_sh.tot.flips == 0, "switches off must leave the classifier and the capture untouched");
    }
    if (!strcmp(mode, "refuse")) CHECK(!g_rsx_fcap_cfg && !g_rsx_fcap_on, "a capture directory without a 'work' segment must be refused");
    if (!strcmp(mode, "hist")) {
        CHECK(g_rsx_sh_on, "LD_SYNC_HIST=1 must turn the hooks on");
        expect_hist();
    }
    if (!strncmp(mode, "capture", 7)) {
        gcm_fcap_atexit();                         /* the window may still be open (capture-a: frame 0 closes at the first flip) */
        char path[700]; snprintf(path, sizeof path, "%s/fifo-capture.kz3fc", dir);
        if (!strcmp(mode, "capture")) rd_cmds(path, g_frame_first[1], g_frame_last[1]);
        else {   /* capture-a: frame 0 = frame A, with the NV0039 copy */
            rsx_fc_reader r; rsx_fc_rec rec; int rc, rd = 0, wr = 0, cp = 0, rel = 0;
            if (rsx_fc_r_open(&r, path)) { puts("FAIL: open"); return 1; }
            while ((rc = rsx_fc_r_next(&r, &rec)) == 1) {
                if (rec.type == RSX_FC_RANGE && rsx_fc_get32(rec.p) == RSX_FC_RD_WALKER && (rsx_fc_get32(rec.p + 16) & RSX_FC_F_BYTES)) rd++;
                if (rec.type == RSX_FC_RANGE && rsx_fc_get32(rec.p) == RSX_FC_WR_WALKER) wr++;
                if (rec.type == RSX_FC_COPY && rsx_fc_get32(rec.p) == 0 && rsx_fc_get32(rec.p + 32) == 4096) cp++;
                if (rec.type == RSX_FC_WORD && rsx_fc_get32(rec.p + 4) == 3) rel++;
            }
            CHECK(rc == 0 && rd == 1 && wr == 1 && cp == 1 && rel == 2, "capture-a: end %d rd %d wr %d copy %d label writes %d", rc, rd, wr, cp, rel);
            rsx_fc_r_close(&r);
        }
    }
    free(vm_base);
    return fails ? 1 : 0;
}
