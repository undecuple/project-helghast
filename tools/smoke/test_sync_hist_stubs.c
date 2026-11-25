/* Link stubs for tools/smoke/test_sync_hist.c (compiled apart from the toolkit headers, like test_gcm_fast_methods_stubs.c):
 * the walker's other consumers. rsx_live_draw_before_guest_sync and rsx_live_draw_writeback_range are defined by the test. */
#define Z(n) long n()  { return 0; }
/* weak: test_log_replay.c supplies the recorded renderer report value (pass 156) */
__attribute__((weak)) unsigned rsx_live_draw_report_value(unsigned off, unsigned dflt) { (void)off; (void)dflt; return 0; }
Z(CreateEventA) Z(SetEvent) Z(WaitForSingleObject)
Z(kz3_wacct_flip) Z(kz3_wacct_note) Z(kz3_wacct_now) Z(kz3_wacct_trace)
Z(rsx_live_draw_debug_cond_render_stats) Z(rsx_live_draw_debug_report_readback_stats)
Z(rsx_live_draw_note_guest_write_pages) Z(rsx_live_draw_note_spu_read) Z(rsx_live_draw_wbp_get) Z(rsx_live_draw_wbp_put) Z(rsx_live_draw_wbp_copy_done) Z(rsx_live_draw_wbp_usercmd)
Z(rsx_live_draw_set_write_source) Z(rsx_live_draw_submit_pending)
Z(rsx_live_draw_watch_copy)
/* weak: test_log_replay.c (patch 2172) feeds the NV0039 destination to the LD_FRONT_PROBE write generations */
__attribute__((weak)) unsigned rsx_live_draw_note_guest_write(unsigned l, unsigned o, unsigned n) { (void)l; (void)o; (void)n; return 0; }
int g_kz3_wacct;
void* g_kz3_wtrace;
__attribute__((weak)) unsigned long long g_rsx_game_frame_marks[1024];
int spu_taskset_round_busy(unsigned* ts, unsigned* tid) { (void)ts; (void)tid; return 0; }
Z(rsx_live_draw_wbd_drain) Z(rsx_live_draw_wbd_touch) Z(rsx_live_draw_wbd_active) Z(rsx_live_draw_wbd_set_site) Z(rsx_live_draw_wbd_set_site_4097) Z(rsx_live_draw_wbd_hold_label) Z(rsx_live_draw_wbd_idle) Z(rsx_live_draw_wbd_defer_move)   /* patch 2185 */
#include "test_gs_stubs.inc"
