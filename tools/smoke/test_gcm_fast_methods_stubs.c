/* Link stubs for tools/smoke/test_gcm_fast_methods.c: the walker's other consumers (live-draw hooks, work accounting, the
 * Win32 event calls of the park wait). Deliberately compiled apart from the toolkit headers, so only the symbol names
 * matter; every stub returns 0 and does nothing. Never reached by the synthetic ring (no park, no blit, no flip). */
#define Z(n) long n()  { return 0; }
Z(CreateEventA) Z(SetEvent) Z(WaitForSingleObject)
Z(kz3_wacct_flip) Z(kz3_wacct_note) Z(kz3_wacct_now) Z(kz3_wacct_trace)
Z(rsx_live_draw_before_guest_sync) Z(rsx_live_draw_debug_cond_render_stats) Z(rsx_live_draw_debug_report_readback_stats)
Z(rsx_live_draw_note_guest_write) Z(rsx_live_draw_note_guest_write_pages) Z(rsx_live_draw_note_spu_read)
Z(rsx_live_draw_report_value) Z(rsx_live_draw_set_write_source) Z(rsx_live_draw_submit_pending)
Z(rsx_live_draw_watch_copy) Z(rsx_live_draw_writeback_range)
int g_kz3_wacct;
void* g_kz3_wtrace;
unsigned long long g_rsx_game_frame_marks[1024];
/* v0.19: cellGcmSys.c's nv3089 blit reads/writes through rsx_tiling.c and the walker asks spu_taskset_round_busy */
int spu_taskset_round_busy(unsigned* ts, unsigned* tid) { (void)ts; (void)tid; return 0; }
