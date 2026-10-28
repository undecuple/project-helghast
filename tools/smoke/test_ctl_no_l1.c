/* Host test for KZ3 patch 2117 (RSX pass 110): the controls page picture without L1's leader line under a layout
 * that gives L1 no action (Modern). A synthetic 512x256 BC3 picture in the game's style (white line art in alpha:
 * a body outline crossing the L1 line, the L1 button outline, the L1 line, another line elsewhere) goes through
 * rsx_pg_patch(KC_CTL): with the callback off the PS3 picture is the game's (NULL); with it on the L1 line's texels
 * drop to the ground, the body outline stays continuous where the line crossed it, the button outline and every block
 * away from the line keep their bytes; the keyboard set keeps the game's picture; the drawn PS4 and Xbox pictures lose
 * only the L1 line. No game data. */
#include "rsx_prompt_glyphs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
static int l1_on;
static int l1_unused(void)
{
    return l1_on;
}
static int no_kb(void)
{
    return 0;
}
static const rsx_keycap_source SRC = {no_kb, NULL, NULL, NULL, NULL, NULL, l1_unused};

static float seg(float x, float y, float ax, float ay, float bx, float by)
{
    return pc_d_seg(x, y, ax, ay, bx, by);
}
static uint8_t A[256][512];
static void paint(float ax, float ay, float bx, float by, float hw, uint8_t lum)
{
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 512; x++) {
            const float d = seg((float)x, (float)y, ax, ay, bx, by);
            const float c = d <= hw ? 1.f : d >= hw + 1.f ? 0.f : hw + 1.f - d;
            const uint8_t v = (uint8_t)(lum * c + 0.5f);
            if (v > A[y][x]) A[y][x] = v;
        }
}
static void encode(uint8_t* bc3)
{
    for (int by = 0; by < 64; by++)
        for (int bx = 0; bx < 128; bx++) {
            uint8_t px[16][4];
            for (int i = 0; i < 16; i++) {
                px[i][0] = px[i][1] = px[i][2] = 255;
                px[i][3] = A[by * 4 + (i >> 2)][bx * 4 + (i & 3)];
            }
            kc_bc3_encode(px, bc3 + ((size_t)by * 128 + bx) * 16);
        }
}
static int alpha_at(const uint8_t* bc3, int x, int y)
{
    uint8_t px[16][4];
    kc_bc3_decode(bc3 + ((size_t)(y / 4) * 128 + x / 4) * 16, px);
    return px[(y & 3) * 4 + (x & 3)][3];
}

int main(void)
{
    static uint8_t pic[512 * 256];
    paint(133.f, 18.f, 124.f, 40.f, 1.0f, 72);     /* the body outline, crossing the line near x 128-130 */
    paint(135.f, 30.f, 134.f, 40.f, 0.8f, 230);    /* the L1 button's left outline */
    paint(104.f, 23.3f, 132.5f, 33.1f, 0.6f, 235); /* the L1 leader line */
    paint(49.f, 90.f, 144.f, 90.f, 0.6f, 235);     /* another line (d-pad up) */
    encode(pic);
    rsx_keycaps_set_source(&SRC);
    int n = 0;
    l1_on = 0;
    uint8_t* o = rsx_pg_patch(pic, 512, 256, sizeof pic, KC_CTL, PG_PS3, 0, &n);
    check("callback off: the PS3 picture is the game's (no copy)", o == NULL);
    free(o);
    l1_on = 1;
    o = rsx_pg_patch(pic, 512, 256, sizeof pic, KC_CTL, PG_PS3, 0, &n);
    check("Modern: the PS3 picture is a copy", o != NULL && n == 1);
    if (!o) return 1;
    int line_left = 0, body_ok = 1, outside_same = 1;
    for (int x = 104; x <= 122; x++)
        for (int y = 20; y < 36; y++)
            if (seg((float)x, (float)y, 104.f, 23.3f, 132.5f, 33.1f) < 1.0f && alpha_at(o, x, y) > 24) line_left++;
    /* the body outline: every row 18..39 still has a texel of body strength near the body's centre line */
    for (int y = 19; y < 39; y++) {
        const float cx = 133.f - 9.f * (y - 18) / 22.f;
        int best = 0;
        for (int x = (int)cx - 2; x <= (int)cx + 2; x++) {
            const int a = alpha_at(o, x, y);
            if (a > best) best = a;
        }
        if (best < 45 || best > 110) {
            body_ok = 0;
            printf("  body row %d: max alpha %d\n", y, best);
        }
    }
    for (int b = 0; b < 128 * 64; b++) {
        const int bx = b % 128, by = b / 128;
        if (bx >= 25 && bx <= 33 && by >= 4 && by <= 10) continue; /* the blocks the erase may touch */
        if (memcmp(o + b * 16, pic + b * 16, 16)) {
            outside_same = 0;
            break;
        }
    }
    int outline = 1;
    for (int y = 31; y < 40; y++)
        if (alpha_at(o, 134, y) < 150 && alpha_at(o, 135, y) < 150) outline = 0;
    check("Modern: L1 line texels gone (alpha <= 24)", line_left == 0);
    check("Modern: the body outline stays continuous across the old crossing", body_ok);
    check("Modern: the L1 button outline is kept", outline);
    check("Modern: blocks away from the line keep their bytes", outside_same);
    check("Modern: the d-pad line elsewhere is kept", alpha_at(o, 100, 90) > 200);
    free(o);
    o = rsx_pg_patch(pic, 512, 256, sizeof pic, KC_CTL, PG_KBM, 0, &n);
    check("keyboard set keeps the game's picture", o == NULL);
    free(o);
    /* drawn pads: with and without the L1 line */
    for (int set = PG_PS4; set <= PG_XBOX; set++) {
        static uint8_t a[512 * 256 * 4], b[512 * 256 * 4];
        rsx_pg_draw_pad(set, 0, a, 512, 256);
        rsx_pg_draw_pad(set, PG_OPT_NO_L1, b, 512, 256);
        int near = 0, far = 0;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 512; x++)
                if (memcmp(a + ((size_t)y * 512 + x) * 4, b + ((size_t)y * 512 + x) * 4, 4)) {
                    if (seg((float)x, (float)y, 105.f, 24.f, 133.f, 33.f) <= 3.f)
                        near++;
                    else
                        far++;
                }
        char t[96];
        snprintf(t, sizeof t, "%s picture: only the L1 line differs (%d texels near it, %d elsewhere)",
                 set == PG_PS4 ? "PS4" : "Xbox", near, far);
        check(t, near > 20 && far == 0);
    }
    printf("test_ctl_no_l1: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
