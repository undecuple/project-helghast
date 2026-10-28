/* Host test for KZ3 patch 2062 (libs/video/rsx_window_size.h in the patched toolkit): KZ3_WINDOW_SIZE parsing, the
 * aspect-correct fit into a work area, and the WM_SIZING correction that keeps the guest's 16:9 while dragging.
 * Platform-free; run by test_window_size.sh. */
#include "rsx_window_size.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        printf((c) ? "[PASS] " : "[FAIL] ");                                                                           \
        if (!(c)) fails++;                                                                                             \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

int main(void)
{
    int w = 0, h = 0, r[4];
    CHECK(rsx_win_parse_size("1920x1080", &w, &h) && w == 1920 && h == 1080, "1920x1080");
    CHECK(rsx_win_parse_size("1080p", &w, &h) && w == 1920 && h == 1080, "1080p");
    CHECK(rsx_win_parse_size("1440P", &w, &h) && w == 2560 && h == 1440, "1440P");
    CHECK(rsx_win_parse_size("4K", &w, &h) && w == 3840 && h == 2160, "4K");
    CHECK(rsx_win_parse_size("2560X1600", &w, &h) && w == 2560 && h == 1600, "2560X1600");
    CHECK(!rsx_win_parse_size("", &w, &h) && !rsx_win_parse_size("1920", &w, &h) &&
              !rsx_win_parse_size("1920x", &w, &h) && !rsx_win_parse_size("x1080", &w, &h) &&
              !rsx_win_parse_size("1920x1080x", &w, &h) && !rsx_win_parse_size("100x50", &w, &h) &&
              !rsx_win_parse_size("99999x1080", &w, &h) && !rsx_win_parse_size("1080i", &w, &h) &&
              !rsx_win_parse_size(NULL, &w, &h),
          "bad sizes refused");

    w = 0;
    h = 0;
    rsx_win_fit(16, 9, 1920, 1080, &w, &h);
    CHECK(w == 1920 && h == 1080, "fit 16:9 into 1920x1080 (%dx%d)", w, h);
    w = 0;
    h = 0;
    rsx_win_fit(16, 9, 2560, 1600, &w, &h);
    CHECK(w == 2560 && h == 1440, "16:10 request -> 2560x1440 (%dx%d)", w, h);
    w = 1920;
    h = 1080;
    rsx_win_fit(16, 9, 1264, 689, &w, &h); /* Xvfb 1280x720 minus a frame */
    CHECK(w == 1224 && h == 689 && w * 9 <= h * 16 + 16, "1080p on a 1280x720 screen shrinks to %dx%d", w, h);
    w = 1280;
    h = 720;
    rsx_win_fit(16, 9, 1904, 1001, &w, &h);
    CHECK(w == 1280 && h == 720, "a size that fits is kept (%dx%d)", w, h);

    /* WM_SIZING with a 16x39 frame (8+8 sides, 31 caption + 8 bottom) */
    r[0] = 100;
    r[1] = 100;
    r[2] = 100 + 1600 + 16;
    r[3] = 100 + 500 + 39;
    rsx_win_sizing(RSX_WMSZ_RIGHT, r, 16, 39, 16, 9, 320);
    CHECK(r[0] == 100 && r[1] == 100 && r[2] - r[0] - 16 == 1600 && r[3] - r[1] - 39 == 900,
          "right edge drives height (%dx%d)", r[2] - r[0] - 16, r[3] - r[1] - 39);
    r[0] = 100;
    r[1] = 100;
    r[2] = 100 + 1000 + 16;
    r[3] = 100 + 1080 + 39;
    rsx_win_sizing(RSX_WMSZ_TOP, r, 16, 39, 16, 9, 320);
    CHECK(r[3] == 100 + 1080 + 39 && r[2] - r[0] - 16 == 1920 && r[3] - r[1] - 39 == 1080 && r[0] == 100,
          "top edge drives width, bottom stays (%dx%d)", r[2] - r[0] - 16, r[3] - r[1] - 39);
    r[0] = 100;
    r[1] = 100;
    r[2] = 100 + 1280 + 16;
    r[3] = 100 + 1000 + 39;
    rsx_win_sizing(RSX_WMSZ_TOPLEFT, r, 16, 39, 16, 9, 320);
    CHECK(r[2] == 100 + 1280 + 16 && r[3] == 100 + 1000 + 39 && r[2] - r[0] - 16 == 1778 && r[3] - r[1] - 39 == 1000,
          "top-left corner: the larger size wins, right/bottom stay (%dx%d)", r[2] - r[0] - 16, r[3] - r[1] - 39);
    r[0] = 0;
    r[1] = 0;
    r[2] = 50;
    r[3] = 60;
    rsx_win_sizing(RSX_WMSZ_BOTTOMRIGHT, r, 16, 39, 16, 9, 320);
    CHECK(r[2] - 16 == 320 && r[3] - 39 == 180, "minimum 320x180 (%dx%d)", r[2] - 16, r[3] - 39);

    printf(fails ? "[test_window_size] FAIL (%d)\n" : "[test_window_size] PASS\n", fails);
    return fails ? 1 : 0;
}
