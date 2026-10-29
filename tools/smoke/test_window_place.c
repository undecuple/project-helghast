/* Host test for KZ3 patch 2126 (libs/video/rsx_window_place.h in the patched toolkit): the close decision table, the
 * When unfocused rules, F11, the display.ini window keys (round trip) and where a saved window comes back (saved
 * monitor, moved or missing monitor -> primary centred, clamped to the work area). No game data, no GPU.
 *   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_window_place.sh */
#include <stdio.h>
#include <string.h>
#include "rsx_window_place.h"

static int s_n, s_bad;
static void check(int ok, const char* what)
{
    s_n++;
    if (!ok) s_bad++;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static void test_close(void)
{
    rsx_close_ctx c;
    memset(&c, 0, sizeof c);
    c.ui = 1;
    /* play: no menu -> pause first */
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_ASK_PAUSE, "X in play: pause menu, then dialog");
    c.menu = 1; c.ask_page = 1;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_ASK_MENU, "X on the main/pause menu: dialog");
    c.ask_page = 0;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_NOW, "X on a loading screen / movie / other page: close");
    c.ask_page = 1; c.busy = 1;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_NOW, "second X while asking or dialog up: close");
    c.busy = 0; c.ui = 0;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_NOW, "X during boot (no menu tracking): close");
    c.ui = 1; c.fault = 1;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_NOW, "X on the crash path: close");
    c.fault = 0; c.off = 1;
    check(rsx_close_decide(RSX_CLOSE_SRC_USER, &c) == RSX_CLOSE_NOW, "KZ3_CLOSE_ASK=0: close");
    c.off = 0;
    check(rsx_close_decide(RSX_CLOSE_SRC_SCRIPT, &c) == RSX_CLOSE_NOW, "bare WM_CLOSE (scripts, the dialog's Yes): close");
    check(rsx_close_decide(RSX_CLOSE_SRC_ENDSESSION, &c) == RSX_CLOSE_NOW, "WM_ENDSESSION: close");
    c.menu = 0;
    check(rsx_close_decide(RSX_CLOSE_SRC_SCRIPT, &c) == RSX_CLOSE_NOW, "bare WM_CLOSE in play: close");
}

static void test_unfocused(void)
{
    check(rsx_unf_parse("keep") == RSX_UNF_KEEP && rsx_unf_parse(" Mute ") == RSX_UNF_MUTE && rsx_unf_parse("PAUSE") == RSX_UNF_PAUSE &&
          rsx_unf_parse("x") == -1 && rsx_unf_parse(NULL) == -1, "unfocused names");
    check(rsx_dm_ini_unfocused("[display]\nunfocused = pause\n") == RSX_UNF_PAUSE && rsx_dm_ini_unfocused("[display]\n") == -1,
          "unfocused ini key");
    check(!rsx_unf_muted(RSX_UNF_KEEP, 0, 1) && rsx_unf_muted(RSX_UNF_MUTE, 0, 0) && rsx_unf_muted(RSX_UNF_MUTE, 1, 1) &&
          !rsx_unf_muted(RSX_UNF_MUTE, 1, 0) && !rsx_unf_muted(RSX_UNF_PAUSE, 0, 0), "mute only when set and unfocused or minimised");
    check(rsx_unf_pause_now(RSX_UNF_PAUSE, 1, 0, 1) && !rsx_unf_pause_now(RSX_UNF_PAUSE, 0, 0, 1) &&
          !rsx_unf_pause_now(RSX_UNF_PAUSE, 1, 0, 0) && !rsx_unf_pause_now(RSX_UNF_PAUSE, 1, 1, 1) &&
          !rsx_unf_pause_now(RSX_UNF_MUTE, 1, 0, 1), "pause once on focus loss, in play only");
    check(rsx_f11_toggles(0) && !rsx_f11_toggles(1), "F11 toggles unless bound");
    check(rsx_dm_ini_maximize("[display]\nmaximize = window\n") == RSX_MAX_WINDOW &&
          rsx_dm_ini_maximize("[display]\nmaximize = borderless\n") == RSX_MAX_BORDERLESS &&
          rsx_dm_ini_maximize("[display]\n") == -1, "maximize ini key");
}

