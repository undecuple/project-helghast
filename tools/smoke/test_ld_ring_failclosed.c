/* Patch 2157 host test: the shipped ld_wait_fence / ld_rm_ensure / ld_srv_take / ld_smp_take / ld_rm_try_wrap are
 * compiled against stubs with injected failures. No GPU, no game data. Built by test_ld_ring_failclosed.py, which
 * also builds the pre-2157 text as a negative control (it must fail this test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rsx_ring_marks.h"
typedef uint64_t u64; typedef uint32_t u32; typedef long HRESULT; typedef unsigned long DWORD; typedef void* HANDLE;
#define FAILED(h) ((h) < 0)
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define LD_TAKE_FAIL_REF 0xFFFFFFFFu
#define LD_TAKE_FAIL 0xFFFFFFFFu   /* the shipped #define is checked by the driver */
#define SRV_TABLE_SIZE 1
#define SMP_TABLE_SIZE 1
enum { LD_RM_SRV, LD_RM_SMP, LD_RM_PSCB, LD_RM_N };
enum { RSX_PERF_GPUWAIT_T, RSX_PERF_W_RFW_PSCB_WAIT, RSX_PERF_W_RFW_DESC_WAIT, RSX_PERF_W_RFW_PSCB_AVOID, RSX_PERF_W_RFW_DESC_AVOID };
static u32 g_ld_frames;
static u64 f_completed; static HRESULT f_event_hr; static DWORD f_wait_rc; static u64 f_done_after_wait;
static int f_waits;
struct fence_s;
struct fence_vt { u64 (*GetCompletedValue)(struct fence_s*); HRESULT (*SetEventOnCompletion)(struct fence_s*, u64, HANDLE); };
struct fence_s { const struct fence_vt* lpVtbl; };
static u64 vt_get(struct fence_s* f) { (void)f; return f_completed; }
static HRESULT vt_set(struct fence_s* f, u64 v, HANDLE h) { (void)f; (void)v; (void)h; return f_event_hr; }
static const struct fence_vt vt = { vt_get, vt_set };
static struct fence_s fence_obj = { &vt };
static struct { int ready; struct fence_s* fence; HANDLE fence_event; u32 fr_cur, srv_ring_used, smp_ring_used; } g;
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; f_waits++; f_completed = f_done_after_wait; return f_wait_rc; }
static long long rsx_perf_now(void) { return 0; }
static void rsx_perf_add(int k, long long v) { (void)k; (void)v; }
static int rsx_perf_walk_on(void) { return 0; }
static void ld_dump_dred(const char* w, HRESULT h) { (void)w; (void)h; }
static rsx_ring_marks g_ld_rm[2][LD_RM_N];
static unsigned long long g_ld_rm_wait_n[LD_RM_N], g_ld_rm_avoid_n[LD_RM_N], g_ld_rm_bad_n, g_ld_rm_fallback_n;
static u32 g_ld_unsubmitted; static int oq_open;
static int ring_on = 1;
static int ld_ring_fence_wait_on(void) { return ring_on; }
static rsx_ring_marks* ld_rm(int r) { return &g_ld_rm[g.fr_cur & 1u][r]; }
static int ld_wait_fence(u64 v);
static void ld_oq_close(void) { oq_open = 0; }
static void ld_submit_nowait(void) { if (!g.ready || !g_ld_unsubmitted || oq_open) return; g_ld_unsubmitted = 0; }
#include "shipped.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void reset(void)
{
    memset(g_ld_rm, 0, sizeof g_ld_rm); g.ready = 1; g.fence = &fence_obj; g.srv_ring_used = g.smp_ring_used = 0;
    f_completed = 0; f_event_hr = 0; f_wait_rc = WAIT_OBJECT_0; f_done_after_wait = 100; f_waits = 0; oq_open = 0;
    g_ld_unsubmitted = 0; ring_on = 1;
    /* ring SRV: an old generation mark [0,8) read by fence 7 (not yet complete) */
    rsx_rm_submit(&g_ld_rm[0][LD_RM_SRV], 8, 7); rsx_rm_wrap(&g_ld_rm[0][LD_RM_SRV]);
    rsx_rm_submit(&g_ld_rm[0][LD_RM_SMP], 8, 7); rsx_rm_wrap(&g_ld_rm[0][LD_RM_SMP]);
}
int main(void)
{
    /* success: the wait runs, the mark is retired afterwards, an allocation is handed out */
    reset(); f_done_after_wait = 7;
    CHECK(ld_srv_take() == 0); CHECK(f_waits == 1); CHECK(g_ld_rm[0][LD_RM_SRV].old_n == 1 && g_ld_rm[0][LD_RM_SRV].old[0].start == 1); CHECK(g.srv_ring_used == 1);
    puts("PASS: successful wait retires the mark and allocates");
    /* SetEventOnCompletion fails */
    reset(); f_event_hr = -1;
    CHECK(ld_srv_take() == LD_TAKE_FAIL_REF); CHECK(g_ld_rm[0][LD_RM_SRV].old_n == 1); CHECK(g.srv_ring_used == 0); CHECK(!g.ready);
    puts("checked: event failure");
    /* abandoned / failed wait: the event did not signal and the fence did not advance */
    reset(); f_wait_rc = 0x80; f_done_after_wait = 3;
    CHECK(ld_wait_fence(7) == 0); CHECK(!g.ready);
    reset(); f_wait_rc = 0x80; f_done_after_wait = 3;
    CHECK(ld_smp_take() == LD_TAKE_FAIL_REF); CHECK(g_ld_rm[0][LD_RM_SMP].old_n == 1); CHECK(g.smp_ring_used == 0);
    puts("checked: abandoned wait");
    /* wait returns signalled but the fence is short of the value */
    reset(); f_done_after_wait = 6;
    CHECK(ld_wait_fence(7) == 0);
    /* device removed: the completed value reads all ones */
    reset(); f_done_after_wait = ~(u64)0;
    CHECK(ld_srv_take() == LD_TAKE_FAIL_REF); CHECK(g_ld_rm[0][LD_RM_SRV].old_n == 1);
    reset(); f_completed = ~(u64)0;
    CHECK(ld_wait_fence(7) == 0);
    puts("checked: device removed");
    /* device lost before / during allocation: g.ready already zero, no allocation, no mark retired, no wait */
    reset(); g.ready = 0;
    CHECK(ld_srv_take() == LD_TAKE_FAIL_REF); CHECK(ld_smp_take() == LD_TAKE_FAIL_REF);
    CHECK(f_waits == 0); CHECK(g_ld_rm[0][LD_RM_SRV].old_n == 1); CHECK(g.srv_ring_used == 0 && g.smp_ring_used == 0);
    puts("checked: allocation after g.ready == 0");
    /* the descring flush cause: an open occlusion query must not make the no-drain wrap fall back */
    reset(); oq_open = 1; { const int rings[2] = { LD_RM_SRV, LD_RM_SMP };
    CHECK(ld_rm_try_wrap(rings, 2) == 1); CHECK(!oq_open); CHECK(g_ld_rm_fallback_n == 0); }
    puts("checked: open query does not force the drain");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("ld_ring_failclosed: all pass");
    return 0;
}
