// Host harness for tools/smoke/test_upscale_config.py (review of PR #27, points 1 and 4): how port/kz3/main.cpp turns
// RSX_UPSCALE / display.ini names and sharpness values into the renderer's upscaler request, and when the camera hooks
// keep FSR 3.1's pose. The driver pastes kz3_upscale_parse, kz3_upscale_apply and kz3_upscale_temporal from main.cpp
// where the marker line is. No game data, no GPU, no Windows headers.
#include <stdio.h>
#include <string.h>
#include "rsx_upscale.h"
#include "rsx_display_mode.h"

typedef long LONG;
static LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c)
{
    __atomic_compare_exchange_n(p, &c, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}

static volatile LONG s_upscale_eff = RSX_UPSCALE_NONE;
static volatile LONG s_fsr_sharpness_eff = 300;   // FSR 3.1: thousandths, 0.3
static float s_fsr1_sharpness = 1.5f;             // RSX_UPSCALE_SHARPNESS=1.5
static int last_backend = -1;
static float last_sharpness = -1.0f;
static void rsx_live_draw_set_upscale(rsx_upscale_backend b, float s) { last_backend = b; last_sharpness = s; }
#ifdef _WIN32   // rsx_upscale.c names the FSR 1 D3D12 table on Windows; the cases never create an FSR 1 context
extern "C" const rsx_upscale_ops rsx_upscale_fsr1_d3d12_ops = {nullptr, nullptr, nullptr};
#endif

/* @@EXTRACTED@@ */

static void* fake_create(void*, const rsx_upscale_config*) { return (void*)1; }
static void fake_destroy(void*, void (*)(void*, void*), void*) {}
static int fake_evaluate(void*, const rsx_upscale_eval*, float, rsx_upscale_result*, void (*)(void*, void*), void*) { return 0; }

static unsigned checks, fails;
static void check(const char* name, bool ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", name); }
}

int main()
{
    static const rsx_upscale_ops ops = {fake_create, fake_destroy, fake_evaluate};
    rsx_upscale_register_backend(RSX_UPSCALE_FSR3, &ops);   // kz3_fsr31_register runs before kz3_display_load
    // every spelling the toolkit's RSX_UPSCALE reader accepted before the port configured the upscaler itself
    check("fsr1-names", kz3_upscale_parse("fsr1") == RSX_UPSCALE_FSR1 && kz3_upscale_parse("FSR") == RSX_UPSCALE_FSR1);
    check("fsr-1-spelling", kz3_upscale_parse("fsr 1") == RSX_UPSCALE_FSR1);
    check("fsr_1-spelling", kz3_upscale_parse("fsr_1") == RSX_UPSCALE_FSR1 && kz3_upscale_parse(" fsr_1 ") == RSX_UPSCALE_FSR1);
    check("fsr3-names", kz3_upscale_parse("fsr3") == RSX_UPSCALE_FSR3 && kz3_upscale_parse("fsr3.1") == RSX_UPSCALE_FSR3 &&
                        kz3_upscale_parse("FSR31") == RSX_UPSCALE_FSR3);
    check("off-names", kz3_upscale_parse("none") == RSX_UPSCALE_NONE && kz3_upscale_parse("off") == RSX_UPSCALE_NONE &&
                       kz3_upscale_parse("0") == RSX_UPSCALE_NONE);
    check("unregistered-or-unknown-refused", kz3_upscale_parse("xess") < 0 && kz3_upscale_parse("dlss") < 0 &&
                                             kz3_upscale_parse("bogus") < 0 && kz3_upscale_parse("") < 0 &&
                                             kz3_upscale_parse("a-very-long-backend-name") < 0);

    s_upscale_eff = RSX_UPSCALE_FSR1;
    kz3_upscale_apply();
    check("fsr1-sharpness", last_backend == RSX_UPSCALE_FSR1 && last_sharpness == s_fsr1_sharpness);
    s_upscale_eff = RSX_UPSCALE_FSR3;
    kz3_upscale_apply();
    check("fsr3-sharpness", last_backend == RSX_UPSCALE_FSR3 && last_sharpness > 0.299f && last_sharpness < 0.301f);

    s_upscale_eff = RSX_UPSCALE_NONE;
    const bool off = kz3_upscale_temporal();
    s_upscale_eff = RSX_UPSCALE_FSR1;
    const bool spatial = kz3_upscale_temporal();
    s_upscale_eff = RSX_UPSCALE_FSR3;
    const bool fsr3 = kz3_upscale_temporal();
    s_upscale_eff = RSX_UPSCALE_XESS;
    const bool xess = kz3_upscale_temporal();
    check("camera-idle-without-temporal", !off && !spatial);
    check("camera-kept-for-temporal", fsr3 && xess);

    printf("%u checks, %u failed\n", checks, fails);
    return fails ? 1 : 0;
}
