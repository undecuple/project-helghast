/* Host test for KZ3 patch 2095 (libs/video/rsx_display_mode.h in the patched toolkit): display-mode names, the mode
 * state machine (Options choice, Alt+Enter toggle, exclusive refusal), the letterbox/pillarbox rectangle, client ->
 * guest mouse mapping, display.ini text, and the Options row's GUM patch (on a synthetic page; KZ3_DM_GUM=<file> also
 * patches a real displaysettings.gum when given). Platform-free; run by test_display_mode.sh. */
#include "rsx_display_mode.h"
#include "rsx_gfx_page.h"

#include <stdint.h>
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

/* RSX pass 125 (patch 2131): the page text carries kz3_tr.h markers (@tr{key|English}, @trl{...} in Lua strings); the port
 * expands them in the game's language, English on failure. These tests check the English page, so expand them here the
 * same way (the English literal), and count the markers in the raw page. */
static int g_markers;
static char* tr_english(char* in, size_t* n)
{
    char* o = (char*)malloc(strlen(in) + 1);
    size_t k = 0;
    for (const char* p = in; *p;) {
        if (p[0] == '@' && p[1] == 't' && p[2] == 'r') {
            const char* q = p + 3;
            if (*q == 'l') q++;
            if (*q == '{') {
                const char* bar = strchr(q, '|');
                const char* end = bar ? strchr(bar, '}') : NULL;
                if (end) { g_markers++; memcpy(o + k, bar + 1, (size_t)(end - bar - 1)); k += (size_t)(end - bar - 1); p = end + 1; continue; }
            }
        }
        o[k++] = *p++;
    }
    o[k] = 0;
    free(in);
    if (n) *n = k;
    return o;
}
static char* t_gum_opts(const char* in, size_t n, size_t* out_n, unsigned opts)
{
    char* r = rsx_dm_patch_gum_opts(in, n, out_n, opts);
    return r ? tr_english(r, out_n) : NULL;
}
#define rsx_dm_patch_gum(a, b, c) t_gum_opts(a, b, c, 0)
#define rsx_dm_patch_gum_opts(a, b, c, d) t_gum_opts(a, b, c, d)

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
              rsx_dm_parse_filter("NEAREST") == RSX_DM_NEAREST &&
              rsx_dm_parse_filter("fsr31") == RSX_DM_FSR31 &&
              rsx_dm_parse_filter("FSR3.1") == RSX_DM_FSR31 &&
              rsx_dm_parse_filter("fsr") < 0 &&
              rsx_dm_parse_scaling(NULL) < 0,
          "scaling/filter names");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1200, RSX_DM_STRETCH, RSX_DM_BILINEAR), 0, 0, 1920, 1200),
          "stretch: whole client");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1200, RSX_DM_FIT, RSX_DM_BILINEAR), 0, 60, 1920, 1080),
          "fit bilinear = rsx_dm_fit");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1080, RSX_DM_FIT, RSX_DM_NEAREST), 320, 180, 1280, 720),
          "nearest at 1080p: 1x centred (1.5x is not whole)");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1080, RSX_DM_FIT, RSX_DM_FSR31), 0, 0, 1920, 1080),
          "FSR at 1080p uses the full non-integer Fit target");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 2560, 1440, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 2560, 1440),
          "nearest at 1440p: 2x fills");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 3840, 2160, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 3840, 2160),
          "nearest at 2160p: 3x fills");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 960, 540, RSX_DM_FIT, RSX_DM_NEAREST), 0, 0, 960, 540),
          "nearest below 1x: plain fit");
    CHECK(rect_is(rsx_dm_rect_for(1280, 720, 1920, 1080, RSX_DM_STRETCH, RSX_DM_NEAREST), 0, 0, 1920, 1080),
          "stretch + nearest: whole client");
    CHECK(rsx_dm_parse_fsr_sharpness("0") == 0 &&
              rsx_dm_parse_fsr_sharpness(" 0.2 ") == 200 &&
              rsx_dm_parse_fsr_sharpness("0.25") == 300 &&
              rsx_dm_parse_fsr_sharpness("1.000") == 1000 &&
              rsx_dm_parse_fsr_sharpness("1.01") < 0 &&
              rsx_dm_parse_fsr_sharpness("-0.1") < 0 &&
              rsx_dm_parse_fsr_sharpness("nan") < 0 &&
              rsx_dm_parse_fsr_sharpness("0.") < 0,
          "FSR sharpness accepts 0.0..1.0 and rejects invalid values");
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
              rsx_dm_ini_filter("[display]\nfilter = fsr31\n") == RSX_DM_FSR31 &&
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
                  strstr(out, "FSR 3.1 (experimental)</selectitem>") &&
                  strstr(out, "\"scaling \"..e") && strstr(out, "\"filter \"..gKz3FilterWant") &&
                  strstr(out, "KZ3DisplayCmd(\"filter\")"),
              "Image scaling and Scaling filter rows with their functions");
        CHECK(strstr(out, "slc_kz3_fsr_sharpness") &&
                  strstr(out, "default-value=\"0.2\"") &&
                  strstr(out, "\"fsrsharpness \"..e") &&
                  strstr(out, "KZ3DisplayCmd(\"fsrsharpness\")"),
              "FSR sharpness row uses 0.0..1.0 and the display bridge");
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
        const char* fsr_dlg = strstr(out, "<dialog id=\"dlg_kz3_fsr\"");
        CHECK(fsr_dlg && fsr_dlg < scr &&
                  strstr(fsr_dlg, "default-focus=\"dlg_kz3_fsr_no\"") &&
                  strstr(out, "dialog(dlg_kz3_fsr, settingscript, OnKZ3FsrClose)") &&
                  strstr(out, "function OnKZ3FsrClose") &&
                  strstr(out, "KZ3DisplayCmd(\"filter fsr31\")"),
              "FSR is experimental and requires confirmation before it is selected");
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
        {   /* RSX pass 136: the log line main.cpp prints at start and on every row change */
            char pl[96];
            int k30 = rsx_dm_fps_pace_line(pl, sizeof pl, 30);
            CHECK(k30 > 0 && !strcmp(pl, "[pace] fps limit 30 (vblank tick 16 ms)"), "RSX pass 136: pace line for 30: '%s'", pl);
            rsx_dm_fps_pace_line(pl, sizeof pl, 60);
            CHECK(!strcmp(pl, "[pace] fps limit 60 (vblank tick 8 ms)"), "RSX pass 136: pace line for 60: '%s'", pl);
            rsx_dm_fps_pace_line(pl, sizeof pl, 120);
            CHECK(!strcmp(pl, "[pace] fps limit 120 (vblank tick 4 ms)"), "RSX pass 136: pace line for 120: '%s'", pl);
        }
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
        CHECK(fv && fv < gam && fv < strstr(out, "slc_kz3_mblur") && strstr(out, "label=\"Field of view\"") &&
                  strstr(out, "default-value=\"default\"") && strstr(out, ">Default</selectitem>") &&
                  strstr(out, "<selectitem value=\"70\">70</selectitem>") && strstr(out, "<selectitem value=\"110\">110</selectitem>") &&
                  strstr(out, "function KZ3FovOn") && strstr(out, "function KZ3FovOn") < cd && strstr(out, "\"fov \"..e") &&
                  strstr(out, "KZ3DisplayCmd(\"fov\")"),
              "2109/2121: Field of view row before Motion blur, Default and 70..110, with its functions");
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
    /* patch 2121 (RSX pass 114): Motion blur as a 0..100 % row after Field of view, display.ini numbers. Patch 2206: Bloom moved
     * to Options > Graphics, so the Display page never has a Bloom row (with or without the BuildPost hook), and its Reset to
     * defaults resets the Graphics rows through "gfx reset". */
    out = rsx_dm_patch_gum_opts(page, sizeof page - 1, &n, RSX_DM_GUM_MBLUR | RSX_DM_GUM_FOV | RSX_DM_GUM_BLOOM);
    CHECK(out && n == strlen(out), "2121: page with Field of view and Motion blur rows patched (Bloom flag set)");
    const char* rf = NULL;
    if (out) {
        const char* gam = strstr(out, "<listitem id=\"lst_gamma_screen\"");
        const char* fv = strstr(out, "<select id=\"slc_kz3_fov\"");
        const char* mb = strstr(out, "<select id=\"slc_kz3_mblur\"");
        CHECK(fv && mb && fv < mb && mb < gam, "2121: order Field of view, Motion blur");
        char want[64];
        int ok = mb != NULL;
        for (int v = 0; v <= 100 && ok; v += 10) {
            snprintf(want, sizeof want, "<selectitem value=\"%d\">%d%%</selectitem>", v, v);
            const char* a = strstr(mb, want);
            ok = a && a < gam;   /* 11 values 0,10..100 */
        }
        CHECK(ok && !strstr(out, "<selectitem value=\"5\">"), "2121: Motion blur offers exactly 0, 10 .. 100 percent (no on / off)");
        CHECK(mb && strstr(mb, "default-value=\"100\"") && strstr(mb, "label=\"Motion blur\""), "2121: Motion blur default 100");
        CHECK(!strstr(out, "slc_kz3_bloom") && !strstr(out, "KZ3BloomOn") && !strstr(out, "KZ3ChangeBloom") && !strstr(out, "label=\"Bloom\""),
              "2206: no Bloom row, function or tooltip on the Display page (it moved to Graphics)");
        rf = strstr(out, "function KZ3ResetDisplay()");
        CHECK(rf && strstr(rf, "\"gfx reset\"") && strstr(rf, "\"motionblur 100\"") && strstr(out, "slc_kz3_mblur = \"Strength"),
              "2206: Reset to defaults resets Motion blur and, through the Graphics command, the Graphics rows (Bloom)");
        free(out);
    }
    out = rsx_dm_patch_gum_opts(page, sizeof page - 1, &n, RSX_DM_GUM_MBLUR | RSX_DM_GUM_FOV);
    CHECK(out && !strstr(out, "slc_kz3_bloom") && !strstr(out, "function KZ3BloomOn"), "2206: no Bloom row on Display without the BuildPost hook either");
    free(out);
    {
        char ib[2048];
        CHECK(rsx_dm_parse_pct("0") == 0 && rsx_dm_parse_pct("100") == 100 && rsx_dm_parse_pct("50") == 50 &&
                  rsx_dm_parse_pct(" 70 ") == 70 && rsx_dm_parse_pct("30%") == 30 && rsx_dm_parse_pct("on") == 100 &&
                  rsx_dm_parse_pct("OFF") == 0 && rsx_dm_parse_pct("true") == 100 && rsx_dm_parse_pct("no") == 0,
              "2121: parse 0..100, percent sign, on/off words");
        CHECK(rsx_dm_parse_pct("55") == 60 && rsx_dm_parse_pct("54") == 50 && rsx_dm_parse_pct("4") == 0 &&
                  rsx_dm_parse_pct("5") == 10 && rsx_dm_parse_pct("95") == 100 && rsx_dm_parse_pct("94") == 90,
              "2121: step rounding to the nearest 10, half up");
        CHECK(rsx_dm_parse_pct("101") == 100 && rsx_dm_parse_pct("250") == 100 && rsx_dm_parse_pct("99999") == 100 &&
                  rsx_dm_parse_pct("-5") == 0 && rsx_dm_parse_pct("1234567") == -1,
              "2121: out of range clamps to 0..100 (huge numbers unreadable)");
        CHECK(rsx_dm_parse_pct("") == -1 && rsx_dm_parse_pct("  ") == -1 && rsx_dm_parse_pct("5x") == -1 && rsx_dm_parse_pct("%") == -1 &&
                  rsx_dm_parse_pct("maybe") == -1 && rsx_dm_parse_pct("-") == -1 && rsx_dm_parse_pct("1.5") == -1,
              "2121: unreadable values are -1 (the key is ignored)");
        CHECK(rsx_dm_pct_round(-3) == 0 && rsx_dm_pct_round(0) == 0 && rsx_dm_pct_round(44) == 40 && rsx_dm_pct_round(45) == 50 &&
                  rsx_dm_pct_round(100) == 100 && rsx_dm_pct_round(400) == 100,
              "2121: rsx_dm_pct_round");
        {
            const float v = 0.0123f;
            uint32_t b1, b2;
            const float s100 = rsx_dm_pct_scale(v, 100), s50 = rsx_dm_pct_scale(v, 50);
            memcpy(&b1, &v, 4); memcpy(&b2, &s100, 4);
            CHECK(b1 == b2 && rsx_dm_pct_scale(v, 0) == 0.0f && rsx_dm_pct_scale(v, 150) == v && s50 == v * 0.5f &&
                      rsx_dm_pct_scale(v, 10) > 0.0f && rsx_dm_pct_scale(v, 10) < rsx_dm_pct_scale(v, 20),
                  "2121: bloom scale: 100 leaves the float bit for bit, 0 gives 0, 50 halves it, monotonic");
        }
        CHECK(rsx_dm_ini_motion_blur("[display]\nmotion_blur = on\n") == 100 && rsx_dm_ini_motion_blur("[display]\nmotion_blur = off\n") == 0 &&
                  rsx_dm_ini_motion_blur("[display]\nmotion_blur = 40\n") == 40 && rsx_dm_ini_motion_blur("[display]\nmotion_blur = zzz\n") == -1 &&
                  rsx_dm_ini_motion_blur("[display]\nvsync = on\n") == -1,
              "2121: an old file's motion_blur on / off reads as 100 / 0, a number as itself");
        CHECK(rsx_dm_ini_bloom("[display]\nbloom = 0\n") == 0 && rsx_dm_ini_bloom("[display]\nbloom=100\n") == 100 &&
                  rsx_dm_ini_bloom("[display]\nbloom = 33\n") == 30 && rsx_dm_ini_bloom("[display]\nbloom = 999\n") == 100 &&
                  rsx_dm_ini_bloom("[display]\nbloom = on\n") == 100 && rsx_dm_ini_bloom("[display]\nmotion_blur = 50\n") == -1,
              "2121: bloom key read (rounded, clamped), missing -> -1");
        int k = rsx_dm_ini_write_ext4(ib, sizeof ib, RSX_DM_FULLSCREEN, RSX_DM_FIT, RSX_DM_NEAREST, 0, 30, RSX_DM_FPS_60, 95, 1, 70);
        CHECK(k > 0 && k < (int)sizeof ib && (size_t)k == strlen(ib) && strstr(ib, "motion_blur = 30\n") && strstr(ib, "bloom = 70\n") &&
                  rsx_dm_ini_motion_blur(ib) == 30 && rsx_dm_ini_bloom(ib) == 70 && rsx_dm_ini_fov(ib) == 95 &&
                  rsx_dm_ini_fps(ib) == 60 && rsx_dm_ini_hud169(ib) == 1 && rsx_dm_ini_parse(ib) == RSX_DM_FULLSCREEN &&
                  rsx_dm_ini_vsync(ib) == 0,
              "2121: ini round trip motion_blur 30, bloom 70, the other keys kept; the number is always written");
        rsx_dm_ini_write_ext4(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 100, RSX_DM_FPS_30, 0, 1, 100);
        CHECK(strstr(ib, "motion_blur = 100\n") && !strstr(ib, "motion_blur = on") && strstr(ib, "bloom = 100\n"),
              "2121: defaults are written as 100, never on / off");
        rsx_dm_ini_write_ext4(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 55, RSX_DM_FPS_30, 0, 1, 12);
        CHECK(strstr(ib, "motion_blur = 60\n") && strstr(ib, "bloom = 10\n"), "2121: the writer rounds to the step too");
        rsx_dm_ini_write_ext3(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 100, RSX_DM_FPS_30, 0, 1);
        CHECK(rsx_dm_ini_bloom(ib) == -1, "2121: a file from before the Bloom row has no bloom key (default 100 applies)");
        k = rsx_dm_ini_write_ext5(
            ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_FSR31,
            1, 100, RSX_DM_FPS_30, 0, 1, 100, 250);
        CHECK(k > 0 && k < (int)sizeof ib &&
                  strstr(ib, "filter = fsr31\n") &&
                  strstr(ib, "fsr_sharpness = 0.3\n") &&
                  rsx_dm_ini_fsr_sharpness(ib) == 300 &&
                  rsx_dm_ini_fsr_sharpness(
                      "[display]\nfsr_sharpness = 1.1\n") < 0,
              "FSR filter and sharpness persist, round, and reject out-of-range input");
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
        CHECK(strstr(out, "function KZ3Tooltip(e)") && strstr(out, "slc_kz3_fps = \"Used in missions. Menus and videos stay at 30. Above 30") &&
              strstr(out, "slc_kz3_fov = ") && strstr(out, "slc_kz3_mblur = ") && strstr(out, "slc_kz3_display = ") &&
              strstr(out, "slc_kz3_scaling = ") && strstr(out, "slc_kz3_filter = ") &&
              strstr(out, "slc_kz3_fsr_sharpness = ") && strstr(out, "slc_kz3_vsync = "),
              "2110: a tooltip for every port row");
        CHECK(rc && rc < strstr(sd, "end") && rf && rf < cd, "2110: the game's reset calls KZ3ResetDisplay right after SetDefaultDisplay");
        CHECK(rf && strstr(rf, "KZ3DisplayCmd(\"display windowed\")") && strstr(rf, "KZ3DisplayCmd(\"fps 30\")") &&
              strstr(rf, "KZ3DisplayCmd(\"vsync on\")") && strstr(rf, "KZ3DisplayCmd(\"scaling stretch\")") &&
              strstr(rf, "KZ3DisplayCmd(\"filter bilinear\")") &&
              strstr(rf, "KZ3DisplayCmd(\"fsrsharpness 0.2\")") &&
              strstr(rf, "\"motionblur 100\"") && strstr(rf, "\"fov default\"") &&
              strstr(rf, "OnPageOn()"), "2110: the reset sets every port row to its default and shows the values");
        CHECK(rsx_dm_parse(RSX_DM_DEF_MODE) == RSX_DM_WINDOWED && rsx_dm_parse_scaling(RSX_DM_DEF_SCALING) == RSX_DM_STRETCH &&
              rsx_dm_parse_filter(RSX_DM_DEF_FILTER) == RSX_DM_BILINEAR && rsx_dm_parse_vsync(RSX_DM_DEF_VSYNC) == RSX_DM_VSYNC_ON &&
              rsx_dm_parse_fps(RSX_DM_DEF_FPS) == RSX_DM_FPS_30 && rsx_dm_parse_pct(RSX_DM_DEF_MBLUR) == 100 && rsx_dm_parse_pct(RSX_DM_DEF_BLOOM) == 100 &&
              rsx_dm_parse_fov(RSX_DM_DEF_FOV) == 0 &&
              rsx_dm_parse_fsr_sharpness(RSX_DM_DEF_FSR_SHARPNESS) ==
                  RSX_DM_FSR_SHARPNESS_DEFAULT,
              "2110: the reset values parse to the port's defaults");
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

/* patch 2206: Options > Graphics, the Quality preset. Pure logic (rsx_gfx_page.h), the ini key, the page text. */
static int count_str(const char* h, const char* needle)
{
    int n = 0;
    for (const char* p = h; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}
static void test_gfx(void)
{
    char ib[2048], rp[64];
    /* 1. names and parsing: the same shape as the other parsers (unknown = -1, NULL = -1, case and blanks ignored) */
    CHECK(rsx_gfx_quality_parse("high") == RSX_GFX_HIGH && rsx_gfx_quality_parse("HIGH") == RSX_GFX_HIGH &&
              rsx_gfx_quality_parse(" Ultra ") == RSX_GFX_ULTRA && rsx_gfx_quality_parse("custom\r\n") == RSX_GFX_CUSTOM,
          "2206: quality parse: high, ultra, custom in any case, blanks ignored");
    CHECK(rsx_gfx_quality_parse("") == -1 && rsx_gfx_quality_parse("x") == -1 && rsx_gfx_quality_parse(NULL) == -1 &&
              rsx_gfx_quality_parse("ultra2") == -1 && rsx_gfx_quality_parse("ultr") == -1 && rsx_gfx_quality_parse("auto") == -1,
          "2206: quality parse: empty, NULL, unknown words are -1 (never Ultra; Auto is not a state)");
    for (int q = 0; q < 3; q++)
        CHECK(rsx_gfx_quality_parse(rsx_gfx_quality_name(q)) == q, "2206: quality name round trip %d", q);
    CHECK(!strcmp(rsx_gfx_quality_name(77), "high"), "2206: an out-of-range state names as high");

    /* 2. display.ini: the key, the writer, the older readers */
    CHECK(rsx_dm_ini_quality("[display]\nquality = ultra\n") == RSX_GFX_ULTRA && rsx_dm_ini_quality("[display]\nquality=custom\n") == RSX_GFX_CUSTOM &&
              rsx_dm_ini_quality("[display]\nquality = High\n") == RSX_GFX_HIGH,
          "2206: quality key read");
    CHECK(rsx_dm_ini_quality("[display]\nbloom = 50\n") == -1 && rsx_dm_ini_quality("") == -1 && rsx_dm_ini_quality("[input]\nquality = ultra\n") == -1,
          "2206: missing key, empty file, key in another section: -1");
    CHECK(rsx_dm_ini_quality("[display]\nquality = ultraa\n") == -1 && rsx_dm_ini_quality("[display]\nquality = \n") == -1 &&
              rsx_dm_ini_quality("[display]\nquality = 2\n") == -1,
          "2206: unknown value never reads as Ultra (negative control)");
    int k = rsx_dm_ini_write_ext5(ib, sizeof ib, RSX_DM_FULLSCREEN, RSX_DM_FIT, RSX_DM_NEAREST, 0, 30, RSX_DM_FPS_60, 95, 1, 70, RSX_GFX_CUSTOM);
    CHECK(k > 0 && k < (int)sizeof ib && (size_t)k == strlen(ib) && strstr(ib, "quality = custom\n") && rsx_dm_ini_quality(ib) == RSX_GFX_CUSTOM,
          "2206: ext5 writes quality = custom and reads it back");
    CHECK(rsx_dm_ini_parse(ib) == RSX_DM_FULLSCREEN && rsx_dm_ini_scaling(ib) == RSX_DM_FIT && rsx_dm_ini_filter(ib) == RSX_DM_NEAREST &&
              rsx_dm_ini_vsync(ib) == 0 && rsx_dm_ini_motion_blur(ib) == 30 && rsx_dm_ini_fps(ib) == RSX_DM_FPS_60 && rsx_dm_ini_fov(ib) == 95 &&
              rsx_dm_ini_hud169(ib) == 1 && rsx_dm_ini_bloom(ib) == 70,
          "2206: the ext4 readers still parse an ext5 file, every key kept");
    rsx_dm_ini_write_ext4(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 100, RSX_DM_FPS_30, 0, 1, 100);
    CHECK(rsx_dm_ini_quality(ib) == -1 && rsx_dm_ini_bloom(ib) == 100, "2206: an ext4 file has no quality key (High applies)");
    rsx_dm_ini_write_ext5(ib, sizeof ib, RSX_DM_WINDOWED, RSX_DM_FIT, RSX_DM_BILINEAR, 1, 100, RSX_DM_FPS_30, 0, 1, 100, RSX_GFX_HIGH);
    CHECK(strstr(ib, "quality = high\n") && !strstr(ib, "quality = ultra"), "2206: a default file says quality = high");

    /* 3. the tables: High is today's default Bloom, Ultra differs from High in exactly the landed Ultra levers */
    {
        const rsx_gfx_vals h = rsx_gfx_table(RSX_GFX_HIGH, RSX_GFX_L_BLOOM);
        CHECK(h.v[0] == rsx_dm_parse_pct(RSX_DM_DEF_BLOOM) && h.v[0] == 100, "2206: the High table equals today's default Bloom (100)");
        const rsx_gfx_vals h4 = rsx_gfx_table(RSX_GFX_HIGH, 15);
        CHECK(h4.v[1] == 100 && h4.v[2] == 0 && h4.v[3] == 0, "2206: High keeps the game's own resolution, anti-aliasing and sampling");
    }
    CHECK(!rsx_gfx_ultra_offered(0) && !rsx_gfx_ultra_offered(RSX_GFX_L_BLOOM), "2206: with only Bloom landed Ultra is not offered (it would equal High)");
    {
        int ok = 1;
        for (unsigned lv = 0; lv < 16; lv++) {
            const rsx_gfx_vals h = rsx_gfx_table(RSX_GFX_HIGH, lv), u = rsx_gfx_table(RSX_GFX_ULTRA, lv);
            unsigned diff = 0;
            for (int i = 0; i < RSX_GFX_NLEVERS; i++) if (h.v[i] != u.v[i]) diff |= 1u << i;
            if (diff != (lv & ~(unsigned)RSX_GFX_L_BLOOM)) ok = 0;   /* Bloom is the one lever Ultra never changes */
            if (diff != rsx_gfx_ultra_levers(lv)) ok = 0;
            if (rsx_gfx_ultra_offered(lv) != (diff != 0)) ok = 0;
        }
        CHECK(ok, "2206: for every set of landed levers the Ultra table differs from High in exactly those levers and nothing else");
    }

    /* 4. the Custom rule */
    {
        rsx_gfx_vals v = rsx_gfx_table(RSX_GFX_HIGH, RSX_GFX_L_BLOOM);
        CHECK(rsx_gfx_classify(&v, RSX_GFX_L_BLOOM) == RSX_GFX_HIGH, "2206: the High table classifies as High");
        v.v[0] = 50;
        CHECK(rsx_gfx_classify(&v, RSX_GFX_L_BLOOM) == RSX_GFX_CUSTOM, "2206: High + Bloom 50 = Custom");
        v.v[0] = 100;
        CHECK(rsx_gfx_classify(&v, RSX_GFX_L_BLOOM) == RSX_GFX_HIGH, "2206: Custom + Bloom back to 100 = High again");
        const unsigned lv = RSX_GFX_L_BLOOM | RSX_GFX_L_AA;
        v = rsx_gfx_table(RSX_GFX_ULTRA, lv);
        CHECK(rsx_gfx_classify(&v, lv) == RSX_GFX_ULTRA, "2206: the Ultra table classifies as Ultra when a lever landed");
        v.v[0] = 40;
        CHECK(rsx_gfx_classify(&v, lv) == RSX_GFX_CUSTOM, "2206: Ultra + Bloom 40 = Custom");
        v.v[0] = 100;
        CHECK(rsx_gfx_classify(&v, lv) == RSX_GFX_ULTRA, "2206: and back to Ultra when the values match again");
        v.v[2] = 0;
        CHECK(rsx_gfx_classify(&v, lv) == RSX_GFX_HIGH, "2206: Ultra with the anti-aliasing lever set back is exactly High");
        v = rsx_gfx_table(RSX_GFX_ULTRA, lv);
        CHECK(rsx_gfx_classify(&v, RSX_GFX_L_BLOOM) == RSX_GFX_HIGH, "2206: the Ultra values read with that lever not landed are High (negative control)");
        v.v[1] = 150;   /* a lever that has not landed never counts */
        CHECK(rsx_gfx_classify(&v, lv) == RSX_GFX_ULTRA, "2206: values of a lever that has not landed are ignored");
    }
    {   /* load: quality from the file against the values */
        const unsigned lv = RSX_GFX_L_BLOOM | RSX_GFX_L_AA;
        rsx_gfx_vals h = rsx_gfx_table(RSX_GFX_HIGH, lv), u = rsx_gfx_table(RSX_GFX_ULTRA, lv), c = h;
        c.v[0] = 30;
        CHECK(rsx_gfx_load_state(RSX_GFX_ULTRA, &h, lv) == RSX_GFX_CUSTOM, "2206: hand-edited quality = ultra with the High values loads as Custom");
        CHECK(rsx_gfx_load_state(RSX_GFX_ULTRA, &u, lv) == RSX_GFX_ULTRA && rsx_gfx_load_state(RSX_GFX_HIGH, &h, lv) == RSX_GFX_HIGH,
              "2206: matching quality and values load as that preset");
        CHECK(rsx_gfx_load_state(-1, &h, lv) == RSX_GFX_HIGH && rsx_gfx_load_state(-1, &c, lv) == RSX_GFX_CUSTOM,
              "2206: a missing key is High, and edited values then show as Custom");
        CHECK(rsx_gfx_load_state(RSX_GFX_HIGH, &c, lv) == RSX_GFX_CUSTOM && rsx_gfx_load_state(RSX_GFX_CUSTOM, &h, lv) == RSX_GFX_HIGH,
              "2206: quality = high over edited values is Custom; quality = custom over the High values is High");
        CHECK(rsx_gfx_load_state(RSX_GFX_ULTRA, &u, RSX_GFX_L_BLOOM) == RSX_GFX_CUSTOM,
              "2206: quality = ultra after the Ultra lever was removed: Custom, never a silent Ultra");
    }
    /* the file is not rewritten on load: the loader only reads, the host writes on a change (rsx_gfx_cmd's return value) */

    /* 5. the multiplayer lock */
    {
        rsx_gfx_vals s = rsx_gfx_table(RSX_GFX_ULTRA, 15);
        s.v[0] = 40;
        const rsx_gfx_vals before = s, h = rsx_gfx_table(RSX_GFX_HIGH, 15);
        int ok = 1;
        for (int t = 1; t <= 3; t++) {
            const rsx_gfx_vals e = rsx_gfx_effective(&s, t, 15);
            ok = ok && !memcmp(&e, &h, sizeof e) && rsx_gfx_effective_bloom(40, t) == 100 && rsx_gfx_locked(t);
        }
        CHECK(ok, "2206: game types 1, 2 and 3 get the High table and Bloom 100");
        ok = 1;
        const int open[] = { 0, -1, 4, -2 };
        for (int i = 0; i < 4; i++) {
            const rsx_gfx_vals e = rsx_gfx_effective(&s, open[i], 15);
            ok = ok && !memcmp(&e, &s, sizeof e) && rsx_gfx_effective_bloom(40, open[i]) == 40 && !rsx_gfx_locked(open[i]);
        }
        CHECK(ok, "2206: campaign (0), no session (-1) and other types keep the saved values (negative control)");
        CHECK(!memcmp(&s, &before, sizeof s), "2206: the saved values are untouched by the lock and unlock");
    }
    {   /* the bridge: commands on a state, the lock, the reset */
        rsx_gfx_state st = { rsx_gfx_table(RSX_GFX_HIGH, 15), RSX_GFX_HIGH, 15 };
        int ch = rsx_gfx_cmd(&st, "quality", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "high"), "2206: cmd quality reads High");
        ch = rsx_gfx_cmd(&st, "quality ultra", 0, rp, sizeof rp);
        CHECK(ch == 1 && !strcmp(rp, "ultra") && st.v.v[1] == 150 && st.v.v[2] == 1 && st.v.v[3] == 1 && st.v.v[0] == 100,
              "2206: cmd quality ultra writes every landed lever at once");
        ch = rsx_gfx_cmd(&st, "bloom 50", 0, rp, sizeof rp);
        CHECK(ch == 1 && !strcmp(rp, "50") && st.quality == RSX_GFX_CUSTOM, "2206: a manual change leaves the preset (Custom)");
        ch = rsx_gfx_cmd(&st, "quality custom", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "custom") && st.v.v[0] == 50, "2206: picking Custom by hand changes nothing");
        ch = rsx_gfx_cmd(&st, "bloom 100", 0, rp, sizeof rp);
        CHECK(ch == 1 && st.quality == RSX_GFX_ULTRA, "2206: Bloom back to 100 returns to Ultra (the other levers are at Ultra)");
        ch = rsx_gfx_cmd(&st, "quality high", 0, rp, sizeof rp);
        CHECK(ch == 1 && !strcmp(rp, "high") && st.v.v[1] == 100 && st.v.v[2] == 0, "2206: cmd quality high");
        ch = rsx_gfx_cmd(&st, "bloom 77", 0, rp, sizeof rp);
        CHECK(ch == 1 && !strcmp(rp, "80"), "2206: a bloom value rounds to the step like the Display row did");
        ch = rsx_gfx_cmd(&st, "bloom banana", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "80") && st.quality == RSX_GFX_CUSTOM, "2206: an unreadable value changes nothing");
        ch = rsx_gfx_cmd(&st, "res 133", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "100"), "2206: a value outside a lever's list is refused");
        ch = rsx_gfx_cmd(&st, "reset", 0, rp, sizeof rp);
        CHECK(ch == 1 && !strcmp(rp, "high") && st.v.v[0] == 100 && st.quality == RSX_GFX_HIGH, "2206: reset gives the High table");
        ch = rsx_gfx_cmd(&st, "reset", 0, rp, sizeof rp);
        CHECK(!ch, "2206: reset on High changes nothing (no needless save)");
        /* lock */
        rsx_gfx_cmd(&st, "quality ultra", 0, rp, sizeof rp);
        rsx_gfx_cmd(&st, "bloom 30", 0, rp, sizeof rp);
        const rsx_gfx_state keep = st;
        for (int t = 1; t <= 3; t++) {
            ch = rsx_gfx_cmd(&st, "lock", t, rp, sizeof rp);
            CHECK(!ch && !strcmp(rp, "1"), "2206: lock reads 1 in game type %d", t);
            ch = rsx_gfx_cmd(&st, "quality ultra", t, rp, sizeof rp);
            int a = !ch && !strcmp(rp, "high");
            ch = rsx_gfx_cmd(&st, "bloom 90", t, rp, sizeof rp);
            a = a && !ch && !strcmp(rp, "100");
            ch = rsx_gfx_cmd(&st, "res 100", t, rp, sizeof rp);
            a = a && !ch && !strcmp(rp, "100");
            ch = rsx_gfx_cmd(&st, "reset", t, rp, sizeof rp);
            a = a && !ch;
            CHECK(a && !memcmp(&st, &keep, sizeof st), "2206: in game type %d every answer is High and nothing is written or changed", t);
        }
        ch = rsx_gfx_cmd(&st, "lock", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "0"), "2206: lock reads 0 in the campaign");
        ch = rsx_gfx_cmd(&st, "bloom", -1, rp, sizeof rp);
        CHECK(!strcmp(rp, "30"), "2206: unlocked, the player's choice comes back (negative control for the lock)");
        /* levers that have not landed */
        rsx_gfx_state only = { rsx_gfx_table(RSX_GFX_HIGH, RSX_GFX_L_BLOOM), RSX_GFX_HIGH, RSX_GFX_L_BLOOM };
        ch = rsx_gfx_cmd(&only, "quality ultra", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "high"), "2206: Ultra is refused while no Ultra lever landed");
        ch = rsx_gfx_cmd(&only, "aa 1", 0, rp, sizeof rp);
        CHECK(!ch && !strcmp(rp, "0") && only.v.v[2] == 0, "2206: a lever that has not landed cannot be set");
        ch = rsx_gfx_cmd(&only, "nonsense", 0, rp, sizeof rp);
        CHECK(!ch && !rp[0], "2206: an unknown command answers empty");
    }
    {   /* KZ3_QUALITY for one run */
        rsx_gfx_vals o;
        CHECK(rsx_gfx_env_vals(15, "ultra", &o) == 1 && o.v[1] == 150 && rsx_gfx_env_vals(15, "HIGH", &o) == 1 && o.v[1] == 100,
              "2206: KZ3_QUALITY=ultra / high pick the table");
        CHECK(rsx_gfx_env_vals(RSX_GFX_L_BLOOM, "ultra", &o) == -1 && rsx_gfx_env_vals(15, "", &o) == 0 && rsx_gfx_env_vals(15, NULL, &o) == 0 &&
                  rsx_gfx_env_vals(15, "custom", &o) == 0 && rsx_gfx_env_vals(15, "x", &o) == 0,
              "2206: KZ3_QUALITY=ultra without an Ultra lever is reported, unset / custom / unknown do nothing");
    }

    /* 6. the page text */
    {
        size_t n = 0;
        const int base_markers = g_markers;   /* the Display page's count (below) is kept */
        char* raw = rsx_gfx_page_gum(RSX_GFX_L_BLOOM, &n);
        CHECK(raw && n == strlen(raw), "2206: the page is generated (Bloom only)");
        if (raw) {
            const int markers = count_str(raw, "@tr");
            char* pg = tr_english(raw, &n);
            const char* pre = strstr(pg, "<select id=\"slc_kz3_gfx_preset\"");
            const char* blo = strstr(pg, "<select id=\"slc_kz3_gfx_bloom\"");
            CHECK(pre && blo && pre < blo && strstr(pg, "default-focus=\"slc_kz3_gfx_preset\"") && count_str(pg, "<select ") == 2,
                  "2206: Quality first, then Bloom, and nothing else (one select per landed lever)");
            CHECK(strstr(pg, "label=\"Quality\"") && strstr(pg, "label=\"Bloom\"") && strstr(pg, ">Change the graphics settings</text>") &&
                      strstr(pg, "@key{menu_options}") && strstr(pg, "class=\"PEDS\""),
                  "2206: labels, subtitle in the siblings' form, the PEDS list");
            CHECK(!strstr(pg, "slc_kz3_mblur") && !strstr(pg, "Motion blur") && !strstr(pg, "slc_kz3_fov") && !strstr(pg, "slc_kz3_fps") &&
                      !strstr(pg, "Field of view") && !strstr(pg, "Frame rate"),
                  "2206: no motion blur row, no Field of view or Frame rate limit (they stay on Display)");
            CHECK(!strstr(pg, "value=\"ultra\"") && !strstr(pg, ">Ultra<") && strstr(pg, "value=\"high\">High<") && strstr(pg, "value=\"custom\">Custom<"),
                  "2206: negative control: with no Ultra lever there is no Ultra item, only High and Custom");
            const char* oa = strstr(pg, "function OnPageOn()");
            const char* g0 = oa ? strstr(oa, "gKz3G = 0") : NULL;
            const char* c1 = oa ? strstr(oa, "KZ3GfxCmd(") : NULL;
            const char* g1 = oa ? strstr(oa, "gKz3G = 1") : NULL;
            CHECK(strstr(pg, "gKz3G = 0\n\t\tgKz3Lock = 0") && g0 && c1 && g1 && g0 < c1 && c1 < g1 && strstr(pg, "if (gKz3G == 1)"),
                  "2206: the guard is 0 before any KZ3GfxCmd of OnPageOn and set to 1 after the rows are filled");
            const char* rfsh = strstr(pg, "function KZ3Refresh()");
            CHECK(rfsh && strstr(rfsh, "gKz3G = 0") && strstr(rfsh, "gKz3G = 0") < strstr(rfsh, "KZ3GfxShow()") &&
                      strstr(rfsh, "KZ3GfxShow()") < strstr(rfsh, "gKz3G = 1"),
                  "2206: a refresh after a change keeps the guard at 0 while the rows are set");
            CHECK(strstr(pg, "labelcancel=\"@key{menu_legend_back}\"") && strstr(pg, "labeloptions=\"@key{menu_legend_default}\"") &&
                      strstr(pg, "oncancel=\"back()\"") && !strstr(pg, "onstart"),
                  "2206: the game's two legend prompts, Back and Default");
            CHECK(strstr(pg, "class=\"tooltip\" y=\"440\"") && strstr(pg, "dialogconfirm.gum") && strstr(pg, "dlg_confirmdefaults") &&
                      strstr(pg, "class=\"background\"") && strstr(pg, "MenuBackground.bik") && strstr(pg, "type3.gum"),
                  "2206: tooltip panel, the game's confirm dialog, background and movie as the sibling pages");
            CHECK(strstr(pg, "KZ3GfxCmd(\"lock\") == \"1\"") && strstr(pg, "Fixed to High in multiplayer so everyone sees the same picture.") &&
                      strstr(pg, "kb:gfx "),
                  "2206: the lock is asked at OnPageOn; the lock line is the tooltip when locked; bridge prefix gfx");
            CHECK(strstr(pg, "Your own mix. Pick High to reset the rows below.") && !strstr(pg, "Pick High or Ultra") &&
                      strstr(pg, "High is the game's own look. Changing a setting below makes it Custom.") &&
                      !strstr(pg, "Applies at the next level load") && !strstr(pg, "Applies after restarting") && !strstr(pg, "apply later"),
                  "2206: tooltips for Bloom only: High wording, no status sentence (Bloom applies at once)");
            CHECK(markers >= 8 && g_markers - base_markers == markers && !strstr(pg, "@tr"), "2206: every player-visible string carries a kz3_tr marker (%d)", markers);
            free(pg);
        }
        /* with levers: Ultra offered, the status line per row, the row cap */
        raw = rsx_gfx_page_gum(RSX_GFX_L_BLOOM | RSX_GFX_L_RES | RSX_GFX_L_AA | RSX_GFX_L_SHARP, &n);
        CHECK(raw != NULL, "2206: the page with every lever is generated");
        if (raw) {
            char* pg = tr_english(raw, &n);
            CHECK(count_str(pg, "<select ") == 5 && rsx_gfx_page_rows(15) == 5 && rsx_gfx_page_rows(15) <= RSX_GFX_ROWS_MAX &&
                      rsx_gfx_page_rows(RSX_GFX_L_BLOOM) == 2,
                  "2206: one select per landed lever, at most %d rows", RSX_GFX_ROWS_MAX);
            CHECK(strstr(pg, "value=\"ultra\">Ultra<") && strstr(pg, "Ultra adds PC picture improvements.") &&
                      strstr(pg, "Pick High or Ultra to reset the rows below.") && strstr(pg, "Some changes apply later. Each row says when."),
                  "2206: with levers landed Ultra is offered and the preset tooltip says some changes apply later");
            const char* bl = strstr(pg, "slc_kz3_gfx_bloom = \"");
            const char* rs = strstr(pg, "slc_kz3_gfx_res = \"");
            const char* aa = strstr(pg, "slc_kz3_gfx_aa = \"");
            const char* sh = strstr(pg, "slc_kz3_gfx_sharp = \"");
            const char* nl = rs ? strstr(rs, "Applies at the next level load.") : NULL;
            const char* rt = aa ? strstr(aa, "Applies after restarting the game.") : NULL;
            CHECK(bl && rs && aa && sh && bl < rs && rs < aa && aa < sh && nl && nl < aa && rt && rt < sh,
                  "2206: the status line (next level load / restart) is in the tooltip of a not-live row");
            const char* st = bl ? strstr(bl, "Applies") : NULL;
            CHECK(bl && rs && st && st > rs, "2206: Bloom's own tooltip has no status sentence (it applies at once)");
            free(pg);
        }
    }
    {   /* the Bloom row: 0..100 in tens, default 100 (moved from Display, patch 2121) */
        size_t n = 0;
        char* raw = rsx_gfx_page_gum(RSX_GFX_L_BLOOM, &n);
        char* pg = raw ? tr_english(raw, &n) : NULL;
        const char* bl = pg ? strstr(pg, "<select id=\"slc_kz3_gfx_bloom\"") : NULL;
        int ok = bl != NULL;
        char want[64];
        for (int v = 0; v <= 100 && ok; v += 10) {
            snprintf(want, sizeof want, "<selectitem value=\"%d\">%d%%</selectitem>", v, v);
            ok = strstr(bl, want) != NULL;
        }
        CHECK(ok && strstr(bl, "default-value=\"100\"") && !strstr(pg, "<selectitem value=\"5\">"), "2206: the Bloom row offers 0, 10 .. 100 percent and defaults to 100");
        free(pg);
    }
    {   /* the Default legend: on while the page can change, off when a multiplayer match locks it */
        size_t n = 0, nl = 0;
        char* un = rsx_gfx_page_gum_l(RSX_GFX_L_BLOOM, 0, &n);
        char* lk = rsx_gfx_page_gum_l(RSX_GFX_L_BLOOM, 1, &nl);
        char* wr = rsx_gfx_page_gum(RSX_GFX_L_BLOOM, &n);
        CHECK(un && lk && wr && !strcmp(un, wr) && strstr(un, "labeloptions=\"@key{menu_legend_default}\"") &&
                  strstr(un, "onoptions=\"script(settingscript, SetToDefault)\""),
              "2206: unlocked page has the Default legend (and the plain generator is the unlocked page)");
        CHECK(lk && !strstr(lk, "labeloptions") && !strstr(lk, "onoptions=") && strstr(lk, "labelcancel=\"@key{menu_legend_back}\"") &&
                  strstr(lk, "<panel id=\"pnl_main\" class=\"safearea\"") && strstr(lk, "SetToDefault") &&
                  count_str(lk, "<select ") == 2,
              "2206: locked page has Back but no Default legend; the rest of the page is the same");
        free(un); free(lk); free(wr);
    }
    {   /* a Bloom value that is not a step of 10 shows as the nearest step the row has (never from the ini, which rounds) */
        char rp[32];
        rsx_gfx_state st = { rsx_gfx_table(RSX_GFX_HIGH, RSX_GFX_L_BLOOM), RSX_GFX_CUSTOM, RSX_GFX_L_BLOOM };
        int ok = 1;
        for (int v = 0; v <= 100; v++) {
            st.v.v[0] = v;
            (void)rsx_gfx_cmd(&st, "bloom", 0, rp, sizeof rp);
            ok = ok && atoi(rp) == rsx_dm_pct_round(v) && atoi(rp) % 10 == 0 && st.v.v[0] == v;
        }
        CHECK(ok, "2206: every Bloom value 0..100 shows as a step of 10 on the row and the saved value is untouched");
        st.v.v[0] = 55;
        (void)rsx_gfx_cmd(&st, "bloom", 0, rp, sizeof rp);
        CHECK(!strcmp(rp, "60") && rsx_dm_parse_pct("55") == 60, "2206: bloom 55 shows 60, as the ini parse rounds it");
    }
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
    CHECK(g_markers >= 60, "patch 2131: the Display page rows, values, dialog and tooltips carry @tr markers (%d seen)", g_markers);
    test_gfx();
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
