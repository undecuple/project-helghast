/* Host test for KZ3 patch 2113 (RSX pass 106): the 2D layer kept 16:9 (libs/video/rsx_hud169.h) and its display.ini
 * key (rsx_display_mode.h). No game data, no GPU. Run through test_hud169.sh. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "rsx_display_mode.h"
#include "rsx_hud169.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); fails++; } printf("\n"); } while (0)
static int near(double a, double b, double e) { return fabs(a - b) <= e; }

/* buffer pixel x of guest pixel x through a full 1280-wide viewport epilogue squeezed by kx */
static double map_x(double gx, float kx)
{
    float xf[8] = { 1, -1, 1, 0, 0, 0, 0, 0 };   /* full guest viewport: scale 640/(1280/2), offset 0 */
    rsx_h169_xf(xf, kx, 1.0f);
    const double clip = gx / 640.0 - 1.0;          /* the guest's own ortho: pixel -> clip */
    const double ndc = clip * xf[0] + xf[4];
    return (ndc + 1.0) * 640.0;
}

int main(void)
{
    float kx, ky;
    CHECK(rsx_h169_factors(2560, 720, &kx, &ky) && near(kx, 0.5, 1e-6) && ky == 1.0f, "32:9 2560x720: kx 0.5 (%g)", kx);
    CHECK(rsx_h169_factors(1720, 720, &kx, &ky) && near(kx, (16.0 / 9.0) / (1720.0 / 720.0), 1e-6) && ky == 1.0f,
          "21:9 1720x720: kx %g", kx);
    CHECK(rsx_h169_factors(1280, 800, &kx, &ky) && kx == 1.0f && near(ky, 0.9, 1e-6), "16:10 1280x800: ky 0.9 (%g)", ky);
    CHECK(!rsx_h169_factors(1280, 720, &kx, &ky) && kx == 1.0f && ky == 1.0f, "16:9 1280x720: inactive");
    CHECK(!rsx_h169_factors(1920, 1080, &kx, &ky), "16:9 1920x1080: inactive");
    CHECK(!rsx_h169_factors(1366, 768, &kx, &ky), "1366x768 (within 1e-3 of 16:9): inactive");
    CHECK(!rsx_h169_factors(0, 720, &kx, &ky), "no client: inactive");

    /* transform: the 1280 guest columns land on the centred box 320..960 at 32:9; centre fixed; y untouched */
    CHECK(near(map_x(0, 0.5f), 320, 1e-4) && near(map_x(1280, 0.5f), 960, 1e-4) && near(map_x(640, 0.5f), 640, 1e-4),
          "xf squeeze 32:9: 0->%g 640->%g 1280->%g", map_x(0, 0.5f), map_x(640, 0.5f), map_x(1280, 0.5f));
    { float xf[8] = { 1, -1, 1, 0, 0, 0, 0.25f, 0 };
      rsx_h169_xf(xf, 1.0f, 0.9f);
      CHECK(xf[0] == 1 && near(xf[1], -0.9, 1e-6) && xf[2] == 1 && xf[4] == 0 && xf[5] == 0 && xf[6] == 0.25f,
            "xf squeeze 16:10: y scale -0.9, z untouched"); }
    { float xf[8] = { 1, -1, 1, 0, 0, 0, 0, 0 }; float ref[8]; memcpy(ref, xf, sizeof ref);
      rsx_h169_xf(xf, 1.0f, 1.0f);
      CHECK(!memcmp(xf, ref, sizeof ref), "xf at k = 1: bit-identical"); }

    /* scissors in guest pixels */
    { long r[4] = { 0, 0, 1280, 720 }; rsx_h169_scissor(r, 1280, 720, 0.5f, 1.0f);
      CHECK(r[0] == 320 && r[1] == 0 && r[2] == 960 && r[3] == 720, "no scissor -> box 320,0..960,720"); }
    { long r[4] = { 0, 0, 4096, 4096 }; rsx_h169_scissor(r, 1280, 720, 0.5f, 1.0f);
      CHECK(r[0] == 320 && r[2] == 960 && r[3] == 720, "4096 reset scissor clamped to the box"); }
    { long r[4] = { 151, 174, 151 + 305, 174 + 420 }; rsx_h169_scissor(r, 1280, 720, 0.5f, 1.0f);   /* GUM panel */
      CHECK(r[0] == 395 && r[2] == 548 && r[1] == 174 && r[3] == 594, "GUM panel 151+305 -> %ld..%ld", r[0], r[2]); }
    { long r[4] = { 161, 174, 161 + 295, 174 + 36 }; rsx_h169_scissor(r, 1280, 720, 1.0f, 0.9f);   /* text line, 16:10 */
      CHECK(r[0] == 161 && r[2] == 456 && r[1] == 192 && r[3] == 225, "16:10 text line y 174+36 -> %ld..%ld", r[1], r[3]); }
    { long r[4] = { 1039, 0, 1039, 720 }; rsx_h169_scissor(r, 1280, 720, 0.5f, 1.0f);
      CHECK(r[0] == r[2], "empty scissor stays empty (%ld..%ld)", r[0], r[2]); }
    { int ok = 1;   /* monotone, inside the box, never loses a pixel the squeezed draw covers */
      for (long a = 0; a <= 1280 && ok; a += 7) for (long b = a; b <= 1280 && ok; b += 13) {
          long r[4] = { a, 0, b, 720 }; rsx_h169_scissor(r, 1280, 720, 0.762f, 1.0f);
          const double ea = 640 + (a - 640) * 0.762, eb = 640 + (b - 640) * 0.762;
          if (r[0] > ea + 1e-9 || (b > a && r[2] < eb - 1e-9) || r[0] < 152 || r[2] > 1128) ok = 0; }
      CHECK(ok, "21:9 scissors cover their squeezed span and stay in the box"); }

    /* menu mouse inverse: buffer -> UI pixel, bars are no hit, round trip */
    { float gx, gy;
      CHECK(rsx_h169_ui_point(320.5f, 100.5f, 1280, 720, 0.5f, 1.0f, &gx, &gy) && near(gx, 1.0, 1e-4) && near(gy, 100.5, 1e-4),
            "32:9 buffer 320.5 -> UI %g", gx);
      CHECK(!rsx_h169_ui_point(100.5f, 100.5f, 1280, 720, 0.5f, 1.0f, &gx, &gy), "32:9 left bar: no hit");
      CHECK(!rsx_h169_ui_point(1000.5f, 100.5f, 1280, 720, 0.5f, 1.0f, &gx, &gy), "32:9 right bar: no hit");
      CHECK(!rsx_h169_ui_point(640.5f, 20.5f, 1280, 720, 1.0f, 0.9f, &gx, &gy), "16:10 top bar: no hit");
      int ok = 1;
      for (int x = 0; x < 1280 && ok; x++) {
          const double b = map_x(x + 0.5, 0.5f);
          if (!rsx_h169_ui_point((float)b, 360.0f, 1280, 720, 0.5f, 1.0f, &gx, &gy) || (int)floorf(gx) != x) ok = 0; }
      CHECK(ok, "32:9 every guest column maps forward and back to itself"); }

    /* patch 2115: the HUD's world projection with the box frustum lands where the 3D view (Hor+/Vert+ frustum, drawn
     * 1280x720 and stretched to the window) shows the point, after the squeeze; the unscaled frustum misses by kx. */
    { const float top = 0.5f, aspects[3][2] = { { 3840, 1080 }, { 2560, 1080 }, { 1920, 1200 } };
      for (int a = 0; a < 3; a++) {
        float kx, ky;
        rsx_h169_factors((unsigned)aspects[a][0], (unsigned)aspects[a][1], &kx, &ky);
        const double r = (aspects[a][0] / aspects[a][1]) / (16.0 / 9.0);
        const float right = (float)(top * 16.0 / 9.0 * (r > 1.0 ? r : 1.0)), top_v = (float)(r < 1.0 ? top / r : top);
        unsigned char f[0x48] = { 0 };
        rsx_h169_setbef(f + 0x0C, -right); rsx_h169_setbef(f + 0x10, right);
        rsx_h169_setbef(f + 0x14, top_v);  rsx_h169_setbef(f + 0x18, -top_v);
        rsx_h169_setbef(f + 0x1C, 0.25f);   /* a field after the four: untouched */
        rsx_h169_frustum(f, kx, ky);
        const double l2 = rsx_h169_bef(f + 0x0C), r2 = rsx_h169_bef(f + 0x10), t2 = rsx_h169_bef(f + 0x14), b2 = rsx_h169_bef(f + 0x18);
        CHECK(near(r2, top * 16.0 / 9.0, 1e-5) && near(l2, -r2, 1e-6) && near(t2, top, 1e-5) && near(b2, -t2, 1e-6)
              && rsx_h169_bef(f + 0x1C) == 0.25f, "2115 %gx%g: box frustum = the 16:9 one (right %g top %g)",
              aspects[a][0], aspects[a][1], r2, t2);
        int ok = 1, drift = 0;
        for (int i = -4; i <= 4; i++) for (int j = -3; j <= 3; j++) {
          const double vx = i * 0.21, vy = j * 0.13;   /* view-space x/z, y/z of a world point, inside and outside the box */
          const double bx = 640.0 + vx / right * 640.0, by = 360.0 - vy / top_v * 360.0;            /* 3D view pixel */
          const double hx = 640.0 + vx / r2 * 640.0, hy = 360.0 - vy / t2 * 360.0;                  /* HUD guest pixel */
          const double sx = 640.0 + (hx - 640.0) * kx, sy = 360.0 + (hy - 360.0) * ky;              /* after 2113 */
          const double ux = 640.0 + (640.0 + vx / right * 640.0 - 640.0) * kx;                      /* unscaled frustum */
          if (!near(sx, bx, 1e-3) || !near(sy, by, 1e-3)) ok = 0;
          if (r > 1.0 && i && !near(ux, bx, 0.5)) drift = 1;
          if (r > 1.0 && i && near(sx - map_x(hx, kx), 0.0, 1e-3) == 0) ok = 0;   /* same squeeze as the epilogue */
        }
        CHECK(ok && (r < 1.0 || drift), "2115 %gx%g: marker on its target after the squeeze (unscaled drifts: %d)",
              aspects[a][0], aspects[a][1], drift);
      }
      unsigned char f[0x48] = { 0 };
      rsx_h169_setbef(f + 0x10, 0.888889f); rsx_h169_setbef(f + 0x14, 0.5f);
      rsx_h169_frustum(f, 1.0f, 1.0f);
      CHECK(rsx_h169_bef(f + 0x10) == 0.888889f && rsx_h169_bef(f + 0x14) == 0.5f, "2115 16:9: frustum unchanged"); }

    /* display.ini key */
    { char t[2048];
      const int n = rsx_dm_ini_write_ext3(t, sizeof t, RSX_DM_WINDOWED, RSX_DM_STRETCH, RSX_DM_BILINEAR, 1, 1, RSX_DM_FPS_30, 0, 0);
      CHECK(n > 0 && (size_t)n < sizeof t && rsx_dm_ini_hud169(t) == 0 && rsx_dm_ini_fov(t) == 0, "ini: hud_169 = off round trip");
      rsx_dm_ini_write_ext3(t, sizeof t, RSX_DM_WINDOWED, RSX_DM_STRETCH, RSX_DM_BILINEAR, 1, 1, RSX_DM_FPS_30, 90, 1);
      CHECK(rsx_dm_ini_hud169(t) == 1 && rsx_dm_ini_fov(t) == 90 && rsx_dm_ini_parse(t) == RSX_DM_WINDOWED, "ini: hud_169 = on, other keys kept");
      CHECK(rsx_dm_ini_hud169("[display]\nmode = windowed\n") == -1, "ini: missing key -> -1 (default on)"); }

    printf("%s\n", fails ? "test_hud169: FAILED" : "test_hud169: all passed");
    return fails ? 1 : 0;
}