static void test_ini(void)
{
    rsx_wp a, b;
    char buf[1024], file[1200];
    memset(&a, 0, sizeof a);
    a.valid = 1; a.x = -1900; a.y = 40; a.w = 1600; a.h = 900; a.maximized = 1;
    snprintf(a.mon, sizeof a.mon, "%s", "\\\\.\\DISPLAY2");
    a.ml = -1920; a.mt = 0; a.mr = 0; a.mb = 1080;
    check(rsx_wp_ini_write(buf, sizeof buf, &a) > 0, "window keys written");
    snprintf(file, sizeof file, "[display]\nmode = windowed\n%s", buf);
    check(rsx_wp_ini_read(file, &b) && b.x == a.x && b.y == a.y && b.w == a.w && b.h == a.h && b.maximized == 1 &&
          !strcmp(b.mon, a.mon) && b.ml == a.ml && b.mt == a.mt && b.mr == a.mr && b.mb == a.mb, "window keys round trip");
    check(!rsx_wp_ini_read("[display]\nwindow_x = 1\nwindow_y = 2\nwindow_width = 100\nwindow_height = 50\n", &b),
          "too small a client is no placement");
    check(!rsx_wp_ini_read("[display]\nmode = windowed\n", &b) && !b.valid, "no keys: no placement");
    check(!rsx_wp_ini_read("[other]\nwindow_x = 1\nwindow_y = 2\nwindow_width = 800\nwindow_height = 450\n", &b),
          "keys outside [display] are ignored");
    a.valid = 0;
    check(rsx_wp_ini_write(buf, sizeof buf, &a) == 0 && buf[0] == 0, "no placement writes nothing");
}

static void test_resolve(void)
{
    /* primary 1920x1080 with a 40 px taskbar; a second 2560x1440 to the left */
    rsx_wp_mon m[2] = {
        {"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, 0, 0, 1920, 1040, 1},
        {"\\\\.\\DISPLAY2", -2560, 0, 0, 1440, -2560, 0, 0, 1440, 0},
    };
    const int fw = 16, fh = 39;
    rsx_wp w;
    int out[4], fb = -1, k;
    memset(&w, 0, sizeof w);
    w.valid = 1; w.x = -2400; w.y = 100; w.w = 1600; w.h = 900;
    snprintf(w.mon, sizeof w.mon, "%s", "\\\\.\\DISPLAY2");
    w.ml = -2560; w.mt = 0; w.mr = 0; w.mb = 1440;
    k = rsx_wp_resolve(&w, m, 2, fw, fh, 320, 180, out, &fb);
    check(k == 1 && !fb && out[0] == -2400 && out[1] == 100 && out[2] == 1600 && out[3] == 900, "saved monitor: as saved");
    w.x = -500;   /* hangs off the right edge of DISPLAY2 */
    k = rsx_wp_resolve(&w, m, 2, fw, fh, 320, 180, out, &fb);
    check(k == 1 && !fb && out[0] == 0 - (1600 + fw) && out[1] == 100, "saved monitor: clamped into its work area");
    w.x = -2400;
    w.mr = 1;     /* the monitor's rectangle changed (moved or other resolution) */
    k = rsx_wp_resolve(&w, m, 2, fw, fh, 320, 180, out, &fb);
    check(k == 0 && fb && out[2] == 1600 && out[3] == 900 && out[0] == (1920 - (1600 + fw)) / 2 && out[1] == (1040 - (900 + fh)) / 2,
          "moved monitor: primary, centred");
    snprintf(w.mon, sizeof w.mon, "%s", "\\\\.\\DISPLAY9");
    w.mr = 0;
    k = rsx_wp_resolve(&w, m, 2, fw, fh, 320, 180, out, &fb);
    check(k == 0 && fb, "missing monitor: primary");
    w.w = 2560; w.h = 1440;   /* larger than the primary's work area: shrunk, aspect kept */
    k = rsx_wp_resolve(&w, m, 2, fw, fh, 320, 180, out, &fb);
    check(k == 0 && out[2] + fw <= 1920 && out[3] + fh <= 1040 && out[3] == 1040 - fh &&
          out[2] == (int)((long long)(1040 - fh) * 2560 / 1440) && out[0] >= 0 && out[1] >= 0, "too large: shrunk to the work area, aspect kept");
    check(rsx_wp_resolve(&w, m, 0, fw, fh, 320, 180, out, &fb) == -1, "no monitors: nothing");
    {
        rsx_wp_mon one[1] = {{"\\\\.\\DISPLAY1", 0, 0, 1280, 720, 0, 0, 1280, 720, 0}};   /* no primary flag */
        w.w = 800; w.h = 450;
        k = rsx_wp_resolve(&w, one, 1, fw, fh, 320, 180, out, &fb);
        check(k == 0 && fb && out[2] == 800 && out[3] == 450, "no primary flag: the first monitor");
    }
}

int main(void)
{
    test_close();
    test_unfocused();
    test_ini();
    test_resolve();
    printf("test_window_place: %d checks, %d failed\n", s_n, s_bad);
    return s_bad ? 1 : 0;
}
