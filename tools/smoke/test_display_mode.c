/* Host test for KZ3 patch 2095 (libs/video/rsx_display_mode.h in the patched toolkit): display-mode names, the mode
 * state machine (Options choice, Alt+Enter toggle, exclusive refusal), the letterbox/pillarbox rectangle, client ->
 * guest mouse mapping, display.ini text, and the Options row's GUM patch (on a synthetic page; KZ3_DM_GUM=<file> also
 * patches a real displaysettings.gum when given). Platform-free; run by test_display_mode.sh. */
#include "rsx_display_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        const int ok_ = (c) ? 1 : 0;                                                                                   \
        printf(ok_ ? "[PASS] " : "[FAIL] ");                                                                           \
        if (!ok_) fails++;                                                                                             \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static int rect_is(rsx_dm_rect r, int x, int y, int w, int h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void test_names(void)
{
    CHECK(rsx_dm_parse("windowed") == RSX_DM_WINDOWED && rsx_dm_parse("Window") == RSX_DM_WINDOWED, "windowed names");
    CHECK(rsx_dm_parse("BORDERLESS") == RSX_DM_BORDERLESS && rsx_dm_parse(" borderless\r\n") == RSX_DM_BORDERLESS,
          "borderless, case and blanks");
    CHECK(rsx_dm_parse("fullscreen") == RSX_DM_FULLSCREEN && rsx_dm_parse("exclusive") == RSX_DM_FULLSCREEN,
          "fullscreen names");
    CHECK(rsx_dm_parse("") < 0 && rsx_dm_parse("full") < 0 && rsx_dm_parse("windowedx") < 0 && rsx_dm_parse(NULL) < 0 &&
              rsx_dm_parse("fullscreen2") < 0,
          "bad names refused");
    CHECK(!strcmp(rsx_dm_name(RSX_DM_WINDOWED), "windowed") && !strcmp(rsx_dm_name(RSX_DM_BORDERLESS), "borderless") &&
              !strcmp(rsx_dm_name(RSX_DM_FULLSCREEN), "fullscreen"),
          "names round-trip");
}

static void test_states(void)
{
    rsx_dm_state s;
    rsx_dm_init(&s, RSX_DM_WINDOWED);
    CHECK(s.mode == RSX_DM_WINDOWED && s.last_fs == RSX_DM_BORDERLESS, "start windowed, Alt+Enter target borderless");
    CHECK(rsx_dm_toggle(&s) == RSX_DM_BORDERLESS && s.mode == RSX_DM_BORDERLESS, "Alt+Enter: windowed -> borderless");
    CHECK(rsx_dm_toggle(&s) == RSX_DM_WINDOWED && s.mode == RSX_DM_WINDOWED, "Alt+Enter: borderless -> windowed");
    CHECK(rsx_dm_set(&s, RSX_DM_FULLSCREEN) == RSX_DM_FULLSCREEN && s.last_fs == RSX_DM_FULLSCREEN,
          "Options: fullscreen becomes the Alt+Enter target");
    CHECK(rsx_dm_toggle(&s) == RSX_DM_WINDOWED && rsx_dm_toggle(&s) == RSX_DM_FULLSCREEN,
          "Alt+Enter twice: back to fullscreen");
    CHECK(rsx_dm_set(&s, RSX_DM_WINDOWED) == RSX_DM_WINDOWED && s.last_fs == RSX_DM_FULLSCREEN,
          "Options windowed keeps the last fullscreen mode");
    CHECK(rsx_dm_set(&s, 7) == RSX_DM_WINDOWED && s.mode == RSX_DM_WINDOWED, "out-of-range choice ignored");
    rsx_dm_set(&s, RSX_DM_FULLSCREEN);
    CHECK(rsx_dm_exclusive_refused(&s) == RSX_DM_BORDERLESS && s.mode == RSX_DM_FULLSCREEN,
          "exclusive refused: borderless applied, the choice stays fullscreen");
    CHECK(rsx_dm_toggle(&s) == RSX_DM_WINDOWED && rsx_dm_toggle(&s) == RSX_DM_BORDERLESS,
          "after a refusal Alt+Enter goes to borderless");
    CHECK(rsx_dm_set(&s, RSX_DM_FULLSCREEN) == RSX_DM_BORDERLESS, "after a refusal Options fullscreen = borderless");
    rsx_dm_init(&s, RSX_DM_FULLSCREEN);
    CHECK(s.last_fs == RSX_DM_FULLSCREEN && rsx_dm_toggle(&s) == RSX_DM_WINDOWED &&
              rsx_dm_toggle(&s) == RSX_DM_FULLSCREEN,
          "start fullscreen: Alt+Enter returns to fullscreen");
    rsx_dm_init(&s, 9);
    CHECK(s.mode == RSX_DM_WINDOWED, "bad start mode = windowed");
}

static void test_fit(void)
{
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1280, 720), 0, 0, 1280, 720), "same size = whole target");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1920, 1080), 0, 0, 1920, 1080), "16:9 monitor: no bars");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1920, 1200), 0, 60, 1920, 1080), "16:10: letterbox 60 + 60");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 2560, 1080), 320, 0, 1920, 1080), "21:9: pillarbox 320 + 320");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1024, 768), 0, 96, 1024, 576), "4:3: letterbox 96 + 96");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1600, 1000), 0, 50, 1600, 900), "1600x1000 window: letterbox 50 + 50");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1281, 720), 0, 0, 1280, 720), "one pixel wider: the odd pixel on the right");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 1283, 720), 1, 0, 1281, 720) ||
              rect_is(rsx_dm_fit(1280, 720, 1283, 720), 1, 0, 1280, 720),
          "three pixels wider: bars 1 + 2");
    CHECK(rect_is(rsx_dm_fit(1280, 720, 0, 0), 0, 0, 0, 0) && rect_is(rsx_dm_fit(0, 0, 800, 600), 0, 0, 800, 600),
          "zero sizes: the whole target");
    { /* every target size from 160x90 to 2000x1300 (step 7): inside, centred to a pixel, aspect within half a pixel */
        int bad = 0, dw, dh;
        for (dw = 160; dw <= 2000 && !bad; dw += 7)
            for (dh = 90; dh <= 1300 && !bad; dh += 7) {
                rsx_dm_rect r = rsx_dm_fit(1280, 720, dw, dh);
                double ar = (double)r.w / r.h, want = 1280.0 / 720.0;
                int full_w = r.w == dw, full_h = r.h == dh;
                if (r.x < 0 || r.y < 0 || r.x + r.w > dw || r.y + r.h > dh) bad = 1;
                if (!full_w && !full_h) bad = 2;
                if (abs((dw - r.w) - 2 * r.x) > 1 || abs((dh - r.h) - 2 * r.y) > 1) bad = 3;
                if (full_w && (r.h * want < r.w - want || r.h * want > r.w + want)) bad = 4;
                if (full_h && (ar * r.h < r.w - 0.5 - 1e-9 && r.w + 0.5 < want * r.h)) bad = 5;
                if (bad) printf("  failing target %dx%d -> %d,%d %dx%d (%d)\n", dw, dh, r.x, r.y, r.w, r.h, bad);
            }
        CHECK(!bad, "sweep of target sizes: inside, centred, one side full, aspect kept");
    }
}

