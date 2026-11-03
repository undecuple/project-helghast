/* Spu pass 139 host test for patch 1150, adapted from the Killzone 2 port's tools/spu/test_task_ids.c (their patch 7004) (the task id a dispatched SPURS task runs as).
 * The task-exit scan (spu_taskset_start_pending -> spu_taskset_task_exited) starts every enabled, ready, not running
 * task of a taskset. Before 7004 it handed each one its taskset and task id through the globals
 * g_ydkj_real_taskset_ea / g_ydkj_real_taskid, which cellSpursCreateTask also writes outside the scan's lock on
 * another PPU thread. This test runs the real scan and dispatcher (spu_workload.c, async pool) on two tasks of one
 * taskset (task 0 = image 30, task 1 = image 31, two minimal SPU ELFs) while a second thread writes the globals the
 * way cellSpursCreateTask does (task id 7). Each lifted entry records the task id its job runs as
 * (g_kz3_cur_taskid). Pass: every job of image 30 ran as task 0 and every job of image 31 as task 1, no task is left
 * enabled or running, ROUNDS rounds complete. With SPU_TASK_IDS_GLOBAL=1 (the old hand-over) the same run is
 * expected to show wrong ids; tools/spu/test_task_ids.sh prints that count as evidence, it does not gate on it.
 * Built by tools/spu/test_r139_task_ids.sh against work/ps3recomp-patched (SKIP without it). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
#include "spu_channels.c"
#include "spu_workload.h"

unsigned long long ps3_ms_now(void) { return 0; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t ts, uint32_t t, uint32_t a, uint32_t b)
{ (void)ls; (void)ts; (void)t; (void)a; (void)b; return 0; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
__attribute__((weak)) int g_kz3_joblog_unused = 0;
__attribute__((weak)) void kz3_joblog_unused_entry(spu_context* c) { (void)c; }
__attribute__((weak)) void kz3_joblog_unused_get(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
__attribute__((weak)) void kz3_joblog_unused_put(spu_context* c, uint32_t a, uint32_t b) { (void)c; (void)a; (void)b; }

void spu_taskset_start_pending(uint32_t taskset_ea);
extern SPU_THREAD_LOCAL uint32_t g_kz3_cur_taskset, g_kz3_cur_taskid;

#define TS 0x40000u
#define ELF_A 0x60000u
#define ELF_B 0x70000u
#define ROUNDS 3000
static volatile long g_runs[2], g_wrong[2];
static volatile int g_stop;

static void SPU_ABI entry_common(int img)
{
    __atomic_add_fetch(&g_runs[img], 1, __ATOMIC_SEQ_CST);
    if (g_kz3_cur_taskid != (uint32_t)img || g_kz3_cur_taskset != TS) __atomic_add_fetch(&g_wrong[img], 1, __ATOMIC_SEQ_CST);
}
static void SPU_ABI entry_a(spu_context* c) { (void)c; entry_common(0); }
static void SPU_ABI entry_b(spu_context* c) { (void)c; entry_common(1); }

static void be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
/* A minimal big-endian ELF32 SPU image: header, one PT_LOAD of 16 bytes at LS 0x1000 (payload `tag`). */
static void mk_elf(uint32_t ea, uint8_t tag)
{
    uint8_t* e = vm_base + ea;
    memset(e, 0, 0x100);
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 2; e[6] = 1;
    be16(e + 0x10, 2); be16(e + 0x12, 23); be32(e + 0x14, 1); be32(e + 0x18, 0x1000);
    be32(e + 0x1C, 0x34); be16(e + 0x28, 0x34); be16(e + 0x2A, 0x20); be16(e + 0x2C, 1); be16(e + 0x2E, 0x28);
    uint8_t* ph = e + 0x34;
    be32(ph + 0x00, 1); be32(ph + 0x04, 0x60); be32(ph + 0x08, 0x1000); be32(ph + 0x0C, 0x1000);
    be32(ph + 0x10, 16); be32(ph + 0x14, 16); be32(ph + 0x18, 5); be32(ph + 0x1C, 16);
    memset(e + 0x60, tag, 16);
}
static void set_task(uint32_t t, uint32_t elf)
{
    uint8_t* ti = vm_base + TS + 0x80 + 0x30 * t;
    be32(ti + 0x14, elf);
    be32(ti + 0x1C, 0);
    spu_lockline_lock();
    vm_base[TS + 0x10] |= (uint8_t)(0x80u >> t);   /* ready */
    vm_base[TS + 0x30] |= (uint8_t)(0x80u >> t);   /* enabled */
    spu_lockline_unlock();
}
/* cellSpursCreateTask's store to the bridge globals, outside the scan's lock, from another thread. */
static void* writer(void* arg)
{
    (void)arg;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_ydkj_real_taskset_ea, TS, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ydkj_real_taskid, 7u, __ATOMIC_RELAXED);
    }
    return NULL;
}
static int enabled_any(void) { return (__atomic_load_n(&vm_base[TS + 0x30], __ATOMIC_ACQUIRE) & 0xC0) != 0; }

int main(void)
{
    mk_elf(ELF_A, 0xA5);
    mk_elf(ELF_B, 0x5A);
    size_t sa = spu_elf_image_size(vm_base + ELF_A, 0x1000), sb = spu_elf_image_size(vm_base + ELF_B, 0x1000);
    if (!sa || !sb) { printf("FAIL: test ELF not accepted (size %zu %zu)\n", sa, sb); return 1; }
    spu_workload_register_img(spu_workload_fingerprint(vm_base + ELF_A, sa), entry_a, 30, "test_a");
    spu_workload_register_img(spu_workload_fingerprint(vm_base + ELF_B, sb), entry_b, 31, "test_b");
    pthread_t w;
    pthread_create(&w, NULL, writer, NULL);
    int stuck = 0;
    for (int r = 0; r < ROUNDS && !stuck; r++) {
        set_task(0, ELF_A);
        set_task(1, ELF_B);
        spu_taskset_start_pending(TS);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            if (!enabled_any()) break;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 2000) {
                printf("round %d: task bits still enabled after 2 s (en=%02X ready=%02X): a task was cleared under the "
                       "wrong id\n", r, vm_base[TS + 0x30], vm_base[TS + 0x10]);
                stuck = 1;
                break;
            }
            sched_yield();
        }
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(w, NULL);
    struct timespec d = { 0, 50 * 1000 * 1000 };
    nanosleep(&d, NULL);
    printf("test_task_ids: mode=%s runs a=%ld b=%ld wrong-id a=%ld b=%ld stuck=%d\n",
           getenv("SPU_TASK_IDS_GLOBAL") ? "globals (old)" : "ids (1150)", g_runs[0], g_runs[1], g_wrong[0], g_wrong[1],
           stuck);
    int ok = !stuck && g_wrong[0] == 0 && g_wrong[1] == 0 && g_runs[0] == ROUNDS && g_runs[1] == ROUNDS;
    return ok ? 0 : 1;
}
