/* Patch 1154 host test and micro-benchmark: cellSpursCreateTask's host side (take_slot, mark_running, set_exit, add_task,
 * exit-code entry, spu_taskset_dispatch) over a fake Taskset2 in guest memory, with KZ3_SPU_TASK_POOL off and on.
 * Run twice by tools/spu/test_task_pool.sh (mode from the environment); prints RESULT lines the script compares:
 * the taskset bytes after a serialized run must be identical, every job must see its own ids, a burst of concurrent
 * jobs must all run at once, and the pool must reuse threads. Built from work/ps3recomp-patched (SKIP without it). */
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
uint64_t spurs_pm_build_context(uint8_t* c, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    (void)c;
    (void)a;
    (void)b;
    (void)d;
    (void)e;
    return 0;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx)
{
    (void)tid;
    (void)ctx;
}

#include "../../libs/spurs/spurs_taskset.c"
extern int spu_taskset_claim(uint32_t, uint32_t, int);
extern int spu_taskset_dispatch(uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t);
extern void spu_taskset_mark_running(uint32_t, uint32_t);
extern void spu_taskset2_set_exit(uint32_t, uint32_t, int, uint32_t);
extern void spu_taskset2_register(uint32_t);
extern void spu_task2_exit_code_set(uint32_t, uint32_t, int32_t, int);
extern uint64_t spu_workload_fingerprint(const void*, size_t);
extern SPU_THREAD_LOCAL uint32_t g_kz3_cur_taskset, g_kz3_cur_taskid;
extern unsigned long g_spu_pool_reused, g_spu_pool_created;

#define TSE 0x10000u
#define IMG 0x80000u
#define IMGSZ 0x70u
static volatile int g_gate;            /* burst jobs wait for it */
static volatile int g_ran[128], g_bad; /* runs per task id, wrong ids seen */
static volatile int g_inflight, g_peak;
static pthread_t g_tids[4096];
static int g_ntids;
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static void SPU_ABI job_entry(spu_context* c)
{
    (void)c;
    const uint32_t id = g_kz3_cur_taskid;
    if (g_kz3_cur_taskset != TSE || id >= 128) __atomic_add_fetch(&g_bad, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_m);
    pthread_t me = pthread_self();
    int k = 0;
    for (; k < g_ntids; k++)
        if (pthread_equal(g_tids[k], me)) break;
    if (k == g_ntids && g_ntids < 4096) g_tids[g_ntids++] = me;
    int n = ++g_inflight;
    if (n > g_peak) g_peak = n;
    pthread_mutex_unlock(&g_m);
    while (g_gate) usleep(200);
    __atomic_add_fetch(&g_ran[id & 127], 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&g_inflight, 1, __ATOMIC_SEQ_CST);
}
static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

/* cellSpursCreateTask's host-side effects, in its order (libs/spurs/cellSpurs.c): take_slot, running mark, fresh exit entry,
 * task_info + bits, Taskset2 exit-code entry, dispatch. Returns the slot or -1. */
static int create_task(uint32_t elf_ea, uint32_t ctx)
{
    static const uint32_t arg[4] = {1, 2, 3, 4};
    uint32_t s = spurs_taskset_take_slot(TSE, 128);
    if (s >= 128) return -1;
    spu_taskset_mark_running(TSE, s);
    spu_taskset2_set_exit(TSE, s, 0, 0);
    spurs_taskset_add_task(TSE, s, elf_ea, ctx, arg, arg);
    spu_task2_exit_code_set(TSE, s, 0, 1);
    if (!spu_taskset_dispatch(TSE, s, vm_base + elf_ea, IMGSZ, ctx)) {
        spu_taskset_claim(TSE, s, 0);
        return -1;
    }
    return (int)s;
}
static int total_ran(void)
{
    int t = 0;
    for (int i = 0; i < 128; i++) t += g_ran[i];
    return t;
}
static int wait_ran(int want)
{
    for (int i = 0; i < 20000 && total_ran() < want; i++) usleep(500);
    return total_ran() >= want;
}
static int wait_idle(void)
{ /* every enabled bit clear = every task has exited (spu_taskset_task_exited) */
    for (int i = 0; i < 20000; i++) {
        int busy = 0;
        for (int b = 0; b < 16; b++) busy |= vm_base[TSE + 0x30 + b];
        if (!busy) return 1;
        usleep(500);
    }
    return 0;
}
static uint64_t fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

