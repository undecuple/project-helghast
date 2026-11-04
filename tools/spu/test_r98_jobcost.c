/* Patch 1114 host test (SPU pass 98): KZ3_SPU_JOBCOST parsing and the job-call spin. A job call of a listed image
 * costs F x its own time; F 1, an unlisted image, a close at another pc or depth, and a bad switch value spin
 * nothing. Built by tools/spu/test_r98_jobcost.sh against the patched toolkit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "spu_context.h"

/* runtime globals spu_channels.c needs (stubs; the probe touches none of them) */
static uint8_t g_guest_mem[0x10000];
uint8_t* vm_base = g_guest_mem;
uint32_t g_barrier_sync_watch, g_ydkj_real_taskset_ea, g_ydkj_real_taskid, g_ydkj_real_spurs_ea;
uint32_t g_spu_image_src_ea, g_spu_image_span, g_spu_image_ls_start;
unsigned long long ps3_ms_now(void)
{
    return 0;
}
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

extern int g_kz3_jobcost;
void kz3_jobcost_init_once(void);
void kz3_jobcall_open(spu_context* ctx, uint32_t lr);
void kz3_jobcall_close(spu_context* ctx, uint32_t pc);

/* The job-call spin is wall time ((F - 1) x the bracket's own wall duration), so a loaded machine stretches both the
 * work and the spin. The test therefore never compares a wall time with a fixed number: the work is `work` ms of
 * THREAD CPU time (a preempted thread does not advance it), the zero-spin checks judge the call's thread CPU time
 * (a call that spins nothing cannot use more), and the F checks judge wall total / wall of the work part (about F
 * whatever the load did to the work), the lower bound on every run and the upper bound on the best of several runs
 * (a preemption only ever adds time). */
static double clk_ms(clockid_t id)
{
    struct timespec t;
    clock_gettime(id, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

typedef struct { double wall, wall_work, cpu; } callms;

/* one job call of `work` ms of CPU on image img, closed at pc/depth */
static callms call(spu_context* c, int img, double work, uint32_t pc, int depth)
{
    callms r;
    c->resident_ovl = img;
    c->host_depth = 0;
    const double w0 = clk_ms(CLOCK_MONOTONIC), c0 = clk_ms(CLOCK_THREAD_CPUTIME_ID);
    kz3_jobcall_open(c, 0x1000);
    while (clk_ms(CLOCK_THREAD_CPUTIME_ID) - c0 < work) {
    }
    r.wall_work = clk_ms(CLOCK_MONOTONIC) - w0;
    c->host_depth = depth;
    kz3_jobcall_close(c, pc);
    r.wall = clk_ms(CLOCK_MONOTONIC) - w0;
    r.cpu = clk_ms(CLOCK_THREAD_CPUTIME_ID) - c0;
    return r;
}

#define REPS 5
/* zero-spin: the best CPU time of REPS calls stays near the work (limit work + 2 ms: bracket and clock reads only) */
static int spins_nothing(spu_context* c, int img, double work, uint32_t pc, int depth)
{
    double best = 1e9;
    for (int i = 0; i < REPS; i++) {
        const callms r = call(c, img, work, pc, depth);
        if (r.cpu < best) best = r.cpu;
    }
    printf("  best cpu %.2f ms for %.1f ms of work\n", best, work);
    return best < work + 2.0;
}
/* cost xF: no run below 0.95 F of its own work wall time, the best run no more than 1.15 F + 0.5 ms of that */
static int costs_x(spu_context* c, int img, double work, uint32_t pc, int depth, double f)
{
    double best = 1e9;
    int low_ok = 1, i;
    for (i = 0; i < 40 && best >= f * 1.15 + 0.5 / work; i++) {   /* stops at the first run without added delay */
        const callms r = call(c, img, work, pc, depth);
        const double ratio = r.wall / r.wall_work;
        if (ratio < f * 0.95) low_ok = 0;
        if (ratio < best) best = ratio;
    }
    printf("  best wall/work ratio %.3f in %d run(s) (x%.0f expected)\n", best, i, f);
    return low_ok && best < f * 1.15 + 0.5 / work;
}

int main(int argc, char** argv)
{
    static uint8_t ls[0x40000];
    if (argc > 1 && !strcmp(argv[1], "bad")) { /* a malformed value leaves the probe off */
        setenv("KZ3_SPU_JOBCOST", "x:2", 1);
        kz3_jobcost_init_once();
        check("malformed value: off", g_kz3_jobcost == 0);
        return fails != 0;
    }
    spu_context* c = calloc(1, sizeof *c);
    c->ls = ls;
    setenv("KZ3_SPU_JOBCOST", "255:3,227:1", 1);
    kz3_jobcost_init_once();
    check("switch parsed (on)", g_kz3_jobcost == 1);
    check("listed image x3 costs ~3x", costs_x(c, 255, 4.0, 0x1000, 0, 3.0));
    check("listed image x1 spins nothing", spins_nothing(c, 227, 4.0, 0x1000, 0));
    check("unlisted image spins nothing", spins_nothing(c, 100, 4.0, 0x1000, 0));
    check("close at another pc spins nothing", spins_nothing(c, 255, 4.0, 0x2000, 0));
    /* patch 1141 (SPU pass 121): the bracket closes at the link regardless of host depth (an interrupt's iret inside
     * the job leaves host_depth elsewhere), so a close at another depth still costs the job x3 */
    check("close at another depth still closes (x3)", costs_x(c, 255, 4.0, 0x1000, 1, 3.0));
    check("x3 again after the others (bracket re-armed)", costs_x(c, 255, 2.0, 0x1000, 0, 3.0));
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
