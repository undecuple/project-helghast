/* Host test for KZ3 patch 2086 (rsx_gpupass.h): the pass-class rule and the top-N pick behind `[perf-gpupass]`. */
#include <stdio.h>
#include "rsx_gpupass.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("[PASS] %s\n", msg); else { printf("[FAIL] %s\n", msg); fails++; } } while (0)

int main(void)
{
    /* K4 shapes from RSX pass 55's pass table. */
    CHECK(rsx_gp_class(1, 1024, 1024, 1, 1, 0xFFFFFFFFu, 0) == RSX_GP_SHADOW, "Z16 cascade with colour on = shadow");
    CHECK(rsx_gp_class(1, 1024, 1024, 1, 2, 0, 0) == RSX_GP_SHADOW, "square target, colour off = shadow");
    CHECK(rsx_gp_class(4, 1280, 720, 1, 2, 0, 0) == RSX_GP_ZPRE, "G-buffer set, all colour off = zpre");
    CHECK(rsx_gp_class(4, 1280, 720, 1, 2, 0, 0x0010u) == RSX_GP_GBUF, "MRT nibble on keeps gbuf");
    CHECK(rsx_gp_class(4, 1280, 720, 1, 2, 0x01010101u, 0xFFF0u) == RSX_GP_GBUF, "4 targets, colour on = gbuf");
    CHECK(rsx_gp_class(1, 1280, 720, 1, 2, 0x01000000u, 0) == RSX_GP_POST, "one target, alpha only = post");
    CHECK(rsx_gp_class(1, 640, 360, 1, 0, 0x01010101u, 0) == RSX_GP_POST, "half-res, main zeta = post");
    CHECK(rsx_gp_class(1, 1280, 720, 1, 0, 0, 0) == RSX_GP_ZPRE, "main zeta, colour off = zpre");
    CHECK(rsx_gp_class(1, 128, 128, 1, 2, 0, 0) == RSX_GP_ZPRE, "small square colour off below 256 = zpre");
    CHECK(rsx_gp_class(1, 1280, 720, 0, 0, 0, 0) == RSX_GP_POST, "no zeta, colour off = post");
    CHECK(rsx_gp_class(1, 1280, 720, 1, 2, 0, 0x0010u) == RSX_GP_ZPRE, "one target ignores the MRT mask");

    const double v[6] = {1.0, 5.0, 0.0, 5.0, -2.0, 3.0};
    unsigned top[4] = {9, 9, 9, 9};
    unsigned n = rsx_gp_topn(v, 6, 4, top);
    CHECK(n == 4 && top[0] == 1 && top[1] == 3 && top[2] == 5 && top[3] == 0, "top-4 descending, tie lower index first");
    n = rsx_gp_topn(v, 6, 6, top);
    CHECK(n == 4, "zero and negative values are never picked");
    n = rsx_gp_topn(v, 0, 3, top);
    CHECK(n == 0, "empty input");
    printf("[test_gpupass] %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
