/* Host harness for tools/smoke/test_upscale_switch.py (review of PR #27, point 3): when the renderer may release an
 * upscaler context. The Python driver pastes the real code from rsx_live_draw.c (the upscaler state block and
 * ld_flush) where the marker line is; everything else here is a mock. No game data, no GPU, no Windows headers.
 *
 * The mock GPU models the hazard: work recorded into the open command list (a scene capture or depth snapshot that
 * reads the context's resources) and work submitted but not yet complete. Releasing the context while either holds
 * a reference is a use-after-free on the GPU; the mock counts it. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "rsx_upscale.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef long LONG;

/* Windows shims (the extracted code uses the SRW lock and interlocked reads) */
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define AcquireSRWLockShared(p) ((void)pthread_rwlock_rdlock(p))
#define ReleaseSRWLockShared(p) ((void)pthread_rwlock_unlock(p))
#define AcquireSRWLockExclusive(p) ((void)pthread_rwlock_wrlock(p))
#define ReleaseSRWLockExclusive(p) ((void)pthread_rwlock_unlock(p))
static LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c)
{
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}

typedef struct ID3D12Resource ID3D12Resource;
static void retire_texture(ID3D12Resource* r) { (void)r; }

static struct { void* dev; int ready; u64 fence_value; } g;
static u32 g_ld_frames;
static u32 g_up_depth_slot, g_up_depth_frame = UINT32_MAX, g_up_jitter_frame = UINT32_MAX;

/* ---- mock GPU ---- */
static int open_refs;        /* upscaler-resource reads recorded in the open list */
static int inflight_refs;    /* ... submitted, done when the fence reaches inflight_fence */
static u64 inflight_fence, completed_fence;
static int violations, waits;
static void record_capture(void) { open_refs++; }
static int ld_wait_fence(u64 v)
{
    waits++;
    if (v > completed_fence) completed_fence = v;
    if (completed_fence >= inflight_fence) inflight_refs = 0;
    return 1;
}

/* ---- mock rsx_upscale API (the real one lives in rsx_upscale.c) ---- */
struct rsx_upscale { rsx_upscale_backend backend; float sharpness; };
static struct rsx_upscale contexts[8];
static int create_calls, destroy_calls, sharpness_calls;
static float last_sharpness = -1.0f;
rsx_upscale* rsx_upscale_create(const rsx_upscale_config* cfg, void* device)
{
    (void)device;
    if (!cfg || cfg->backend == RSX_UPSCALE_NONE) return NULL;
    rsx_upscale* u = &contexts[create_calls++ & 7];
    u->backend = cfg->backend;
    u->sharpness = cfg->sharpness;
    return u;
}
void rsx_upscale_destroy(rsx_upscale* u)
{
    if (!u) return;
    destroy_calls++;
    if (open_refs || inflight_refs) violations++;
}
void rsx_upscale_set_retire(rsx_upscale* u, void (*retire)(void*, void*), void* user) { (void)u; (void)retire; (void)user; }
void rsx_upscale_set_sharpness(rsx_upscale* u, float s) { if (u) { u->sharpness = s; sharpness_calls++; last_sharpness = s; } }
void rsx_upscale_frame_end(rsx_upscale* u) { (void)u; }
float rsx_upscale_clamp_sharpness(float s) { return s < 0.0f ? 0.0f : s > 2.0f ? 2.0f : s; }
void rsx_upscale_config_from_env(rsx_upscale_config* out) { memset(out, 0, sizeof *out); out->backend = RSX_UPSCALE_NONE; }
const char* rsx_upscale_backend_name(rsx_upscale_backend b) { return b == RSX_UPSCALE_FSR3 ? "fsr3" : b == RSX_UPSCALE_FSR1 ? "fsr1" : "none"; }

/* ---- what ld_flush calls around the extracted body ---- */
typedef enum { LD_FLUSH_PRESENT, LD_FLUSH_MOVIE_PRESENT, LD_FLUSH_GUEST_REFERENCE, LD_FLUSH_READBACK,
               LD_FLUSH_REASON_COUNT } ld_flush_reason;
enum { RSX_PERF_FLUSH_T, RSX_PERF_FLUSH_N, RSX_PERF_FLUSH_PRESENT_N, RSX_PERF_FLUSH_REF_N, RSX_PERF_FLUSH_OTHER_N,
       RSX_PERF_W_FLUSH_N0, RSX_PERF_W_FLUSH_T0 = RSX_PERF_W_FLUSH_N0 + LD_FLUSH_REASON_COUNT };
