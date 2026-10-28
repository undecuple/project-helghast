/* Host test for KZ3 patch 4020 (RSX pass 98): raw mouse input as keyboard+mouse activity for the button prompts
 * (PadMouseAct in libs/input/pad_mouse.h, fed by pad_mouse.c's WM_INPUT, consumed by cellPad.c's pad_prompt_track
 * through pad_last_device_update in pad_host.h). A mouse button is activity at once; motion needs
 * PAD_MOUSE_ACT_COUNTS counts inside one PAD_MOUSE_ACT_WINDOW_MS window; the existing pad/keyboard rules stay. No game data. */
#include "pad_host.h"
#include "pad_mouse.h"
#include <stdio.h>
#include <string.h>

#define PAD_MAX_HOST_PORTS 4   /* cellPad.c's value */
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
unsigned cellGcm_flip_request_count(void) { return 0; }   /* pad_mouse.c's frame clock (the exe: cellGcmSys.c) */

int main(void)
{
    PadMouseAct a;
    memset(&a, 0, sizeof a);
    check("nothing yet: no activity", !pad_mouse_act_take(&a));

    /* motion threshold */
    pad_mouse_act_motion(&a, 1, 0, 1000); pad_mouse_act_motion(&a, 0, -1, 1010); pad_mouse_act_motion(&a, 2, 1, 1020);
    check("a bump (5 counts in 20 ms) is not activity", !pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 0, 0, 1030);
    check("zero motion is not activity", !pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 5, 0, 2000); pad_mouse_act_motion(&a, 0, -3, 2050);
    check("8 counts inside the window (|5|+|-3|) is activity", pad_mouse_act_take(&a));
    check("take clears it", !pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 4, 0, 3000); pad_mouse_act_motion(&a, 4, 0, 3000 + PAD_MOUSE_ACT_WINDOW_MS + 1);
    check("4 + 4 counts a window apart: the window restarted, not activity", !pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 4, 0, 3000 + PAD_MOUSE_ACT_WINDOW_MS + 1 + PAD_MOUSE_ACT_WINDOW_MS);
    check("4 + 4 counts exactly one window apart count together", pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, -8, 0, 5000);
    check("one report at the threshold (negative direction)", pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 100, 100, 6000);
    check("one large report", pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 7, 0, 7000);
    check("7 counts: below the threshold", !pad_mouse_act_take(&a));
    pad_mouse_act_motion(&a, 1, 0, 7001);
    check("the 7 stays in the window: a 1-count nudge reaches 8", pad_mouse_act_take(&a));

    /* a button is activity at once, whatever the motion */
    pad_mouse_act_button(&a);
    check("a button is activity", pad_mouse_act_take(&a));
    check("button flags: left/right/middle/x1/x2 down and the wheels count, button ups do not",
          (PAD_MOUSE_ACT_BUTTON_MASK & 0x1) && (PAD_MOUSE_ACT_BUTTON_MASK & 0x4) && (PAD_MOUSE_ACT_BUTTON_MASK & 0x10) &&
          (PAD_MOUSE_ACT_BUTTON_MASK & 0x40) && (PAD_MOUSE_ACT_BUTTON_MASK & 0x100) && (PAD_MOUSE_ACT_BUTTON_MASK & 0x400) &&
          (PAD_MOUSE_ACT_BUTTON_MASK & 0x800) && !(PAD_MOUSE_ACT_BUTTON_MASK & (0x2 | 0x8 | 0x20 | 0x80 | 0x200)));

    /* into the prompt rule: mouse activity = keys_active; the pad rules are the old ones */
    PadHostState hs[PAD_MAX_HOST_PORTS];
    int port_dev[PAD_MAX_HOST_PORTS];
    memset(hs, 0, sizeof hs);
    memset(port_dev, 0, sizeof port_dev);
    hs[0].connected = 1;
    hs[0].analog_lx = hs[0].analog_ly = hs[0].analog_rx = hs[0].analog_ry = 128;   /* sticks centred */
    port_dev[0] = PAD_DEV_PS4;
    PadLastDevice l = { PAD_DEV_PS4, 0 };
    int changed = pad_last_device_update(&l, 0, hs, port_dev, PAD_MAX_HOST_PORTS);
    check("no activity: the pad stays the last device", !changed && l.dev == PAD_DEV_PS4);
    pad_mouse_act_button(&a);
    changed = pad_last_device_update(&l, pad_mouse_act_take(&a), hs, port_dev, PAD_MAX_HOST_PORTS);
    check("a mouse button flips the prompts to keyboard+mouse", changed && l.dev == PAD_DEV_KEYBOARD && l.port == -1);
    changed = pad_last_device_update(&l, pad_mouse_act_take(&a), hs, port_dev, PAD_MAX_HOST_PORTS);
    check("the next poll without activity keeps keyboard+mouse", !changed && l.dev == PAD_DEV_KEYBOARD);
    hs[0].buttons = 1;
    changed = pad_last_device_update(&l, 0, hs, port_dev, PAD_MAX_HOST_PORTS);
    check("pad input flips back to the pad", changed && l.dev == PAD_DEV_PS4 && l.port == 0);
    pad_mouse_act_motion(&a, 20, 0, 9000);
    changed = pad_last_device_update(&l, pad_mouse_act_take(&a), hs, port_dev, PAD_MAX_HOST_PORTS);
    check("mouse motion and a held pad button in one poll: the pad wins (as keyboard keys do)", !changed && l.dev == PAD_DEV_PS4);
    hs[0].buttons = 0;
    pad_mouse_act_motion(&a, 20, 0, 9100);
    changed = pad_last_device_update(&l, pad_mouse_act_take(&a), hs, port_dev, PAD_MAX_HOST_PORTS);
    check("mouse motion alone flips to keyboard+mouse", changed && l.dev == PAD_DEV_KEYBOARD);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
