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

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* one job call of `work` ms on image img, closed at pc/depth; returns the whole call's ms */
static double call(spu_context* c, int img, double work, uint32_t pc, int depth)
{
    c->resident_ovl = img;
    c->host_depth = 0;
    const double t0 = now_ms();
    kz3_jobcall_open(c, 0x1000);
    while (now_ms() - t0 < work) {
    }
    c->host_depth = depth;
    kz3_jobcall_close(c, pc);
    return now_ms() - t0;
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
    double t = call(c, 255, 4.0, 0x1000, 0);
    printf("  image 255 x3, 4 ms of work: %.2f ms\n", t);
    check("listed image x3 costs ~3x", t > 11.5 && t < 16.0);
    t = call(c, 227, 4.0, 0x1000, 0);
    check("listed image x1 spins nothing", t < 6.0);
    t = call(c, 100, 4.0, 0x1000, 0);
    check("unlisted image spins nothing", t < 6.0);
    t = call(c, 255, 4.0, 0x2000, 0);
    check("close at another pc spins nothing", t < 6.0);
    /* patch 1141 (SPU pass 121): the bracket closes at the link regardless of host depth (an interrupt's iret inside
     * the job leaves host_depth elsewhere), so a close at another depth still costs the job x3 */
    t = call(c, 255, 4.0, 0x1000, 1);
    check("close at another depth still closes (x3)", t > 11.5 && t < 16.0);
    t = call(c, 255, 2.0, 0x1000, 0);
    check("x3 again after the others (bracket re-armed)", t > 5.5 && t < 9.0);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