static void test_scaling(void)
{
    CHECK(RSX_DM_SCALING_DEFAULT == RSX_DM_STRETCH, "image scaling defaults to Stretch, Fit stays selectable");
    CHECK(rsx_dm_parse_scaling("fit") == RSX_DM_FIT && rsx_dm_parse_scaling(" Stretch ") == RSX_DM_STRETCH &&
              rsx_dm_parse_scaling("fill") < 0 && rsx_dm_parse_filter("bilinear") == RSX_DM_BILINEAR &&
              rsx_dm_parse_filter("NEAREST") == RSX_DM_NEAREST && rsx_dm_parse_filter("fsr") < 0 &&
              rsx_dm_parse_scaling(NULL) < 0,
          "scaling/filter names");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1200, RSX_DM_STRETCH, RSX_DM_BILINEAR), 0, 0, 1920, 1200),
          "stretch: whole client");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1200, RSX_DM_FIT, RSX_DM_BILINEAR), 0, 60, 1920, 1080),
          "fit bilinear = rsx_dm_fit");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1080, RSX_DM_FIT, RSX_DM_NEAREST), 320, 180, 1280, 720),
          "nearest at 1080p: 1x centred (1.5x is not whole)");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 2560, 1440, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 2560, 1440),
          "nearest at 1440p: 2x fills");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 3840, 2160, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 3840, 2160),
          "nearest at 2160p: 3x fills");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 960, 540, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 960, 540),
          "nearest below 1x: plain fit");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1080, RSX_DM_STRETCH, RSX_DM_NEAREST), 0, 0, 1920, 1080),
          "stretch + nearest: whole client");
    {
        rsx_dm_rect r = rsx_dm_rect_for(1280, 720, 1600, 1200, RSX_DM_STRETCH, RSX_DM_BILINEAR);
        int gx, gy;
        int in = rsx_dm_to_guest(&r, 1280, 720, 1599, 1199, &gx, &gy);
        CHECK(in && gx == 1279 && gy == 719, "stretch: mouse maps each axis on its own (got %d,%d)", gx, gy);
    }
}

