/* Patch 1095 host test (SPU pass 70): the SPU denormal rule (spu_fpmode.h) applied to the helpers.
 * 1. Under the default MXCSR the helpers give IEEE denormals (the old behaviour, negative control: spu_fm of two
 *    tiny normals is a nonzero denormal, spu_fa of a denormal and 0 keeps it).
 * 2. After spu_fpmode_set_spu() (FTZ|DAZ): an underflowing fm/fa/fs result is 0, a denormal operand reads as 0,
 *    normal results are bit-identical to the IEEE ones over random lanes, the rounding mode and exception masks
 *    are unchanged.
 * 3. A decaying tail y = y * 0.99 (the audio case) reaches 0 under FTZ|DAZ and stays on a denormal fixed point
 *    without; the FTZ|DAZ loop is timed against the IEEE one (ns per spu_fm, information only).
 * 4. spu_fpmode_env_on: "1" = on, unset and "0" = off (KZ3_SPU_FTZ=1 is opt-in).
 *   nix develop --command tools/sandbox.sh -- tools/spu/test_spu_ftz.sh */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_fpmode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_kz3_spu_xr = 1; /* the runtime defaults (patch 1071 / 1089) */
int g_kz3_spu_slowcg;
int g_kz3_fma_stats;
int g_kz3_xr_stats;
int g_kz3_spu_xrq = 1;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra)
{
    (void)op;
    (void)m;
    (void)a;
    (void)b;
    (void)c;
    (void)r;
    (void)ra;
}
void kz3_spu_fma_note(const double s[4], int ftz)
{
    (void)s;
    (void)ftz;
}

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* volatile: the compiler must not fold the helpers at build time (IEEE) instead of running them under MXCSR */
static u128 splat(float f)
{
    volatile float v = f;
    u128 r;
    for (int i = 0; i < 4; i++) r._f32[i] = v;
    return r;
}
static int is_denorm(uint32_t u)
{
    return (u & 0x7F800000u) == 0 && (u & 0x007FFFFFu) != 0;
}
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static double decay(int* steps_to_zero, int* denorm_end)
{
    u128 y = splat(1e-30f), k = splat(0.99f);
    *steps_to_zero = -1;
    double t = now_ns();
    for (int i = 0; i < 200000; i++) {
        y = spu_fm(y, k);
        if (*steps_to_zero < 0 && y._u32[0] == 0) *steps_to_zero = i + 1;
    }
    t = now_ns() - t;
    *denorm_end = is_denorm(y._u32[0]);
    return t / 200000.0;
}

int main(void)
{
    /* 4: the switch */
    CHECK(spu_fpmode_env_on(NULL) == 0, "unset must be off (opt-in)");
    CHECK(spu_fpmode_env_on("1") == 1, "\"1\" must be on");
    CHECK(spu_fpmode_env_on("0") == 0, "\"0\" must be off");

    const float tiny = 1e-20f, den = 1e-40f;
    /* 1: IEEE (negative control) */
    u128 r = spu_fm(splat(tiny), splat(tiny));
    CHECK(is_denorm(r._u32[0]), "IEEE fm(1e-20,1e-20) should be a denormal, got %08X", r._u32[0]);
    r = spu_fa(splat(den), splat(0.0f));
    CHECK(is_denorm(r._u32[0]), "IEEE fa(denormal, 0) should keep the denormal, got %08X", r._u32[0]);
    int z0, d0;
    const double ns_ieee = decay(&z0, &d0);
    CHECK(z0 < 0 && d0, "IEEE decay should stay on a denormal (steps to zero %d)", z0);

    /* random normal lanes: IEEE results, to compare after the switch */
    enum { N = 1 << 16 };
    static u128 A[N], B[N], RM[N], RA[N], RS[N];
    srand(0x4B5A33);
    for (int i = 0; i < N; i++)
        for (int l = 0; l < 4; l++) {
            const float a = ((float)rand() / RAND_MAX - 0.5f) * ldexpf(1.0f, rand() % 60 - 30);
            const float b = ((float)rand() / RAND_MAX - 0.5f) * ldexpf(1.0f, rand() % 60 - 30);
            A[i]._f32[l] = a;
            B[i]._f32[l] = b;
        }
    for (int i = 0; i < N; i++) {
        RM[i] = spu_fm(A[i], B[i]);
        RA[i] = spu_fa(A[i], B[i]);
        RS[i] = spu_fs(A[i], B[i]);
    }

    /* 2: FTZ|DAZ */
    const unsigned before = spu_fpmode_get();
    spu_fpmode_set_spu();
    const unsigned after = spu_fpmode_get();
    CHECK((after & SPU_FPMODE_FTZ_DAZ) == SPU_FPMODE_FTZ_DAZ, "FTZ|DAZ not set (MXCSR %08X)", after);
    CHECK((after & ~SPU_FPMODE_FTZ_DAZ) == (before & ~SPU_FPMODE_FTZ_DAZ), "other MXCSR bits changed %08X -> %08X",
          before, after);
    r = spu_fm(splat(tiny), splat(tiny));
    CHECK((r._u32[0] & 0x7FFFFFFFu) == 0, "FTZ fm(1e-20,1e-20) should be 0, got %08X", r._u32[0]);
    r = spu_fa(splat(den), splat(0.0f));
    CHECK(r._u32[0] == 0, "DAZ fa(denormal, 0) should be +0, got %08X", r._u32[0]);
    r = spu_fm(splat(den), splat(1e30f));
    CHECK(r._u32[0] == 0, "DAZ fm(denormal, 1e30) should be 0 (IEEE: 1e-10), got %08X", r._u32[0]);
    long diff = 0, nonden = 0;
    for (int i = 0; i < N; i++) {
        const u128 m = spu_fm(A[i], B[i]), a = spu_fa(A[i], B[i]), s = spu_fs(A[i], B[i]);
        for (int l = 0; l < 4; l++) {
            const uint32_t pairs[3][2] = {
                {m._u32[l], RM[i]._u32[l]}, {a._u32[l], RA[i]._u32[l]}, {s._u32[l], RS[i]._u32[l]}};
            for (int k = 0; k < 3; k++) {
                if (is_denorm(pairs[k][1])) continue; /* only underflowed lanes may differ */
                nonden++;
                if (pairs[k][0] != pairs[k][1]) diff++;
            }
        }
    }
    CHECK(diff == 0, "%ld of %ld normal-result lanes differ under FTZ|DAZ", diff, nonden);
    int z1, d1;
    const double ns_ftz = decay(&z1, &d1);
    CHECK(z1 > 0 && !d1, "FTZ decay should reach 0 (steps %d)", z1);
    printf("decay tail: IEEE %.2f ns per spu_fm (stays denormal), FTZ|DAZ %.2f ns (0 after %d steps); "
           "%ld normal lanes identical\n",
           ns_ieee, ns_ftz, z1, nonden);
    printf("%s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
