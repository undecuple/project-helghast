/* Glue test for KZ3 patches 2204 (S7 probes), 2208 ([o0-lpb]), 2209 (RSX_WALK_TRIVIAL) and 2210 (KZ3_DIAG_DELAY): the arming
 * paths of the real cellGcmSys.c (built with stubs, the harness of test_sync_hist.c) read the environment and either arm the
 * probe or stay silent. One mode per process (the arming flags are cached statics); argv[1] = mode, the shell test sets the
 * environment and checks the exit status and stderr. This is the check that was missing in pass 173: a probe whose switch is
 * not in the run's environment prints nothing, and nothing in the repository said so.
 * Modes: s7 (expects the S7 hook installed), s7off (expects it NULL), lpb, lpboff, wt (RSX_WALK_TRIVIAL), dd (KZ3_DIAG_DELAY). */
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

void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: mode"); return 2; }
    const char* m = argv[1];
    if (!strncmp(m, "s7", 2) || !strncmp(m, "lpb", 3)) {
        (void)rsx_perf_o0_on();
        if (!strcmp(m, "s7")) {
            CHECK(g_rsx_s7p.on == 1, "S7 probe state not on");
            CHECK(g_rsx_gs_s7 != NULL, "g_rsx_gs_s7 hook not installed");
            if (g_rsx_gs_s7) {   /* the funnel's walker-thread call: a barrier-named kind must reach the probe */
                const unsigned long long before = g_rsx_s7p.nar_ep;
                g_rsx_gs_s7(RSX_GS_LABEL);
                CHECK(g_rsx_s7p.nar_ep == before + 1, "barrier did not reach the probe (%llu -> %llu)", before, (unsigned long long)g_rsx_s7p.nar_ep);
            }
        } else if (!strcmp(m, "s7off")) {
            CHECK(g_rsx_s7p.on == 0, "S7 probe on without RSX_O0_S7");
            CHECK(g_rsx_gs_s7 == NULL, "hook installed without RSX_O0_S7");
        } else if (!strcmp(m, "lpb")) {
            CHECK(g_rsx_lpb.on == 1, "lpb not on");
        } else CHECK(g_rsx_lpb.on == 0, "lpb on without RSX_O0");
    } else if (!strcmp(m, "wt")) {
        const int on = rsx_walk_trivial_on();
        CHECK(on == (getenv("RSX_WALK_TRIVIAL") && getenv("RSX_WALK_TRIVIAL")[0] == '1'), "walk-trivial arming %d", on);
        RSX_WT_ADD(&g_rsx_wt, methods, 3);
        CHECK(g_rsx_wt.methods == 3, "counter");
    } else if (!strcmp(m, "dd")) {
        gcm_dd_init_once();
        for (int i = 0; i < 3; i++) rsx_dd_hook(RSX_DD_MAIN);
        CHECK(g_rsx_dd.on == (getenv("KZ3_DIAG_DELAY") != NULL && g_rsx_dd.ms[0] > 0), "dd arming %d", g_rsx_dd.on);
        CHECK(g_rsx_dd.count[RSX_DD_MAIN] == (g_rsx_dd.on ? 3u : 0u), "dd count %llu", g_rsx_dd.count[RSX_DD_MAIN]);
        CHECK(g_rsx_dd.on == 0 || g_rsx_dd.ticks[RSX_DD_MAIN] >= 3ull * (unsigned long long)(g_rsx_dd.ms[0] * (double)g_rsx_dd.freq / 1000.0), "dd injected less than asked");
    } else { puts("unknown mode"); return 2; }
    printf("[test_probe_glue] %s: %s\n", m, fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