static void test_mouse(void)
{
    rsx_dm_rect r = rsx_dm_fit(1280, 720, 1920, 1200); /* 0,60 1920x1080, scale 1.5 */
    int gx, gy, in, x, ok = 1, last = -1;
    in = rsx_dm_to_guest(&r, 1280, 720, 960, 600, &gx, &gy);
    CHECK(in && gx == 640 && gy == 360, "centre of a 16:10 screen -> guest 640,360 (got %d,%d)", gx, gy);
    in = rsx_dm_to_guest(&r, 1280, 720, 0, 60, &gx, &gy);
    CHECK(in && gx == 0 && gy == 0, "image top-left -> 0,0");
    in = rsx_dm_to_guest(&r, 1280, 720, 1919, 1139, &gx, &gy);
    CHECK(in && gx == 1279 && gy == 719, "image bottom-right -> 1279,719 (got %d,%d)", gx, gy);
    in = rsx_dm_to_guest(&r, 1280, 720, 100, 10, &gx, &gy);
    CHECK(!in && gy == 0 && gx == 67, "top bar: outside, clamped to the top row (got %d,%d)", gx, gy);
    in = rsx_dm_to_guest(&r, 1280, 720, 100, 1199, &gx, &gy);
    CHECK(!in && gy == 719, "bottom bar: outside, clamped to the bottom row");
    for (x = 0; x < 1920; x++) { /* monotone, every guest column reached */
        rsx_dm_to_guest(&r, 1280, 720, x, 600, &gx, &gy);
        if (gx < last || gx > last + 1) ok = 0;
        last = gx;
    }
    CHECK(ok && last == 1279, "1.5x: client columns map monotonically onto all 1280 guest columns");
    r = rsx_dm_fit(1280, 720, 1280, 720);
    ok = 1;
    for (x = 0; x < 1280; x += 3) {
        rsx_dm_to_guest(&r, 1280, 720, x, x % 720, &gx, &gy);
        if (gx != x || gy != x % 720) ok = 0;
    }
    CHECK(ok, "unscaled: identity");
    r = rsx_dm_fit(1280, 720, 2560, 1080); /* pillarbox 320,0 1920x1080 */
    in = rsx_dm_to_guest(&r, 1280, 720, 319, 540, &gx, &gy);
    CHECK(!in && gx == 0, "left pillar: outside, clamped to column 0");
    in = rsx_dm_to_guest(&r, 1280, 720, 320 + 1919, 540, &gx, &gy);
    CHECK(in && gx == 1279 && gy == 360, "right image edge -> 1279 (got %d,%d)", gx, gy);
}

