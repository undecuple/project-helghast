/* Patch 1066 host test: cellSpursJoinTaskset's wait (spu_taskset_join_wait) and the taskset generation.
 * Case "parked": an old task (generation 0) parks in WAIT_SIGNAL; join returns at once (a parked task is not running)
 * and cuts it off; a new task of the same taskset EA and task id parks too; one signal wakes the new task, never the
 * old one (before 1066 either thread could consume it: the KZ3 Bink menu-movie stall). Patch 1067: the cut-off task
 * leaves the wait with rc 1 (its thread can end) without taking the signal when KZ3_JOIN_CUTOFF_EXIT=1 (opt-in); without
 * it, it stays parked.
 * Case "running": a task marked running and not parked keeps join waiting with no bound (patch 1093; it was 1000 ms,
 * KZ3_JOIN_TASKSET_MS): still waiting after 1.3 s, it returns once the task exits.
 * Built by tools/spu/test_spurs_join.sh against work/ps3recomp-patched (SKIP without it). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "spu_channels.c"
#include "spu_workload.h" /* spu_lifted_entry_fn, spu_workload_register (pass 96 case) */

unsigned long long ps3_ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}
void ppu_resv_break_store(uint64_t ea)
{
    (void)ea;
}
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a;
    (void)v;
    (void)w;
}
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits)
{
    (void)ea;
    (void)bits;
}
void ydkj_wake_all_event_flags(void) {}
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c;
    (void)a;
    (void)b;
    (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx)
{
    (void)tid;
    (void)ctx;
}

extern int spu_taskset_join_wait(uint32_t taskset_ea);
extern void spu_taskset_bind_thread(uint32_t taskset_ea);
extern int spu_taskset_claim(uint32_t taskset_ea, uint32_t t, int on);
extern int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId);
extern void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

static int g_pass, g_fail;
static const char* g_case = "(none)";
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c)                                                                                                         \
            g_pass++;                                                                                                  \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            fprintf(stderr, "FAIL: %s at %s:%d: %s\n", g_case, __FILE__, __LINE__, #c);                                \
        }                                                                                                              \
    } while (0)

#define TS 0x00010000u
static volatile int g_woke[2];
static volatile int g_join_done;
static void* joiner(void* p)
{
    (void)p;
    spu_taskset_join_wait(TS);
    g_join_done = 1;
    return NULL;
}
static void* waiter(void* p)
{
    int k = (int)(intptr_t)p;
    spu_taskset_bind_thread(TS);
    const int rc = spu_taskset_wait_signal(TS, 0);
    g_woke[k] = rc == 1 ? 2 : 1; /* 2 = left the wait as a cut-off task (patch 1067), 1 = signalled */
    return NULL;
}

#define TS2 0x00020000u
static volatile int g_disp_done, g_disp_rc;
static void SPU_ABI dummy_entry(spu_context* c)
{
    (void)c;
}
static void* dispatcher(void* p)
{
    extern int spu_taskset_dispatch(uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
    g_disp_rc = spu_taskset_dispatch(TS2, 3, vm_base + (uint32_t)(uintptr_t)p, 256, 0);
    g_disp_done = 1;
    return NULL;
}

int main(void)
{
    pthread_t a, b;
    g_case = "parked";
    spu_taskset_claim(TS, 0, 1);                /* the old task: created (running mark) ... */
    pthread_create(&a, NULL, waiter, (void*)0); /* ... and parked in WAIT_SIGNAL */
    usleep(100000);
    unsigned long long t0 = ps3_ms_now();
    int busy = spu_taskset_join_wait(TS);
    CHECK(busy == 0);
    CHECK(ps3_ms_now() - t0 < 150); /* a parked task does not keep join waiting */
    spu_taskset_claim(TS, 0, 1);    /* the new task: same EA, same id */
    pthread_create(&b, NULL, waiter, (void*)1);
    usleep(100000);
    spu_taskset_signal_task(TS, 0);
    usleep(300000);
    CHECK(g_woke[1] == 1); /* the new task got its signal */
    CHECK(g_woke[0] != 1); /* the cut-off task did not */
    if (!getenv("KZ3_JOIN_CUTOFF_EXIT"))
        CHECK(g_woke[0] == 0); /* 1066 alone (1067 off): it stays parked */
    else {
        CHECK(g_woke[0] == 2); /* 1067: it left the wait (its thread ends) */
        CHECK(pthread_join(a, NULL) == 0);
    }
    pthread_join(b, NULL);

    g_case = "running";
    spu_taskset_claim(TS, 0, 1); /* running, never parks */
    {
        pthread_t j;
        pthread_create(&j, NULL, joiner, NULL);
        usleep(1300000);
        CHECK(g_join_done == 0);     /* 1093: no bound, still waiting for the running task */
        spu_taskset_claim(TS, 0, 0); /* the task exits */
        usleep(200000);
        CHECK(g_join_done == 1);     /* ... and join returns */
        pthread_join(j, NULL);
    }

    /* Patch 1072 (KZ3_JOIN2_PROBE=1): the switch installs the channel-count hook; the JoinTask2 dump reads the
     * taskset and the (empty) task-thread registry without touching anything else. */
    g_case = "join2 probe";
    {
        extern int kz3_j2_on(void);
        extern void spu_j2_dump(uint32_t ts, uint32_t id, unsigned spins);
        extern void (*g_kz3_j2_note)(spu_context*, uint32_t);
        setenv("KZ3_JOIN2_PROBE", "1", 1);
        CHECK(kz3_j2_on() == 1);
        CHECK(g_kz3_j2_note != NULL);
        spu_j2_dump(TS, 0, 3000);
        CHECK(1);
    }

    /* Patch 1023 + upstream #381 (SPU pass 96): spu_taskset_dispatch holds the taskset lock while it calls
     * spu_workload_dispatch_async, which (since #381) marks the task running through spu_taskset_mark. Without the
     * held flag the second ts_lock on the same thread spun forever: the first cellSpursCreateTask hung KZ3's main
     * thread (v0.17.0 boot check, 0 flips). The image is junk (its ELF load fails on the task thread); only the
     * registered fingerprint matters, so dispatch reaches the mark. */
    g_case = "dispatch under the taskset lock";
    {
        extern uint64_t spu_workload_fingerprint(const void* data, size_t n);
        const uint32_t img_ea = 0x80000u;
        memset(vm_base + img_ea, 0x5A, 256);
        spu_workload_register(spu_workload_fingerprint(vm_base + img_ea, 256), dummy_entry, "r96-dummy");
        spu_taskset_claim(TS2, 3, 1); /* cellSpursCreateTask marks the slot running first */
        pthread_t d;
        pthread_create(&d, NULL, dispatcher, (void*)(uintptr_t)img_ea);
        for (int i = 0; i < 200 && !g_disp_done; i++) usleep(10000);
        CHECK(g_disp_done == 1); /* returned (before the fix: still spinning on ts_lock) */
        CHECK(g_disp_rc == 1);   /* fingerprint hit: a task thread was spawned */
        if (g_disp_done) pthread_join(d, NULL);
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0; /* without KZ3_JOIN_CUTOFF_EXIT=1 thread a stays parked (cut off); exit ends it */
}