int main(int argc, char** argv)
{
    const int N = argc > 1 ? atoi(argv[1]) : 400;
    const int pool = getenv("KZ3_SPU_TASK_POOL") && *getenv("KZ3_SPU_TASK_POOL") == '1';
    uint8_t* e = vm_base + IMG;
    memset(e, 0, IMGSZ);
    memcpy(e,
           "\x7f"
           "ELF\x01\x02\x01",
           7);
    e[0x19] = 0x10;
    e[0x1C + 3] = 0x34;
    e[0x2A + 1] = 0x20;
    e[0x2C + 1] = 1; /* entry 0x1000, phoff 0x34 */
    e[0x34 + 3] = 1;
    e[0x34 + 7] = 0x60;
    e[0x34 + 0xA] = 0x10;
    e[0x34 + 0x13] = 0x10;
    e[0x34 + 0x17] = 0x10; /* PT_LOAD off 0x60 va 0x1000 fz 16 mz 16 */
    spu_workload_register(spu_workload_fingerprint(e, IMGSZ), job_entry, "pool-test");
    spurs_taskset_init(TSE, 0x20000, 0, 1, 0x2900, 0, 0);
    vm_write32(TSE + 0x1890, 0x2900);
    spu_taskset2_register(TSE);
    int fail = 0;
    /* A: serialized creates; each job ends before the next create, so the slot is always 0 and the taskset bytes after
     * the run do not depend on timing. */
    for (int k = 0; k < 50; k++) {
        int s = create_task(IMG, 0x91000 + 16u * k);
        if (s != 0) {
            fprintf(stderr, "FAIL: serialized create %d got slot %d\n", k, s);
            fail++;
            break;
        }
        if (!wait_ran(k + 1) || !wait_idle()) {
            fprintf(stderr, "FAIL: job %d did not finish\n", k);
            fail++;
            break;
        }
    }
    printf("RESULT tshash=%016llx ran0=%d bad=%d\n", (unsigned long long)fnv(vm_base + TSE, 0x2900), g_ran[0], g_bad);
    /* B: two bursts of 16 jobs held at a gate: all 16 run at once, each id exactly once. Second burst: the pool reuses. */
    for (int round = 0; round < 2; round++) {
        memset((void*)g_ran, 0, sizeof g_ran);
        g_peak = 0;
        g_gate = 1;
        for (int k = 0; k < 16; k++)
            if (create_task(IMG, 0x91000) < 0) {
                fprintf(stderr, "FAIL: burst create\n");
                fail++;
            }
        for (int i = 0; i < 4000 && g_peak < 16; i++) usleep(500);
        int peak = g_peak;
        g_gate = 0;
        int ok = wait_ran(16) && wait_idle();
        int once = 1;
        for (int i = 0; i < 16; i++)
            if (g_ran[i] != 1) once = 0;
        printf("RESULT burst%d peak=%d once=%d finished=%d bad=%d\n", round, peak, once, ok, g_bad);
        if (peak != 16 || !once || !ok || g_bad) fail++;
    }
    printf("RESULT threads=%d pool=%d reused=%lu created=%lu\n", g_ntids, pool, g_spu_pool_reused, g_spu_pool_created);
    /* C: micro-benchmark, batches of 8 creates (the main thread's pattern), timing the creates only. */
    memset((void*)g_ran, 0, sizeof g_ran);
    g_gate = 0;
    double tcr = 0;
    int done = 0, want = 0;
    for (int b = 0; b < N / 8; b++) {
        for (int k = 0; k < 8; k++) {
            double a = now_us();
            int s = create_task(IMG, 0x91000);
            tcr += now_us() - a;
            if (s < 0) {
                fail++;
                continue;
            }
            done++;
            want++;
        }
        wait_ran(want);
        wait_idle();
    }
    printf("BENCH pool=%d creates=%d us_per_create=%.1f creates_per_s=%.0f\n", pool, done, tcr / done,
           1e6 * done / tcr);
    printf(fail ? "FAIL\n" : "OK\n");
    return fail ? 1 : 0;
}