static void test_ini(void)
{
    char buf[512];
    int n = rsx_dm_ini_write(buf, sizeof buf, RSX_DM_BORDERLESS, RSX_DM_STRETCH, RSX_DM_NEAREST, RSX_DM_VSYNC_OFF), m;
    CHECK(n > 0 && n < (int)sizeof buf && rsx_dm_ini_parse(buf) == RSX_DM_BORDERLESS, "write/read borderless");
    for (m = 0; m <= 2; m++) {
        rsx_dm_ini_write(buf, sizeof buf, m, m & 1, (m >> 1) & 1, m != 1);
        if (rsx_dm_ini_parse(buf) != m || rsx_dm_ini_scaling(buf) != (m & 1) ||
            rsx_dm_ini_filter(buf) != ((m >> 1) & 1) || rsx_dm_ini_vsync(buf) != (m != 1))
            break;
    }
    CHECK(m == 3, "all three modes round-trip (with scaling and filter)");
    CHECK(rsx_dm_ini_scaling(buf) >= 0 && rsx_dm_ini_scaling("[display]\nmode = borderless\n") < 0 &&
              rsx_dm_ini_filter("[display]\nfilter = Point\n") == RSX_DM_NEAREST &&
              rsx_dm_ini_scaling("[display]\nscaling=Stretch\r\n") == RSX_DM_STRETCH,
          "scaling/filter keys: missing = none, aliases, case");
    CHECK(n > 0 && rsx_dm_ini_vsync(buf) >= 0, "vsync key written");
    rsx_dm_ini_write(buf, sizeof buf, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, RSX_DM_VSYNC_OFF);
    CHECK(rsx_dm_ini_vsync(buf) == RSX_DM_VSYNC_OFF && strstr(buf, "vsync = off"), "vsync off round-trips");
    CHECK(rsx_dm_ini_vsync("[display]\nmode = borderless\n") < 0 && rsx_dm_ini_vsync("[display]\nVSync=ON\r\n") == RSX_DM_VSYNC_ON &&
              rsx_dm_ini_vsync("[display]\nvsync = 0\n") == RSX_DM_VSYNC_OFF && rsx_dm_ini_vsync("[display]\nvsync = maybe\n") < 0 &&
              rsx_dm_ini_vsync("[other]\nvsync = off\n[display]\n") < 0 && rsx_dm_ini_vsync(NULL) < 0,
          "vsync key: missing/bad = none (default on), aliases, case, section");
    {
        unsigned tr;
        CHECK(rsx_dm_present_args(1, 1, 0, -1, &tr) == 1 && tr == 0, "vsync on: interval 1, no flag (tearing-capable chain too)");
        CHECK(rsx_dm_present_args(0, 1, 0, -1, &tr) == 0 && tr == 1, "vsync off, tearing supported, windowed: interval 0 + ALLOW_TEARING");
        CHECK(rsx_dm_present_args(0, 0, 0, -1, &tr) == 0 && tr == 0, "vsync off, no tearing support: plain interval 0");
        CHECK(rsx_dm_present_args(0, 1, 1, -1, &tr) == 0 && tr == 0, "vsync off, exclusive fullscreen: interval 0 without the flag");
        CHECK(rsx_dm_present_args(0, 1, 0, 2, &tr) == 2 && tr == 0 && rsx_dm_present_args(1, 1, 0, 0, &tr) == 0 && tr == 0,
              "LD_PRESENT_INTERVAL wins either way, no flag");
    }
    CHECK(rsx_dm_ini_parse("[other]\nmode = fullscreen\n[display]\r\n  mode=Fullscreen \r\n") == RSX_DM_FULLSCREEN,
          "other section skipped, CRLF, blanks, case");
    CHECK(rsx_dm_ini_parse("[display]\nmodex = borderless\nmode = bogus\n") < 0, "bad key/value -> none");
    CHECK(rsx_dm_ini_parse("mode = borderless\n") < 0 && rsx_dm_ini_parse("") < 0 && rsx_dm_ini_parse(NULL) < 0,
          "no section -> none");
}

static const char page[] =
    "<gum default-focus=\"slc_crosshair\">\n\t<panel class=\"safearea\">\n\t\t<list id=\"lst_options\" "
    "class=\"PEDS\">\n"
    "\t\t\t<select id=\"slc_hints\" label=\"x\">\n\t\t\t\t<selectitem "
    "value=\"false\">off</selectitem>\n\t\t\t</select>\n"
    "\t\t\t\n\t\t\t<listitem id=\"lst_gamma_screen\" label=\"g\" onaccept=\"open(file:///common/gamma.gum)\"/>\n"
    "\t</list>\n\t</panel>\n\t<script id=\"settingscript\">\n<![CDATA[\t\t\n\t\tfunction OnDialogClose(e)\n"
    "\t\t\tif (e:GetSender():GetValue() == \"yes\") then\n\t\t\t\tgPlayerProfile:SetDefaultDisplay()\n\t\t\tend\n\t\tend\n"
    "\t\tfunction OnPageOff()\n\t\tend\n]]>\n"
    "\t</script>\n</gum>\n";

