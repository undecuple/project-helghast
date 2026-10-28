/* Host test for KZ3 patch 2099 (libs/video/rsx_d3d12_device.h): the RSX_D3D12_WARP rule. Hardware first and only
 * while it works; WARP when it gives no device (auto) or always (1); never with 0. No game data, no GPU. */
#include "rsx_d3d12_device.h"
#include <stdio.h>

static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    int bad = 9;
    check("unset = auto", rsx_warp_mode_parse(NULL, &bad) == RSX_WARP_AUTO && bad == 0);
    check("empty = auto", rsx_warp_mode_parse("", &bad) == RSX_WARP_AUTO && bad == 0);
    check("1 = force", rsx_warp_mode_parse("1", &bad) == RSX_WARP_FORCE && bad == 0);
    check("0 = never", rsx_warp_mode_parse("0", &bad) == RSX_WARP_NEVER && bad == 0);
    check("junk = auto, flagged", rsx_warp_mode_parse("yes", &bad) == RSX_WARP_AUTO && bad == 1);
    check("10 = auto, flagged", rsx_warp_mode_parse("10", &bad) == RSX_WARP_AUTO && bad == 1);
    check("auto tries hardware first", rsx_warp_try_hw(RSX_WARP_AUTO) && rsx_warp_try_hw(RSX_WARP_NEVER));
    check("force skips hardware", !rsx_warp_try_hw(RSX_WARP_FORCE));
    check("auto: no WARP while hardware works", !rsx_warp_try_warp(RSX_WARP_AUTO, 1));
    check("auto: WARP when hardware failed", rsx_warp_try_warp(RSX_WARP_AUTO, 0));
    check("never: no WARP", !rsx_warp_try_warp(RSX_WARP_NEVER, 0) && !rsx_warp_try_warp(RSX_WARP_NEVER, 1));
    check("force: WARP", rsx_warp_try_warp(RSX_WARP_FORCE, 0) && rsx_warp_try_warp(RSX_WARP_FORCE, 1));
    /* patch 2101: the one-line adapter summary (support.py GPU_RX must find it) */
    char l[400];
    rsx_adapter_line(l, sizeof l, "NVIDIA GeForce RTX 4070", 0x10DE, 0x2786, 12282, 0);
    check("adapter line, hardware", !strcmp(l, "[rsx] D3D12 adapter: NVIDIA GeForce RTX 4070 (vendor 0x10DE NVIDIA device 0x2786), 12282 MB, hardware"));
    rsx_adapter_line(l, sizeof l, "Microsoft Basic Render Driver", 0x1414, 0x8C, 0, 1);
    check("adapter line, WARP", strstr(l, "software (WARP)") && strstr(l, "vendor 0x1414 Microsoft"));
    rsx_adapter_line(l, sizeof l, "Radeon RX 7800 XT", 0x1002, 0x747E, 16368, 0);
    check("adapter line, AMD", strstr(l, "vendor 0x1002 AMD device 0x747E") && strstr(l, "16368 MB"));
    rsx_adapter_line(l, sizeof l, "?", 0, 0, 0, -1);
    check("adapter line, unknown", strstr(l, "vendor 0x0000 device 0x0000") && strstr(l, "unknown"));
    printf("%s\n", fails ? "FAIL" : "all ok");
    return fails != 0;
}
