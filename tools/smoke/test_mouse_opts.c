/* Host test for KZ3 patch 2101 (Options > Mouse rows): the value clamp and step, the number format, the kz3.ini
 * [input] round trip (libs/input/pad_mouse.h) and the runtime setter (pad_mouse.c). No game data, no GPU. */
#include "pad_mouse.h"
#include <stdio.h>
#include <string.h>

unsigned cellGcm_flip_request_count(void) { return 0; }   /* pad_mouse.c links the guest frame clock */
static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

static void up(const char* in, char* out, size_t cap, const char* k, const char* v)
{
    if (pad_mouse_ini_upsert(in, out, cap, k, v) < 0) strcpy(out, "<overflow>");
}

int main(void)
{
    char b[512], c[512], v[32];
    check("clamp low", pad_mouse_opt_clamp(0.1f, 2.5f) == 0.5f && pad_mouse_opt_clamp(-3.f, 2.5f) == 0.5f);
    check("clamp high", pad_mouse_opt_clamp(99.f, 2.5f) == 10.0f);
    check("snap to 0.5", pad_mouse_opt_clamp(2.7f, 2.5f) == 2.5f && pad_mouse_opt_clamp(2.8f, 2.5f) == 3.0f);
    check("nan/inf -> default", pad_mouse_opt_clamp(NAN, 2.5f) == 2.5f && pad_mouse_opt_clamp(INFINITY, 1.0f) == 1.0f);
    check("step up", pad_mouse_opt_step(2.5f, 1) == 3.0f && pad_mouse_opt_step(9.5f, 1) == 10.0f);
    check("step stops at the top", pad_mouse_opt_step(10.0f, 1) == 10.0f);
    check("step down", pad_mouse_opt_step(2.5f, -1) == 2.0f && pad_mouse_opt_step(1.0f, -1) == 0.5f);
    check("step stops at the bottom", pad_mouse_opt_step(0.5f, -1) == 0.5f);
    check("step 0 keeps", pad_mouse_opt_step(2.5f, 0) == 2.5f);
    int n = 0; float x = PAD_MOUSE_OPT_MIN;
    while (x < PAD_MOUSE_OPT_MAX) { x = pad_mouse_opt_step(x, 1); n++; }
    check("19 steps cover 0.5..10 (20 row values)", n == 19 && x == 10.0f);
    pad_mouse_opt_fmt(v, sizeof v, 2.5f); check("fmt 2.5", !strcmp(v, "2.5"));
    pad_mouse_opt_fmt(v, sizeof v, 10.0f); check("fmt 10", !strcmp(v, "10"));
    pad_mouse_opt_fmt(v, sizeof v, 1.0f); check("fmt 1", !strcmp(v, "1"));
    pad_mouse_opt_fmt(v, sizeof v, 0.0f); check("fmt clamps to 0.5", !strcmp(v, "0.5"));

    /* ini: no file, empty, section present, key present (any case, any section), neighbours untouched */
    up("", b, sizeof b, "mouse_sensitivity", "3.5");
    check("empty -> [input] section", !strcmp(b, "[input]\nmouse_sensitivity = 3.5\n"));
    up("[game]\neboot = a.elf\n", b, sizeof b, "mouse_invert_y", "1");
    check("no [input] -> appended", !strcmp(b, "[game]\neboot = a.elf\n\n[input]\nmouse_invert_y = 1\n"));
    up("[input]\nmouse_look = 1\n[display]\nwindow_size = 1080p\n", b, sizeof b, "mouse_ads_sensitivity", "2");
    check("[input] present -> under its header", !strcmp(b, "[input]\nmouse_ads_sensitivity = 2\nmouse_look = 1\n[display]\nwindow_size = 1080p\n"));
    up("[input]\r\nMouse_Sensitivity=4\r\nmouse_look = 1\r\n", b, sizeof b, "mouse_sensitivity", "6.5");
    check("key replaced in place (case, CRLF)", !strcmp(b, "[input]\r\nMouse_Sensitivity= 6.5\r\nmouse_look = 1\r\n"));
    /* round trip through the game's own reader semantics: value text parses back to the same float */
    float vals[] = {0.5f, 2.5f, 7.0f, 10.0f};
    int rt = 1;
    for (int i = 0; i < 4; i++) {
        pad_mouse_opt_fmt(v, sizeof v, vals[i]);
        up("[game]\nlang = en\n", b, sizeof b, "mouse_sensitivity", v);
        up(b, c, sizeof c, "mouse_sensitivity", v);   /* twice = idempotent */
        const char* p = strstr(c, "mouse_sensitivity = ");
        rt &= p && strtof(p + 20, NULL) == vals[i] && strstr(c, "[input]") == strrchr(c, '[');
    }
    check("ini round trip (4 values, idempotent)", rt);
    up("[input]\nmouse_sensitivity = 1\n", b, 10, "mouse_sensitivity", "2");
    check("overflow reported", !strcmp(b, "<overflow>"));

    /* runtime setter */
    float u, z; int inv;
    pad_mouse_opts_set(4.0f, 2.0f, 1);
    pad_mouse_opts_get(&u, &z, &inv);
    check("set/get", u == 4.0f && z == 2.0f && inv == 1);
    check("sens follows the user number", fabsf(pad_mouse_cfg()->sens - 4.0f * PAD_MOUSE_DEG_PER_COUNT * 0.017453292519943295f) < 1e-9f);
    pad_mouse_opts_set(500.0f, -1.0f, 7);
    pad_mouse_opts_get(&u, &z, &inv);
    check("set clamps", u == 10.0f && z == 0.5f && inv == 1);
    printf("%s\n", fails ? "FAIL" : "all ok");
    return fails != 0;
}