static void test_gum(void)
{
    size_t n = 0, n2 = 0;
    char* out = rsx_dm_patch_gum(page, sizeof page - 1, &n);
    CHECK(out != NULL, "synthetic display page patched");
    if (out) {
        const char* row = strstr(out, "<select id=\"slc_kz3_display\"");
        const char* gam = strstr(out, "<listitem id=\"lst_gamma_screen\"");
        const char* lua = strstr(out, "function KZ3ChangeDisplay");
        const char* on = strstr(out, "function OnPageOn");
        const char* cd = strstr(out, "]]>");
        const char* off = strstr(out, "function OnPageOff");
        CHECK(row && gam && row < gam && strstr(row, "</select>") < gam, "row right before the Brightness row");
        CHECK(lua && on && cd && off && off < on && lua < cd, "functions at the end of the settingscript CDATA");
        CHECK(n == strlen(out) && strstr(out, "Windowed</selectitem>") && strstr(out, "Borderless</selectitem>") &&
                  strstr(out, "Fullscreen</selectitem>") && strstr(out, "kb:\"..c") && strstr(out, "\"display \"..e"),
              "three values, bridge command, length");
        CHECK(strstr(out, "slc_kz3_scaling") && strstr(out, "slc_kz3_filter") && strstr(out, "slc_kz3_filter") < gam &&
                  strstr(out, "Stretch</selectitem>") && strstr(out, "Nearest</selectitem>") &&
                  strstr(out, "\"scaling \"..e") && strstr(out, "\"filter \"..e") &&
                  strstr(out, "KZ3DisplayCmd(\"filter\")"),
              "Image scaling and Scaling filter rows with their functions");
        CHECK(strstr(out, "slc_kz3_vsync") && strstr(out, "slc_kz3_vsync") < gam && strstr(out, "slc_kz3_vsync") > strstr(out, "slc_kz3_filter") &&
                  strstr(out, "label=\"VSync\"") && strstr(out, ">On</selectitem>") && strstr(out, ">Off</selectitem>") &&
                  strstr(out, "\"vsync \"..e") && strstr(out, "KZ3DisplayCmd(\"vsync\")") && strstr(out, "function KZ3ChangeVSync"),
              "VSync row after Scaling filter, with its function");
        CHECK(rsx_dm_patch_gum(out, n, &n2) == NULL, "already patched: served unchanged");
        free(out);
    }
    /* patch 2107: Frame rate limit row + its warning dialog always; the Motion blur row only with the hook */
    out = rsx_dm_patch_gum(page, sizeof page - 1, &n);
    if (out) {
        const char* gam = strstr(out, "<listitem id=\"lst_gamma_screen\"");
        const char* dlg = strstr(out, "<dialog id=\"dlg_kz3_fps\"");
        const char* scr = strstr(out, "<script id=\"settingscript\">");
        CHECK(strstr(out, "slc_kz3_fps") && strstr(out, "slc_kz3_fps") < gam && strstr(out, "slc_kz3_fps") > strstr(out, "slc_kz3_vsync") &&
                  strstr(out, ">30 FPS</selectitem>") && strstr(out, ">60 FPS</selectitem>") && strstr(out, ">120 FPS</selectitem>") &&
                  strstr(out, "default-value=\"30\""), "2107: Frame rate limit row after VSync, 30/60/120, default 30");
        CHECK(dlg && scr && dlg < scr && strstr(dlg, "default-focus=\"dlg_kz3_fps_no\"") && strstr(dlg, "@key{menu_dialog_yes}") &&
                  strstr(out, "dialog(dlg_kz3_fps, settingscript, OnKZ3FpsClose)") && strstr(out, "function OnKZ3FpsClose") &&
                  strstr(out, "KZ3DisplayCmd(\"fps 30\")") && strstr(out, "\"fps \"..gKz3FpsWant"),
              "2107: the warning dialog before the settingscript, No focused; 30 applies at once, 60/120 after Yes");
        CHECK(!strstr(out, "<select id=\"slc_kz3_mblur\"") && !strstr(out, "function KZ3MBlurOn") && strstr(out, "if (KZ3MBlurOn) then"),
              "2107: no Motion blur row without the hook (OnPageOn tests for its function)");
        free(out);
    }
    out = rsx_dm_patch_gum_opts(page, sizeof page - 1, &n, RSX_DM_GUM_MBLUR);
    CHECK(out && n == strlen(out), "2107: page with the Motion blur row patched");
    if (out) {
        const char* gam = strstr(out, "<listitem id=\"lst_gamma_screen\"");
        const char* cd = strstr(out, "]]>");
        CHECK(strstr(out, "slc_kz3_mblur") && strstr(out, "slc_kz3_mblur") < gam && strstr(out, "label=\"Motion blur\"") &&
                  strstr(out, "function KZ3MBlurOn") && strstr(out, "function KZ3MBlurOn") < cd &&
                  strstr(out, "\"motionblur \"..e") && strstr(out, "KZ3DisplayCmd(\"motionblur\")"),
              "2107: Motion blur row and its functions");
        free(out);
    }
    {
        char ib[1024];
        int k2 = rsx_dm_ini_write_ext(ib, sizeof ib, RSX_DM_FULLSCREEN, RSX_DM_FIT, RSX_DM_NEAREST, 1, 0, RSX_DM_FPS_120);
        CHECK(k2 > 0 && k2 < (int)sizeof ib && rsx_dm_ini_parse(ib) == RSX_DM_FULLSCREEN && rsx_dm_ini_vsync(ib) == 1 &&
                  rsx_dm_ini_motion_blur(ib) == 0 && rsx_dm_ini_fps(ib) == 120, "2107: ini round trip motion_blur off, fps 120");
        rsx_dm_ini_write(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1);
        CHECK(rsx_dm_ini_motion_blur(ib) == -1 && rsx_dm_ini_fps(ib) == -1, "2107: an old file has neither key (defaults apply)");
        CHECK(rsx_dm_ini_fps("[display]\nfps_limit = 45\n") == -1 && rsx_dm_ini_fps("[display]\nfps_limit=60\n") == 60,
              "2107: fps_limit 45 refused, 60 read");
        CHECK(rsx_dm_fps_vblank_ms(30) == 16 && rsx_dm_fps_vblank_ms(60) == 8 && rsx_dm_fps_vblank_ms(120) == 4,
              "2107: guest vblank 16 ms (30, as before), 8 ms (60), 4 ms (120)");
    }
    /* patch 2109 (RSX pass 103): Field of view row (only with the camera hook) and the display.ini fov key */
    out = rsx_dm_patch_gum(page, sizeof page - 1, &n);
    if (out) {
        CHECK(strstr(out, "<list id=\"lst_options\" class=\"PEDS\" height=\"400\"") != NULL,
              "2110: the list always gets the game's 10-row height (1.14 has 11 rows without the optional ones)");
        CHECK(!strstr(out, "<select id=\"slc_kz3_fov\"") && !strstr(out, "function KZ3FovOn") && strstr(out, "if (KZ3FovOn) then"),
              "2109: no Field of view row without the hook (OnPageOn tests for its function)");
        free(out);
    }
    out = rsx_dm_patch_gum_opts(page, sizeof page - 1, &n, RSX_DM_GUM_MBLUR | RSX_DM_GUM_FOV);
    CHECK(out && n == strlen(out), "2109: page with the Motion blur and Field of view rows patched");
    if (out) {
        const char* gam = strstr(out, "<listitem id=\"lst_gamma_screen\"");
        const char* cd = strstr(out, "]]>");
        const char* fv = strstr(out, "slc_kz3_fov");
        CHECK(fv && fv < gam && fv > strstr(out, "slc_kz3_mblur") && strstr(out, "label=\"Field of view\"") &&
                  strstr(out, "default-value=\"default\"") && strstr(out, ">Default</selectitem>") &&
                  strstr(out, "<selectitem value=\"70\">70</selectitem>") && strstr(out, "<selectitem value=\"110\">110</selectitem>") &&
                  strstr(out, "function KZ3FovOn") && strstr(out, "function KZ3FovOn") < cd && strstr(out, "\"fov \"..e") &&
                  strstr(out, "KZ3DisplayCmd(\"fov\")"),
              "2109: Field of view row after Motion blur, Default and 70..110, with its functions");
        CHECK(strstr(out, "<list id=\"lst_options\" class=\"PEDS\" height=\"400\"") != NULL,
              "2109: the 12-row list gets a height (scrolls instead of running into the legend)");
        free(out);
    }
    {
        char ib[1024], fb[16];
        int k2 = rsx_dm_ini_write_ext2(ib, sizeof ib, RSX_DM_FULLSCREEN, RSX_DM_FIT, RSX_DM_NEAREST, 1, 0, RSX_DM_FPS_60, 95);
        CHECK(k2 > 0 && k2 < (int)sizeof ib && (size_t)k2 == strlen(ib) && rsx_dm_ini_fov(ib) == 95 && rsx_dm_ini_fps(ib) == 60 &&
                  rsx_dm_ini_motion_blur(ib) == 0, "2109: ini round trip fov 95 (other keys kept)");
        rsx_dm_ini_write_ext2(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 1, RSX_DM_FPS_30, 0);
        CHECK(rsx_dm_ini_fov(ib) == 0 && strstr(ib, "fov = default\n"), "2109: default written as 'default', read as 0");
        rsx_dm_ini_write_ext(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 1, RSX_DM_FPS_30);
        CHECK(rsx_dm_ini_fov(ib) == -1, "2109: an older file has no fov key (default applies)");
        CHECK(rsx_dm_parse_fov("69") == -1 && rsx_dm_parse_fov("111") == -1 && rsx_dm_parse_fov("83") == 83 &&
                  rsx_dm_parse_fov(" Off ") == 0 && rsx_dm_parse_fov("9x") == -1 && rsx_dm_parse_fov("") == -1,
              "2109: fov 70..110 or default/off; others refused");
        CHECK(!strcmp(rsx_dm_fov_name(0, fb, sizeof fb), "default") && !strcmp(rsx_dm_fov_name(105, fb, sizeof fb), "105"),
              "2109: names");
    }
    /* patch 2110 (RSX pass 104): row tooltips in the game's tooltip panel, Reset to defaults resets the port's rows,
     * the shorter frame rate warning */
    out = rsx_dm_patch_gum_opts(page, sizeof page - 1, &n, RSX_DM_GUM_MBLUR | RSX_DM_GUM_FOV);
    CHECK(out && n == strlen(out), "2110: page patched");
    if (out) {
        const char* le = strstr(out, "</list>");
        const char* tip = strstr(out, "<panel id=\"pnl_kz3_tooltip\" class=\"tooltip\" y=\"440\">");
        const char* sd = strstr(out, "gPlayerProfile:SetDefaultDisplay()");
        const char* rc = sd ? strstr(sd, "KZ3ResetDisplay()") : NULL;
        const char* rf = strstr(out, "function KZ3ResetDisplay()");
        const char* cd = strstr(out, "]]>");
        CHECK(strstr(out, "class=\"PEDS\" height=\"400\" onchange=\"script(settingscript,KZ3Tooltip)\">") != NULL,
              "2110: the list's onchange fills the tooltip");
        CHECK(le && tip && tip > le && tip < strstr(out, "</panel>"), "2110: tooltip panel right after the list, in the page panel");
        CHECK(strstr(out, "function KZ3Tooltip(e)") && strstr(out, "slc_kz3_fps = \"Killzone 3 was made for 30") &&
              strstr(out, "slc_kz3_fov = ") && strstr(out, "slc_kz3_mblur = ") && strstr(out, "slc_kz3_display = ") &&
              strstr(out, "slc_kz3_scaling = ") && strstr(out, "slc_kz3_filter = ") && strstr(out, "slc_kz3_vsync = "),
              "2110: a tooltip for every port row");
        CHECK(rc && rc < strstr(sd, "end") && rf && rf < cd, "2110: the game's reset calls KZ3ResetDisplay right after SetDefaultDisplay");
        CHECK(rf && strstr(rf, "KZ3DisplayCmd(\"display windowed\")") && strstr(rf, "KZ3DisplayCmd(\"fps 30\")") &&
              strstr(rf, "KZ3DisplayCmd(\"vsync on\")") && strstr(rf, "KZ3DisplayCmd(\"scaling stretch\")") &&
              strstr(rf, "KZ3DisplayCmd(\"filter bilinear\")") && strstr(rf, "\"motionblur on\"") && strstr(rf, "\"fov default\"") &&
              strstr(rf, "OnPageOn()"), "2110: the reset sets every port row to its default and shows the values");
        CHECK(rsx_dm_parse(RSX_DM_DEF_MODE) == RSX_DM_WINDOWED && rsx_dm_parse_scaling(RSX_DM_DEF_SCALING) == RSX_DM_STRETCH &&
              rsx_dm_parse_filter(RSX_DM_DEF_FILTER) == RSX_DM_BILINEAR && rsx_dm_parse_vsync(RSX_DM_DEF_VSYNC) == RSX_DM_VSYNC_ON &&
              rsx_dm_parse_fps(RSX_DM_DEF_FPS) == RSX_DM_FPS_30 && rsx_dm_parse_vsync(RSX_DM_DEF_MBLUR) == 1 &&
              rsx_dm_parse_fov(RSX_DM_DEF_FOV) == 0, "2110: the reset values parse to the port's defaults");
        CHECK(strstr(out, "label=\"A frame rate limit above 30 can make the game run wrong. Are you sure?\"") != NULL,
              "2110: the warning is one line, like the game's own confirm dialogs");
        free(out);
    }
    {   /* a page without SetDefaultDisplay (none known) still gets the rows and tooltips, no reset call */
        static const char noreset[] =
            "<gum>\n\t<panel>\n\t\t<list id=\"lst_options\" class=\"PEDS\">\n\t\t\t<listitem id=\"lst_gamma_screen\" label=\"g\"/>\n"
            "\t</list>\n\t</panel>\n\t<script id=\"settingscript\">\n<![CDATA[\n]]>\n\t</script>\n</gum>\n";
        out = rsx_dm_patch_gum(noreset, sizeof noreset - 1, &n);
        CHECK(out && strstr(out, "KZ3Tooltip") && !strstr(out, "\t\t\t\tKZ3ResetDisplay()"), "2110: no SetDefaultDisplay: no reset call");
        free(out);
    }
    CHECK(rsx_dm_patch_gum("<gum></gum>", 11, &n) == NULL, "no anchors: served unchanged");
    {
        char* withon = (char*)malloc(sizeof page + 64);
        const char* at = strstr(page, "\t\tfunction OnPageOff");
        size_t k = (size_t)(at - page);
        memcpy(withon, page, k);
        strcpy(withon + k, "\t\tfunction OnPageOn()\n\t\tend\n");
        strcat(withon, page + k);
        CHECK(rsx_dm_patch_gum(withon, strlen(withon), &n) == NULL, "page with its own OnPageOn: served unchanged");
        free(withon);
    }
    {
        const char* f = getenv("KZ3_DM_GUM");
        FILE* fp = f && *f ? fopen(f, "rb") : NULL;
        if (!fp) {
            printf("[SKIP] real displaysettings.gum (KZ3_DM_GUM not given)\n");
            return;
        }
        static char text[65536];
        size_t m = fread(text, 1, sizeof text - 1, fp);
        fclose(fp);
        text[m] = 0;
        out = rsx_dm_patch_gum(text, m, &n);
        CHECK(out && strstr(out, "slc_kz3_display") < strstr(out, "lst_gamma_screen"),
              "real displaysettings.gum patched");
        if (out && getenv("KZ3_DM_GUM_OUT")) {
            FILE* o = fopen(getenv("KZ3_DM_GUM_OUT"), "wb");
            if (o) {
                fwrite(out, 1, n, o);
                fclose(o);
            }
        }
        free(out);
    }
}

