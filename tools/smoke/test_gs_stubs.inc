/* Patch 2189/2190 link stubs for host tests that include cellGcmSys.c without linking rsx_commands.c (weak: a build that links the
 * real rsx_commands.c, e.g. test_log_replay, takes the strong definitions). The funnel stub does what the funnel does without the
 * capture words: the guest store. */
#include <stdint.h>
#if __has_include("rsx_guest_signal.h")   /* a pre-2190 tree (test_guest_mem_diff.sh baseline) has no funnel to stub */
#include "rsx_guest_signal.h"
extern uint8_t* vm_base;
extern volatile int g_rsx_fcap_on; extern void rsx_fcap_word(int dir, uint32_t tag, uint32_t ea, uint32_t value);
static void gs_stub_w32(uint32_t a, uint32_t v) { uint8_t* p = vm_base + a; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
__attribute__((weak)) volatile unsigned long long g_rsx_lg_rd_state, g_rsx_lg_rd_param;
__attribute__((weak)) unsigned long long g_rsx_gs_n[RSX_GS_N][2];
__attribute__((weak)) void (*g_rsx_gs_trace)(unsigned kind, uint32_t ea, uint32_t n, int on_walker);
__attribute__((weak)) void (*g_rsx_gs_s7)(unsigned kind);   /* patch 2204: the S7.0 funnel hook; NULL = off */
__attribute__((weak)) int rsx_gs_walker_enter(void) { return 0; }
__attribute__((weak)) void rsx_gs_walker_leave(int prev) { (void)prev; }
__attribute__((weak)) void gcm_guest_signal(unsigned kind, uint32_t ea, uint32_t val) { gs_stub_w32(ea, val); if (g_rsx_fcap_on) rsx_fcap_word(1, kind, ea, val); }
__attribute__((weak)) void gcm_guest_signal_hostbe(unsigned kind, uint8_t* p, uint32_t tag_ea, uint32_t val)
{ if (g_rsx_fcap_on) rsx_fcap_word(1, kind, tag_ea, val); p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16); p[2] = (uint8_t)(val >> 8); p[3] = (uint8_t)val; }
__attribute__((weak)) void gcm_guest_signal_span(unsigned kind, uint32_t ea, uint32_t n) { if (g_rsx_fcap_on) rsx_fcap_word(1, kind, ea, n); }
__attribute__((weak)) void gcm_guest_signal_note(unsigned kind, uint32_t a, uint32_t b) { if (g_rsx_fcap_on) rsx_fcap_word(1, kind, a, b); }
#endif
/* Patch 2197 link stubs: cellGcmSys.c calls the publisher (libs/video/rsx_pub.c) at the pass brackets, the report site and the ref-queue
 * overflow. Weak: a build that links the real rsx_pub.c (test_log_replay and the tests that use its harness) takes the strong definitions;
 * every other host test runs with RSX_PUB off, which is what these do (nothing). */
#if __has_include("rsx_pub.h")
#include "rsx_pub.h"
__attribute__((weak)) int g_rsx_pub_mode;
__attribute__((weak)) void (*g_rsx_pub_trace)(uint64_t seq, unsigned kind, uint32_t ea, uint32_t val, uint32_t stream_off);
__attribute__((weak)) void rsx_pub_set_stream_src(const volatile uint32_t* p) { (void)p; }
__attribute__((weak)) void rsx_pub_set_reader(uint32_t (*rd32)(uint32_t ea)) { (void)rd32; }
__attribute__((weak)) void rsx_pub_init_env(int force_off) { (void)force_off; }
__attribute__((weak)) void rsx_pub_pass_begin(void) {}
__attribute__((weak)) void rsx_pub_pass_end(void) {}
__attribute__((weak)) void rsx_pub_note_refq_drop(void) {}
__attribute__((weak)) void rsx_pub_note_report(int calc) { (void)calc; }
__attribute__((weak)) int rsx_pub_mode(void) { return 0; }
__attribute__((weak)) unsigned rsx_pub_lag(void) { return 0; }
__attribute__((weak)) void rsx_pub_stats_snapshot(rsx_pub_stats* o) { memset(o, 0, sizeof *o); }
#endif
