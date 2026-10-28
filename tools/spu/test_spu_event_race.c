/* Patch 1052 host test: event_status raises and acks from two host threads.
 *
 *   tools/spu/test_spu_event_race.sh   (builds against work/ps3recomp-patched, or SPU_HDR=DIR)
 *
 * The SPU's own thread raises SN (0x2, a list parked at a stall-and-notify element) and acknowledges it; a peer
 * thread raises LR (0x400) as spu_coherency.c does for a lost reservation and acknowledges it. Each thread checks
 * that its bit is set right after its raise: nobody else clears that bit, so a missing bit is an erased raise.
 *   new: spu_event_raise / spu_event_ack (atomic, patch 1052)   -> must lose nothing
 *   old: the pre-1052 bare `event_status |= bit` / `&= ~bit`   -> negative control, loses bits on a multi-core host
 * The old arm proves the test can see the race; on a single CPU it may lose nothing (reported, not a failure).
 * Exit 0 = the atomic helpers lost no raise. */
#include "spu_context.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define SN 0x2u
#define LR 0x400u
static spu_context g_ctx;
static volatile int g_go, g_old;
static long g_iters = 4000000;
static unsigned long g_lost[2];

static void raise_bits(uint32_t b)
{
    if (g_old) {
        volatile uint32_t* p = &g_ctx.event_status;
        *p |= b;
    } else
        spu_event_raise(&g_ctx, b);
}
static void ack_bits(uint32_t b)
{
    if (g_old) {
        volatile uint32_t* p = &g_ctx.event_status;
        *p &= ~b;
    } else
        spu_event_ack(&g_ctx, b);
}

static void* worker(void* arg)
{
    int me = (int)(long)arg;
    uint32_t bit = me ? LR : SN;
    while (!g_go) {
    }
    for (long i = 0; i < g_iters; i++) {
        raise_bits(bit);
        if (!(spu_event_load(&g_ctx) & bit)) g_lost[me]++;
        ack_bits(bit);
    }
    return NULL;
}

static unsigned long run(int old)
{
    pthread_t t[2];
    g_old = old;
    g_go = 0;
    g_lost[0] = g_lost[1] = 0;
    g_ctx.event_status = 0;
    for (long i = 0; i < 2; i++) pthread_create(&t[i], NULL, worker, (void*)i);
    g_go = 1;
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("%s: lost SN %lu, lost LR %lu of %ld raises each\n", old ? "old (bare RMW)" : "new (atomic) ", g_lost[0],
           g_lost[1], g_iters);
    return g_lost[0] + g_lost[1];
}

int main(int argc, char** argv)
{
    if (argc > 1) g_iters = atol(argv[1]);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long old = run(1);
    unsigned long new_ = run(0);
    if (!old) printf("note: the old code lost nothing here (%ld CPUs); the negative control is inconclusive\n", ncpu);
    printf("%s\n", new_ ? "FAIL: the atomic helpers lost a raise" : "PASS");
    return new_ ? 1 : 0;
}