/* patch 2096: KZ3_ASPECT names and the camera hook's factors */
static void test_aspect(void)
{
    CHECK(rsx_dm_parse_aspect("screen") == RSX_DM_ASPECT_SCREEN && rsx_dm_parse_aspect("Auto") == RSX_DM_ASPECT_SCREEN,
          "aspect screen names");
    CHECK(rsx_dm_parse_aspect("16:9") == RSX_DM_ASPECT_16_9 && rsx_dm_parse_aspect("OFF") == RSX_DM_ASPECT_16_9 &&
              rsx_dm_parse_aspect("0") == RSX_DM_ASPECT_16_9, "aspect 16:9 names");
    CHECK(rsx_dm_parse_aspect("") == -1 && rsx_dm_parse_aspect("4:3") == -1 && rsx_dm_parse_aspect(NULL) == -1,
          "aspect unknown names");
    CHECK(rsx_dm_aspect_ratio(1280, 720) == 1.0 && rsx_dm_aspect_ratio(1920, 1080) == 1.0 &&
              rsx_dm_aspect_ratio(1366, 768) == 1.0 && rsx_dm_aspect_ratio(0, 720) == 1.0, "16:9 clients keep r = 1");
    const double w21 = rsx_dm_aspect_ratio(2560, 1080), w1610 = rsx_dm_aspect_ratio(1920, 1200);
    CHECK(w21 > 1.3332 && w21 < 1.3334, "2560x1080 r = 4/3 (%f)", w21);
    CHECK(w1610 > 0.8999 && w1610 < 0.9001, "1920x1200 r = 0.9 (%f)", w1610);
    /* Hor+ above 16:9: divisor unchanged (vertical FOV kept), width aspect = the client's */
    CHECK(rsx_dm_aspect_scale(w21, 0) == 1.0 && rsx_dm_aspect_scale(w21, 1) == w21, "21:9 factors");
    /* Vert+ below 16:9: both = r, so tan(hfov/2) / a * a = tan(hfov/2): horizontal FOV kept */
    CHECK(rsx_dm_aspect_scale(w1610, 0) == w1610 && rsx_dm_aspect_scale(w1610, 1) == w1610, "16:10 factors");
    {   /* the resulting frustum: horizontal half-angle tangent = tan(vfov/2) * width aspect */
        const double th = 1.0, a = 16.0 / 9.0;                          /* tan(hfov/2) at 16:9 */
        const double tv0 = th / a;                                      /* PS3 tan(vfov/2) */
        const double tv21 = th / (a * rsx_dm_aspect_scale(w21, 0)), th21 = tv21 * a * rsx_dm_aspect_scale(w21, 1);
        const double tv16 = th / (a * rsx_dm_aspect_scale(w1610, 0)), th16 = tv16 * a * rsx_dm_aspect_scale(w1610, 1);
        CHECK(tv21 == tv0 && th21 > th * 1.333, "21:9 keeps the vertical FOV, widens the horizontal");
        CHECK(th16 > th * 0.99999 && th16 < th * 1.00001 && tv16 > tv0 * 1.11, "16:10 keeps the horizontal FOV, taller view");
    }
    CHECK(!strcmp(rsx_dm_aspect_name(RSX_DM_ASPECT_16_9), "16:9") && !strcmp(rsx_dm_aspect_name(0), "screen"), "aspect names");
}

int main(void)
{
    test_names();
    test_states();
    test_fit();
    test_mouse();
    test_scaling();
    test_ini();
    test_gum();
    test_aspect();
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
