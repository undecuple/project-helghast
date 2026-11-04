/* Host test for KZ3 patch 2148 (LD_CB_REUSE): over a synthetic draw sequence the bytes a shader would read from the
 * root-CBV slot are identical with reuse on and off, and equal the guest's constants + xf tail at every draw.
 * Writes go through the toolkit's real rsx_dispatch_method (SET_TRANSFORM_CONSTANT_LOAD/uploads), so the generation
 * counter (rsx_dispatch.c) is exercised, not modelled. Draw actions: nothing, a constant write that changes a value,
 * a write of the same value (generation must not move, reuse stays legal), tail change, seed, ring wrap.
 * A deliberately broken variant (generation ignored) must be caught, which proves the check can fail.
 * Built by tools/smoke/test_ld_cb_reuse.sh with the patched toolkit's rsx_dispatch.c; no game data, no GPU. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rsx_dispatch.h"
#include "rsx_cb_reuse.h"

#define M_VP_UPLOAD_CONST_ID 0x1EFCu
#define M_VP_UPLOAD_CONST    0x1F00u
#define BLOCK   8448u            /* 512*16 + 32 rounded up to 256, as CB_BLOCK_ALIGNED */
#define SLOTS   12u
#define RING    (BLOCK * SLOTS)
#define NDRAWS  30000

typedef struct { uint8_t ring[RING]; uint32_t used; ld_cbr cbr; long copies, reuses; } sim;

static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void make_expected(const rsx_dispatch* d, const float* tail, uint8_t* out)
{
    memcpy(out, d->constants, 512 * 16);
    memcpy(out + 512 * 16, tail, 32);
}

/* mode 0: always copy; 1: reuse; 2: reuse with the generation ignored (broken on purpose) */
static void draw(sim* s, int mode, const rsx_dispatch* d, const float* tail, uint8_t* seen)
{
    const uint32_t gen = mode == 2 ? 0u : d->const_gen;
    if (s->used + BLOCK > RING) { s->used = 0; ld_cbr_invalidate(&s->cbr); }
    uint32_t slot = s->used;
    if (mode && ld_cbr_can_reuse(&s->cbr, s->used, BLOCK, gen, tail, 32)) { slot = s->cbr.off; s->reuses++; }
    else {
        memcpy(s->ring + s->used, d->constants, 512 * 16);
        memcpy(s->ring + s->used + 512 * 16, tail, 32);
        ld_cbr_filled(&s->cbr, s->used, gen, tail, 32);
        s->copies++;
    }
    if (slot == s->used) s->used += BLOCK;   /* a reused draw does not consume a slot */
    memcpy(seen, s->ring + slot, 512 * 16 + 32);
}

static int run(int mode, uint8_t* digest_out, long* reuses)
{
    static rsx_dispatch d;
    static sim s;
    memset(&s, 0, sizeof s);
    rng = 12345u;
    rsx_dispatch_init(&d, NULL);
    float tail[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    int bad = 0;
    uint64_t h = 1469598103934665603ull;
    static uint8_t seen[512 * 16 + 32], want[512 * 16 + 32];
    for (int i = 0; i < NDRAWS; i++) {
        const uint32_t a = rnd() % 100;
        if (a >= 55 && a < 75) {            /* a burst of 1-8 words at a random constant register */
            const uint32_t id = rnd() % 500, n = 1 + rnd() % 8;
            rsx_dispatch_method(&d, M_VP_UPLOAD_CONST_ID, id);
            for (uint32_t w = 0; w < n; w++) rsx_dispatch_method(&d, M_VP_UPLOAD_CONST + w * 4, 0x3F800000u + (rnd() % 7));
        } else if (a >= 75 && a < 83) {     /* write the value already stored */
            const uint32_t id = rnd() % 500;
            rsx_dispatch_method(&d, M_VP_UPLOAD_CONST_ID, id);
            rsx_dispatch_method(&d, M_VP_UPLOAD_CONST, d.constants[id][0]);
        } else if (a >= 83 && a < 92) {
            tail[rnd() % 8] = (float)(rnd() % 5);
        } else if (a >= 92 && a < 94) {
            uint32_t w[64]; for (int k = 0; k < 64; k++) w[k] = rnd();
            rsx_dispatch_seed_transform_constants(&d, w, 64);
        } else if (a >= 94 && a < 97) {
            s.used = 0; ld_cbr_invalidate(&s.cbr);     /* ring wrap / frame reset */
        }
        draw(&s, mode, &d, tail, seen);
        make_expected(&d, tail, want);
        if (memcmp(seen, want, sizeof seen) != 0) bad++;
        for (size_t k = 0; k < sizeof seen; k += 8) { uint64_t v; memcpy(&v, seen + k, 8); h = (h ^ v) * 1099511628211ull; }
    }
    memcpy(digest_out, &h, 8);
    *reuses = s.reuses;
    printf("mode %d: copies %ld reuses %ld mismatching draws %d digest %016llx\n", mode, s.copies, s.reuses, bad,
           (unsigned long long)h);
    return bad;
}

int main(void)
{
    uint8_t d0[8], d1[8], d2[8]; long r0, r1, r2; int fails = 0;
    const int b0 = run(0, d0, &r0), b1 = run(1, d1, &r1), b2 = run(2, d2, &r2);
    if (b0) { puts("[FAIL] always-copy mode differs from the guest state"); fails++; }
    if (b1) { puts("[FAIL] reuse mode read bytes that differ from the guest state"); fails++; }
    if (memcmp(d0, d1, 8)) { puts("[FAIL] reuse on and off read different byte streams"); fails++; }
    if (r0 != 0 || r1 < NDRAWS / 5) { printf("[FAIL] reuse count %ld/%ld unexpected (the sequence should reuse often)\n", r0, r1); fails++; }
    if (b2 == 0) { puts("[FAIL] the broken variant (generation ignored) was not detected"); fails++; }
    (void)r2;
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
