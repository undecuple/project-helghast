/* Host test for KZ3 patch 2041: rsx_dsp_zclear_decode (ZSTENCIL_CLEAR_VALUE by zeta format).
 * Z24S8 (SURFACE_FORMAT depth field 2): depth24 << 8 | stencil8. Z16 (1): depth in bits 0-15, no stencil.
 * Built by tools/smoke/test_zclear.sh against the patched toolkit's rsx_dispatch.h; no game data, no GPU. */
#include <stdio.h>
#include <math.h>
#include "rsx_dispatch.h"

static int fails;
static void check(const char* what, u32 v, u32 fmt, float want_d, unsigned want_s)
{
    float d = -1.0f;
    u8 s = 0xAA;
    rsx_dsp_zclear_decode(v, fmt, &d, &s);
    const int ok = fabsf(d - want_d) <= 1e-7f && s == want_s;
    printf("[%s] %s: value=0x%08X fmt=%u -> depth=%.9f stencil=%u (want %.9f/%u)\n", ok ? "PASS" : "FAIL", what, v, fmt,
           d, s, want_d, want_s);
    if (!ok) fails++;
}

int main(void)
{
    check("Z24S8 reset 1.0/0", 0xFFFFFF00u, 2, 1.0f, 0);
    check("Z24S8 0.0/0xFF", 0x000000FFu, 2, 0.0f, 0xFF);
    check("Z24S8 half/0x80", 0x80000080u, 2, (float)0x800000 / 16777215.0f, 0x80);
    check("Z24S8 all ones", 0xFFFFFFFFu, 2, 1.0f, 0xFF);
    check("Z16 1.0", 0x0000FFFFu, 1, 1.0f, 0);
    check("Z16 0.0", 0x00000000u, 1, 0.0f, 0);
    check("Z16 high bits ignored", 0xFFFF8000u, 1, (float)0x8000 / 65535.0f, 0);
    check("Z16 all ones", 0xFFFFFFFFu, 1, 1.0f, 0);
    /* The old Z24S8-only decode read a Z16 clear to 1.0 as depth 0xFF/0xFFFFFF; make sure that is gone. */
    {
        float d;
        u8 s;
        rsx_dsp_zclear_decode(0x0000FFFFu, 1, &d, &s);
        if (d < 0.5f) {
            printf("[FAIL] Z16 clear to 1.0 decoded as near 0 (%.9f)\n", d);
            fails++;
        }
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
