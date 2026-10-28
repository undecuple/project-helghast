/* Pass 105 (KZ3 patch 1124): the SPU dispatch cache lives in the spu_context, not in host thread-local storage.
 *  - a cache filled by one host thread answers the same context on another thread (no TLS, no context id);
 *  - another context on the same thread keeps its own answers;
 *  - SPU_DISPATCH_CACHE=0 (the default) runs the full resolver every time;
 *  - spu_context stays 16-byte aligned (malloc'd and stack contexts);
 *  - SPU_DISPATCH_CACHE=2 (shadow) catches a stale entry: counts it, returns the resolver's answer, drops the entry.
 * The "stale entry" is made by changing resident_code directly, bypassing spu_overlay_note_get (the only runtime
 * writer, which bumps dcache_residency). Built by tools/spu/test_r105_dcache.sh against the patched toolkit. */
/* The PR test's runtime stubs (vm_base, PPU backtrace helpers, ...): include it with its main renamed. */
#define main lifted_start_main
#include "tests/test_spu_lifted_start.c"
#undef main
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spu_begin_image(int image_id);
void spu_register_function(uint32_t addr, void (*fn)(spu_context*));
void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id);
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size);
void spu_dcache_reset_mode_for_test(void);
void spu_dcache_stats(unsigned long long* hits, unsigned long long* misses, unsigned long long* bad);

static int r_pass, r_fail;
static void ck(int ok, const char* what) { if (ok) r_pass++; else { r_fail++; printf("FAIL %s\n", what); } }
static void SPU_ABI f_primary(spu_context* c) { (void)c; }
static void SPU_ABI f_region(spu_context* c) { (void)c; }

static const uint32_t A = 0x1C000;   /* r105 */
typedef struct { spu_context* c; spu_lifted_fn got; } job;
static void* other_thread(void* p) { job* j = p; j->got = spu_lifted_lookup(j->c, A); return NULL; }

static void mode(const char* m) { setenv("SPU_DISPATCH_CACHE", m, 1); spu_dcache_reset_mode_for_test(); }
/* Region resident (filled through note_get), then removed behind the cache's back. */
static void make_stale(spu_context* c)
{
    spu_overlay_note_get(c, 0x830000, c->ls + A, 16);
    (void)spu_lifted_lookup(c, A);
    memset(c->resident_code, 0, sizeof c->resident_code);
}

int main(void)
{
    spu_context* c = calloc(1, sizeof *c);
    spu_context* d = calloc(1, sizeof *d);
    if (!c || !d) return 2;
    spu_context_init(c, 0); spu_context_init(d, 1);
    spu_begin_image(121); spu_register_function(A, (void (*)(spu_context*))f_primary);
    spu_begin_image(124); spu_register_function(A, (void (*)(spu_context*))f_region);
    spu_begin_image(0);
    spu_overlay_register_region(0x830000, 0x100, 124);
    c->image_id = 121; d->image_id = 121;

    mode("1");
    make_stale(c);
    job j = { c, NULL };
    pthread_t t; pthread_create(&t, NULL, other_thread, &j); pthread_join(t, NULL);
    ck(j.got == (spu_lifted_fn)f_region, "on: another host thread hits the entry this thread filled (context-resident)");
    ck(spu_lifted_lookup(d, A) == (spu_lifted_fn)f_primary, "on: another context keeps its own answer");
    ck(sizeof c->dcache / sizeof c->dcache[0] == SPU_DCACHE_ENTRIES, "context holds SPU_DCACHE_ENTRIES entries");
    /* Pass 105: an aligned(64) cache made spu_context over-aligned; the exe built that way hung in 3 of 3 K4 runs
     * (contexts come from malloc and live on job stacks, 16-byte aligned at best). */
    ck(_Alignof(spu_context) <= 16, "spu_context is not over-aligned");

    spu_context_init(c, 0); c->image_id = 121;
    mode("0");
    make_stale(c);
    ck(spu_lifted_lookup(c, A) == (spu_lifted_fn)f_primary, "off: the full resolver answers every lookup");
    unsetenv("SPU_DISPATCH_CACHE"); spu_dcache_reset_mode_for_test();
    spu_context_init(c, 0); c->image_id = 121;
    make_stale(c);
    ck(spu_lifted_lookup(c, A) == (spu_lifted_fn)f_primary, "default is off");

    spu_context_init(c, 0); c->image_id = 121;
    mode("2");
    unsigned long long h0, m0, b0, h1, m1, b1;
    spu_dcache_stats(&h0, &m0, &b0);
    make_stale(c);
    ck(spu_lifted_lookup(c, A) == (spu_lifted_fn)f_primary, "shadow: a stale entry returns the resolver's answer");
    spu_dcache_stats(&h1, &m1, &b1);
    ck(b1 == b0 + 1, "shadow: the stale entry is counted as a mismatch");
    ck(spu_lifted_lookup(c, A) == (spu_lifted_fn)f_primary, "shadow: the dropped entry is resolved again");
    spu_dcache_stats(&h1, &m1, &b1);
    ck(b1 == b0 + 1 && m1 >= m0 + 2, "shadow: no second mismatch, misses counted");

    printf("test_r105_dcache: %d passed, %d failed\n", r_pass, r_fail);
    free(c); free(d);
    return r_fail ? 1 : 0;
}
