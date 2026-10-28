/* Host test for KZ3 patch 2085: SET_COLOR_MASK writes with reserved bits (any bit outside 0x01010101).
 * Default: the write is applied (nonzero byte = channel on, as the live renderer reads it) and logged.
 * RSX_COLOR_MASK_RESERVED=ignore: the write is dropped and the old mask kept (JonathanDC64/ps3recomp PR #47).
 * Clean 0/1 byte values always apply. Built by tools/smoke/test_color_mask.sh against the patched toolkit's
 * rsx_dispatch.c alone; no game data, no GPU. argv[1] is "default" or "ignore" (the script sets the env). */
#include <stdio.h>
#include <string.h>
#include "rsx_dispatch.h"

#define M_COLOR_MASK 0x0324u

static int fails;
static void expect(const char* what, const rsx_dispatch* d, u32 want)
{
    const u32 got = rsx_dsp_reg(d, M_COLOR_MASK);
    const int ok = got == want;
    printf("[%s] %s: mask=0x%08X (want 0x%08X)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) fails++;
}

int main(int argc, char** argv)
{
    const int ignore = argc > 1 && !strcmp(argv[1], "ignore");
    static rsx_dispatch d;
    rsx_dispatch_init(&d, NULL);
    expect("seed is all channels on", &d, 0x01010101u);
    rsx_dispatch_method(&d, M_COLOR_MASK, 0x00000100u);
    expect("clean write (G only) applies", &d, 0x00000100u);
    rsx_dispatch_method(&d, M_COLOR_MASK, 0x00020000u);   /* reserved bit 17 */
    expect(ignore ? "reserved write ignored, old mask kept" : "reserved write applied", &d,
           ignore ? 0x00000100u : 0x00020000u);
    rsx_dispatch_method(&d, M_COLOR_MASK, 0x01010101u);
    expect("clean write after a reserved one applies", &d, 0x01010101u);
    rsx_dispatch_method(&d, M_COLOR_MASK, 0x80000000u);
    expect(ignore ? "top-bit write ignored" : "top-bit write applied", &d, ignore ? 0x01010101u : 0x80000000u);
    rsx_dispatch_method(&d, M_COLOR_MASK, 0x00000000u);
    expect("zero (all off) applies", &d, 0u);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
