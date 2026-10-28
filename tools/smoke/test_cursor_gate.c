/* Host test for KZ3 patch 2097 (cursor hiding and mouse lock, libs/input/pad_mouse.h pad_cursor_decide) and the
 * KZ3_MOUSE_LOOK=0 switch (pad_mouse.c; default on, test_mouse_look.c checks the default). No game data, no Win32. */
#include "pad_mouse.h"
#include <stdio.h>
#include <stdlib.h>

/* pad_mouse.c's look accumulator reads the guest frame clock (patch 2099; cellGcmSys.c in the exe): stub it here,
 * as test_mouse_look.c does. */
unsigned cellGcm_flip_request_count(void) { return 0; }

static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

/* mode, focused, minimized, sizemove, closing, look_on, hide_opt */
static PadCursorDo d(int mode, int foc, int min, int sm, int cl, int look, int hide, int over)
{
    PadCursorState s = { mode, foc, min, sm, cl, look, hide };
    return pad_cursor_decide(&s, over);
}
#define IS(r, h, c) ((r).hide == (h) && (r).clip == (c))

int main(void)
{
    for (int m = 0; m <= 2; m++) {
        char t[96];
        snprintf(t, sizeof t, "mode %d focused, look on, over client: hidden + locked", m);
        check(t, IS(d(m, 1, 0, 0, 0, 1, 1, 1), 1, 1));
        snprintf(t, sizeof t, "mode %d focused, look off: hidden, free", m);
        check(t, IS(d(m, 1, 0, 0, 0, 0, 1, 1), 1, 0));
        snprintf(t, sizeof t, "mode %d focus lost (Alt+Tab): shown, free", m);
        check(t, IS(d(m, 0, 0, 0, 0, 1, 1, 1), 0, 0));
        snprintf(t, sizeof t, "mode %d minimised: shown, free", m);
        check(t, IS(d(m, 1, 1, 0, 0, 1, 1, 1), 0, 0));
        snprintf(t, sizeof t, "mode %d closing: shown, free", m);
        check(t, IS(d(m, 1, 0, 0, 1, 1, 1, 1), 0, 0));
        snprintf(t, sizeof t, "mode %d KZ3_CURSOR_HIDE=0: shown, still locked with look on", m);
        check(t, IS(d(m, 1, 0, 0, 0, 1, 0, 1), 0, 1));
    }
    check("windowed, cursor on the title bar/frame: shown, locked", IS(d(0, 1, 0, 0, 0, 1, 1, 0), 0, 1));
    check("borderless, hit test not HTCLIENT: still hidden", IS(d(1, 1, 0, 0, 0, 1, 1, 0), 1, 1));
    check("fullscreen, hit test not HTCLIENT: still hidden", IS(d(2, 1, 0, 0, 0, 1, 1, 0), 1, 1));
    check("windowed title-bar drag / resize / Alt+Space menu: shown, free", IS(d(0, 1, 0, 1, 0, 1, 1, 1), 0, 0));
    check("drag ends, focused again: hidden + locked", IS(d(0, 1, 0, 0, 0, 1, 1, 1), 1, 1));

    setenv("KZ3_MOUSE_LOOK", "0", 1);
    check("KZ3_MOUSE_LOOK=0: look off", pad_mouse_enabled() == 0);

    printf(fails ? "[test_cursor_gate] FAIL (%d)\n" : "[test_cursor_gate] PASS\n", fails);
    return fails ? 1 : 0;
}