static int perf_on;
static int rsx_perf_enabled(void) { return perf_on; }
static int rsx_perf_walk_on(void) { return 0; }
static long long rsx_perf_now(void) { return 0; }
static void rsx_perf_add(int k, long long v) { (void)k; (void)v; }
static void ld_tw_pool_tick(void) {}
static void ld_flush_body(ld_flush_reason reason)
{   /* close, submit, signal, reset: the open list's work is now in flight and the new list is empty */
    (void)reason;
    inflight_refs += open_refs;
    open_refs = 0;
    inflight_fence = ++g.fence_value;
}

/* @@EXTRACTED@@ */

static unsigned checks, fails;
static void check(const char* name, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", name); }
}

static void* menu_thread(void* arg)
{   /* the game's menu thread changing the setting while the render thread draws */
    (void)arg;
    for (int i = 0; i < 20000; i++)
        rsx_live_draw_set_upscale((i & 1) ? RSX_UPSCALE_FSR3 : RSX_UPSCALE_NONE, 0.25f);
    rsx_live_draw_set_upscale(RSX_UPSCALE_FSR3, 0.5f);
    return NULL;
}

int main(void)
{
    g.ready = 1;
    g.dev = &g;
    /* the port configures before the renderer's first draw */
    rsx_live_draw_set_upscale(RSX_UPSCALE_FSR3, 0.5f);
    rsx_upscale* up = ld_up_get();
    check("startup-creates-requested", up && up->backend == RSX_UPSCALE_FSR3 && create_calls == 1);

    /* frame: a capture reads the context, then the player turns the upscaler off */
    record_capture();
    rsx_live_draw_set_upscale(RSX_UPSCALE_NONE, 0.5f);
    up = ld_up_get();   /* the per-draw path */
    check("no-destroy-mid-frame", destroy_calls == 0 && up && violations == 0);
    ld_flush(LD_FLUSH_GUEST_REFERENCE);   /* a mid-frame flush is not the switch point */
    check("no-switch-on-other-flush", destroy_calls == 0);
    record_capture();
    ld_flush(LD_FLUSH_PRESENT);
    check("switch-at-present-flush", destroy_calls == 1 && ld_up_get() == NULL);
    check("destroy-after-gpu-drained", violations == 0 && waits >= 1);

    /* on again: created at the next present flush */
    rsx_live_draw_set_upscale(RSX_UPSCALE_FSR3, 0.5f);
    check("enable-waits-for-flush", ld_up_get() == NULL && create_calls == 1);
    ld_flush(LD_FLUSH_PRESENT);
    up = ld_up_get();
    check("enable-at-present-flush", up && up->backend == RSX_UPSCALE_FSR3 && create_calls == 2);

    /* sharpness alone applies at once and keeps the context */
    rsx_live_draw_set_upscale(RSX_UPSCALE_FSR3, 0.8f);
    up = ld_up_get();
    check("sharpness-applies-at-once", sharpness_calls == 1 && last_sharpness == 0.8f && destroy_calls == 1);

    /* off and back on within one frame: nothing is torn down */
    record_capture();
    rsx_live_draw_set_upscale(RSX_UPSCALE_NONE, 0.8f);
    (void)ld_up_get();
    rsx_live_draw_set_upscale(RSX_UPSCALE_FSR3, 0.8f);
    (void)ld_up_get();
    ld_flush(LD_FLUSH_PRESENT);
    check("switch-back-keeps-context", destroy_calls == 1 && create_calls == 2 && ld_up_get() == up);

    /* a request that lands after the frame's last draw is still taken at the flush (and with RSX_PERF on) */
    perf_on = 1;
    record_capture();
    rsx_live_draw_set_upscale(RSX_UPSCALE_NONE, 0.8f);
    ld_flush(LD_FLUSH_MOVIE_PRESENT);
    check("late-request-at-flush", destroy_calls == 2 && ld_up_get() == NULL && violations == 0);
    perf_on = 0;

    /* a menu thread toggling while the render thread draws and presents: no torn request, no unsafe release */
    pthread_t t;
    pthread_create(&t, NULL, menu_thread, NULL);
    for (int i = 0; i < 2000; i++) {
        record_capture();
        (void)ld_up_get();
        if ((i & 7) == 7) ld_flush(LD_FLUSH_PRESENT);
    }
    pthread_join(t, NULL);
    ld_flush(LD_FLUSH_PRESENT);
    up = ld_up_get();
    check("threaded-final-request", up && up->backend == RSX_UPSCALE_FSR3 && up->sharpness == 0.5f);
    check("threaded-no-unsafe-release", violations == 0);

    printf("%u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
