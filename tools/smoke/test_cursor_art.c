/* Host test for patch 2111's menu pointer pixels (libs/input/pad_cursor_art.h). CURSOR_DUMP=path writes the 32 px
 * cursor as a PAM (RGB_ALPHA, straight alpha) for a side-by-side picture. */
#include "pad_cursor_art.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, m)                                                                                                    \
    do {                                                                                                               \
        if (c)                                                                                                         \
            printf("[PASS] %s\n", m);                                                                                  \
        else {                                                                                                         \
            printf("[FAIL] %s\n", m);                                                                                  \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static unsigned A(const uint32_t* p, int s, int x, int y)
{
    return p[y * s + x] >> 24;
}

int main(void)
{
    static uint32_t px[64 * 64];
    const int sizes[3] = {32, 48, 64};
    for (int k = 0; k < 3; k++) {
        const int s = sizes[k];
        pad_cursor_render(px, s);
        int opaque = 0, bad = 0;
        for (int i = 0; i < s * s; i++) {
            const uint32_t a = px[i] >> 24, r = (px[i] >> 16) & 0xFF, g = (px[i] >> 8) & 0xFF, b = px[i] & 0xFF;
            if (r > a || g > a || b > a) bad++; /* premultiplied: no channel above alpha */
            if (a == 255) opaque++;
        }
        char m[96];
        snprintf(m, sizeof m, "%d px: premultiplied, no channel above alpha", s);
        CHECK(bad == 0, m);
        snprintf(m, sizeof m, "%d px: tip at the hotspot (1,1) is drawn", s);
        CHECK(A(px, s, 1, 1) > 100, m);
        snprintf(m, sizeof m, "%d px: corners away from the arrow are clear", s);
        CHECK(A(px, s, s - 1, 0) == 0 && A(px, s, s - 1, s - 1) == 0 && A(px, s, s / 2, 2) == 0, m);
        /* a point well inside the body: x = 0.12 s, y = 0.45 s */
        const uint32_t c = px[(int)(0.45f * s) * s + (int)(0.12f * s)];
        snprintf(m, sizeof m, "%d px: the body is the cream fill (0x%08X)", s, (unsigned)c);
        CHECK(c == (0xFF000000u | PAD_CURSOR_FILL), m);
        snprintf(m, sizeof m, "%d px: a solid arrow (%d opaque pixels)", s, opaque);
        CHECK(opaque > s * s / 12 && opaque < s * s / 3, m);
        if (s == 32 && getenv("CURSOR_DUMP")) {
            FILE* f = fopen(getenv("CURSOR_DUMP"), "wb");
            if (f) {
                fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", s, s);
                for (int i = 0; i < s * s; i++) {
                    const unsigned a = px[i] >> 24;
                    unsigned char q[4] = {0, 0, 0, (unsigned char)a};
                    for (int ch = 0; ch < 3; ch++)
                        q[ch] = a ? (unsigned char)((((px[i] >> (16 - 8 * ch)) & 0xFF) * 255 + a / 2) / a) : 0;
                    fwrite(q, 1, 4, f);
                }
                fclose(f);
            }
        }
    }
    printf(fails ? "FAIL: %d failure(s)\n" : "PASS: %d failure(s)\n", fails);
    return fails != 0;
}
