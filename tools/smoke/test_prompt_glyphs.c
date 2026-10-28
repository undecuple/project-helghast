/* Host test for KZ3 patch 2090 (stage 1, RSX pass 85): rsx_prompt_glyphs.h of the patched toolkit.
 * Checks, on synthetic atlases (no game data): PS3 keeps the atlas; PS4/PS5 changes exactly SELECT and START,
 * Xbox exactly its 12 cells, keyboard/mouse all 18; no block outside the changed cells changes; the Xbox letters
 * get their colours (A green, B red, X blue, Y yellow) on a round, opaque body with transparent corners; the trigger
 * body has the bowl bottom; the PS4 SELECT options differ; the shipped 2039 keycaps stay reachable byte for byte;
 * drawing is deterministic; nothing is drawn without a text source where text is needed. */
#include "rsx_prompt_glyphs.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c)                                                                                                         \
            printf("[PASS] " __VA_ARGS__);                                                                             \
        else {                                                                                                         \
            printf("[FAIL] " __VA_ARGS__);                                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
        printf("\n");                                                                                                  \
    } while (0)

static int kb_active(void)
{
    return 1;
}
static int key_for(const char* in, int slot)
{
    static const struct {
        const char* in;
        int a;
    } d[] = {{"cross", 0x20}, {"circle", 0x45},  {"square", 0x52},  {"triangle", 0x51}, {"l1", 0x46},
             {"r1", 0x01},    {"l2", 0x43},      {"r2", 0x47},      {"l3", 0xA0},       {"r3", 0x02},
             {"start", 0x1B}, {"select", 0x09},  {"up", 0x26},      {"down", 0x28},     {"left", 0x25},
             {"right", 0x27}, {"ls_up", 0x57},   {"ls_down", 0x53}, {"ls_left", 0x41},  {"ls_right", 0x44},
             {"rs_up", 0x49}, {"rs_down", 0x4B}, {"rs_left", 0x4A}, {"rs_right", 0x4C}};
    if (slot) return 0;
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
        if (!strcmp(in, d[i].in)) return d[i].a;
    return 0;
}
static const char* key_name(int vk)
{
    static char b[8];
    switch (vk) {
    case 0x01:
        return "Mouse1";
    case 0x02:
        return "Mouse2";
    case 0x09:
        return "Tab";
    case 0x1B:
        return "Escape";
    case 0x20:
        return "Space";
    case 0x25:
        return "Left";
    case 0x26:
        return "Up";
    case 0x27:
        return "Right";
    case 0x28:
        return "Down";
    case 0xA0:
        return "LeftShift";
    default:
        snprintf(b, sizeof b, "%c", vk);
        return b;
    }
}
static int text_mask(const char* t, int max_px, unsigned char* m, int w, int h) /* a solid bar per character */
{
    int n = (int)strlen(t), cw = max_px * 6 / 10;
    if (n * cw > w) cw = w / n;
    const int x0 = (w - n * cw) / 2, y0 = (h - max_px) / 2;
    for (int i = 0; i < n; i++)
        for (int y = y0; y < y0 + max_px; y++)
            for (int x = x0 + i * cw + 1; x < x0 + (i + 1) * cw - 1; x++)
                if (x >= 0 && y >= 0 && x < w && y < h) m[y * w + x] = 255;
    return 1;
}
static int thin_calls, variant;
static int text_thin(const char* t, int max_px, unsigned char* m, int w, int h) /* the thin face: calls are counted */
{
    thin_calls++;
    return text_mask(t, max_px, m, w, h);
}
static int pad_variant(void) { return variant; }
static const rsx_keycap_source SRC = {kb_active, key_for, key_name, text_mask, text_thin, pad_variant, NULL};

static uint32_t rng = 777;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* Texel (x, y) of a BC3 atlas. */
static void texel(const uint8_t* a, int w, int x, int y, uint8_t out[4])
{
    uint8_t px[16][4];
    kc_bc3_decode(a + ((size_t)(y / 4) * (w / 4) + x / 4) * 16, px);
    memcpy(out, px[(y & 3) * 4 + (x & 3)], 4);
}

static const kc_cell* cell_of(int kind, const char* in)
{
    const kc_cell* c = kind == KC_HUD ? kc_hud_cells : kc_menu_cells;
    for (int i = 0; i < 18; i++)
        if (!strcmp(c[i].input, in)) return &c[i];
    return NULL;
}

/* Blocks changed inside / outside the union of the cells of `names` (NULL = any cell). */
static void diff(const uint8_t* a, const uint8_t* b, int w, int h, int kind, const char* const* names, int nn, int* in,
                 int* out)
{
    *in = *out = 0;
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            const size_t o = ((size_t)by * (w / 4) + bx) * 16;
            if (!memcmp(a + o, b + o, 16)) continue;
            int under = 0;
            for (int i = 0; i < nn && !under; i++) {
                const kc_cell* k = cell_of(kind, names[i]);
                if (k && bx * 4 + 3 >= k->l && bx * 4 <= k->r && by * 4 + 3 >= k->t && by * 4 <= k->b) under = 1;
            }
            if (under)
                (*in)++;
            else
                (*out)++;
        }
}

int main(void)
{
    const uint32_t W = 1024, H = 512, N = W * H, MW = 512, MH = 128, MN = MW * MH;
    uint8_t* hud = malloc(N);
    uint8_t* menu = malloc(MN);
    for (uint32_t i = 0; i < N; i++) hud[i] = (uint8_t)rnd();
    for (uint32_t i = 0; i < MN; i++) menu[i] = (uint8_t)rnd();
    int n = -1;

    /* 0. without a source nothing that needs text or keys is drawn for keyboard/mouse */
    uint8_t* p0 = rsx_pg_patch(hud, W, H, N, KC_HUD, PG_KBM, 0, &n);
    CHECK(p0 == NULL && n == 0, "keyboard/mouse without a key source keeps the atlas (%d cells)", n);
    free(p0);
    rsx_keycaps_set_source(&SRC);

    /* 1. PS3 = the game's atlas */
    CHECK(rsx_pg_patch(hud, W, H, N, KC_HUD, PG_PS3, 0, &n) == NULL && n == 0, "PS3 keeps the HUD atlas");
    CHECK(rsx_pg_patch(menu, MW, MH, MN, KC_MENU, PG_PS3, 0, &n) == NULL && n == 0, "PS3 keeps the menu atlas");
    CHECK(rsx_pg_patch(hud, W, H, N, KC_NONE, PG_XBOX, 0, &n) == NULL, "a texture that is no atlas is never drawn");

    /* 2. PS4/PS5: SELECT and START only */
    static const char* const ps4[] = {"select", "start"};
    static const char* const xb[] = {"square", "cross", "triangle", "circle", "l1",     "r1",
                                     "l2",     "r2",    "l3",       "r3",     "select", "start"};
    int in, out;
    for (int k = 0; k < 2; k++) {
        const uint8_t* src = k ? menu : hud;
        const int w = k ? MW : W, h = k ? MH : H, kind = k ? KC_MENU : KC_HUD;
        const uint32_t bytes = k ? MN : N;
        uint8_t* p = rsx_pg_patch(src, w, h, bytes, kind, PG_PS4, 0, &n);
        diff(src, p, w, h, kind, ps4, 2, &in, &out);
        CHECK(p && n == 2 && in > 20 && out == 0, "%s PS4/PS5: 2 cells (%d), %d blocks under them, %d elsewhere",
              k ? "menu" : "HUD", n, in, out);
        uint8_t* q = rsx_pg_patch(src, w, h, bytes, kind, PG_PS4, PG_OPT_PS4_SHARE, &n);
        const kc_cell* s = cell_of(kind, "select");
        const kc_cell* st = cell_of(kind, "start");
        int sel_diff = 0, start_diff = 0;
        for (int y = s->t; y <= s->b; y++)
            for (int x = s->l; x <= s->r; x++) {
                uint8_t a[4], b[4];
                texel(p, w, x, y, a);
                texel(q, w, x, y, b);
                sel_diff += memcmp(a, b, 4) != 0;
            }
        for (int y = st->t; y <= st->b; y++)
            for (int x = st->l; x <= st->r; x++) {
                uint8_t a[4], b[4];
                texel(p, w, x, y, a);
                texel(q, w, x, y, b);
                start_diff += memcmp(a, b, 4) != 0;
            }
        CHECK(sel_diff > 30 && start_diff == 0, "%s PS4/PS5 SHARE option changes SELECT only (%d / %d texels)",
              k ? "menu" : "HUD", sel_diff, start_diff);
        free(p);
        free(q);

        p = rsx_pg_patch(src, w, h, bytes, kind, PG_XBOX, 0, &n);
        diff(src, p, w, h, kind, xb, 12, &in, &out);
        CHECK(p && n == 12 && in > 100 && out == 0, "%s Xbox: 12 cells (%d), %d blocks under them, %d elsewhere",
              k ? "menu" : "HUD", n, in, out);
        uint8_t* p2 = rsx_pg_patch(src, w, h, bytes, kind, PG_XBOX, 0, &n);
        CHECK(p2 && !memcmp(p, p2, bytes), "%s Xbox drawing is deterministic", k ? "menu" : "HUD");
        free(p2);

        /* 3. letter colours at the face centre, opaque body, transparent cell corner */
        static const struct {
            const char* in;
            int ch;
            const char* what;
        } col[] = {
            {"cross", 1, "A green"}, {"circle", 0, "B red"}, {"square", 2, "X blue"}, {"triangle", 3, "Y yellow"}};
        for (int i = 0; i < 4; i++) {
            const kc_cell* c = cell_of(kind, col[i].in);
            float bx[4];
            pg_cell_box(col[i].in, kind == KC_MENU, c->r - c->l + 1, c->b - c->t + 1, bx);
            const int cx = c->l + (int)((bx[0] + bx[2]) * 0.5f), cy = c->t + (int)((bx[1] + bx[3]) * 0.5f);
            uint8_t t[4], corner[4];
            texel(p, w, cx, cy, t);
            texel(p, w, c->l, c->t, corner);
            int ok;
            if (col[i].ch == 3)
                ok = t[0] > 150 && t[1] > 130 && t[2] < 120;
            else
                ok = t[col[i].ch] > 140 && t[col[i].ch] > t[(col[i].ch + 1) % 3] + 40 &&
                     t[col[i].ch] > t[(col[i].ch + 2) % 3] + 40;
            CHECK(ok && t[3] > 200 && corner[3] < 40, "%s %s at the face centre (%d,%d,%d a=%d), corner a=%d",
                  k ? "menu" : "HUD", col[i].what, t[0], t[1], t[2], t[3], corner[3]);
        }
        /* 4. trigger: bowl bottom (the box's bottom corners transparent, its top middle opaque and dark) */
        {
            const kc_cell* c = cell_of(kind, "l2");
            float bx[4];
            pg_cell_box("l2", kind == KC_MENU, c->r - c->l + 1, c->b - c->t + 1, bx);
            uint8_t top[4], bl[4], br[4];
            texel(p, w, c->l + (int)(bx[0] + 4), c->t + (int)bx[1] + 2, top);
            texel(p, w, c->l + (int)bx[0] + 1, c->t + (int)bx[3] - 1, bl);
            texel(p, w, c->l + (int)bx[2] - 2, c->t + (int)bx[3] - 1, br);
            CHECK(top[3] > 200 && top[0] < 90 && bl[3] < 60 && br[3] < 60,
                  "%s LT body: top a=%d lum=%d, bottom corners a=%d/%d", k ? "menu" : "HUD", top[3], top[0], bl[3],
                  br[3]);
        }
        free(p);

        /* 5. keyboard/mouse: all 18 cells; the 2039 look is rsx_keycaps_patch byte for byte */
        p = rsx_pg_patch(src, w, h, bytes, kind, PG_KBM, 0, &n);
        diff(src, p, w, h, kind, NULL, 0, &in, &out);
        int out2 = 0;
        {
            const char* all[18];
            const kc_cell* c = kind == KC_HUD ? kc_hud_cells : kc_menu_cells;
            for (int i = 0; i < 18; i++) all[i] = c[i].input;
            diff(src, p, w, h, kind, all, 18, &in, &out2);
        }
        CHECK(p && n == 18 && out2 == 0, "%s keyboard/mouse: 18 cells (%d), %d blocks elsewhere", k ? "menu" : "HUD", n,
              out2);
        int n2 = 0, n3 = 0;
        uint8_t* q2 = rsx_pg_patch(src, w, h, bytes, kind, PG_KBM, PG_OPT_KBM_2039, &n2);
        uint8_t* q3 = rsx_keycaps_patch(src, w, h, bytes, kind, &n3);
        CHECK(q2 && q3 && n2 == n3 && !memcmp(q2, q3, bytes), "%s PG_OPT_KBM_2039 = rsx_keycaps_patch (%d cells)",
              k ? "menu" : "HUD", n2);
        CHECK(p && q2 && memcmp(p, q2, bytes), "%s matched keycaps differ from the 2039 look", k ? "menu" : "HUD");
        free(p);
        free(q2);
        free(q3);
    }

    /* 6. one cell drawn on its own: the box table is used (nothing drawn left of / above the game's ink box) */
    {
        uint8_t cell[52 * 29 * 4];
        CHECK(rsx_pg_draw_cell(PG_XBOX, 0, "up", 0, cell, 32, 39) == 0, "Xbox keeps the d-pad cells");
        CHECK(rsx_pg_draw_cell(PG_XBOX, 0, "ls", 0, cell, 42, 28) == 0, "Xbox keeps the stick-move cells");
        CHECK(rsx_pg_draw_cell(PG_PS4, 0, "cross", 0, cell, 38, 38) == 0, "PS4/PS5 keeps the face cells");
        rsx_pg_draw_cell(PG_XBOX, 0, "l2", 0, cell, 52, 29);
        int stray = 0;
        for (int y = 0; y < 29; y++)
            for (int x = 0; x < 3; x++) stray += cell[(y * 52 + x) * 4 + 3] > 8;
        CHECK(stray == 0, "Xbox LT stays inside the game's ink box (x 4..46): %d stray texels", stray);
    }
    /* patch 2094 (choices 1, 3, 6) */
    {
        int n;
        thin_calls = 0;
        uint8_t* x = rsx_pg_patch(hud, W, H, N, KC_HUD, PG_XBOX, 0, &n);
        CHECK(x && thin_calls > 0, "labels go through the thin face when the source has one (%d calls)", thin_calls);
        free(x);
        variant = 0;
        uint8_t* ps4 = rsx_pg_patch(hud, W, H, N, KC_HUD, PG_PS4, 0, &n);
        variant = 1;
        uint8_t* ps5 = rsx_pg_patch(hud, W, H, N, KC_HUD, PG_PS4, 0, &n);
        variant = 0;
        const char* sel[] = { "select" };
        int in, out;
        CHECK(ps4 && ps5 && (diff(ps4, ps5, W, H, KC_HUD, sel, 1, &in, &out), in > 5 && out == 0),
              "a DualSense changes the SELECT cell only (%d blocks in, %d out)", in, out);
        free(ps4);
        free(ps5);
        /* the controls page's pad picture: a 512x256 DXT5 texture of kind KC_CTL */
        const uint32_t CW = 512, CH = 256, CN = CW * CH;
        uint8_t* ctl = malloc(CN);
        for (uint32_t i = 0; i < CN; i++) ctl[i] = (uint8_t)rnd();
        CHECK(rsx_keycaps_kind(ctl, CW, CH, CN) == KC_NONE, "random 512x256 data is no pad picture");
        CHECK(rsx_pg_patch(ctl, CW, CH, CN, KC_CTL, PG_PS3, 0, &n) == NULL, "PS3 keeps the game's pad picture");
        CHECK(rsx_pg_patch(ctl, CW, CH, CN, KC_CTL, PG_KBM, 0, &n) == NULL, "keyboard keeps the game's pad picture");
        uint8_t* a = rsx_pg_patch(ctl, CW, CH, CN, KC_CTL, PG_PS4, 0, &n);
        uint8_t* b = rsx_pg_patch(ctl, CW, CH, CN, KC_CTL, PG_XBOX, 0, &n);
        uint8_t* a2 = rsx_pg_patch(ctl, CW, CH, CN, KC_CTL, PG_PS4, 0, &n);
        CHECK(a && b && a2 && !memcmp(a, a2, CN) && memcmp(a, b, CN), "PS4 and Xbox pad pictures: drawn, deterministic, different");
        int opaque = 0;
        uint8_t t4[4];
        for (int y = 0; y < (int)CH; y += 2) for (int xx = 0; xx < (int)CW; xx += 2) { texel(a, CW, xx, y, t4); opaque += t4[3] > 128; }
        CHECK(opaque > 300 && opaque < 20000, "the PS4 picture is line art on a transparent ground (%d opaque samples)", opaque);
        texel(a, CW, 0, 0, t4);
        CHECK(t4[3] == 0, "the picture corner is transparent");
        free(a); free(b); free(a2); free(ctl);
    }
    free(hud);
    free(menu);
    printf("[test_prompt_glyphs] %d failures\n", fails);
    return fails != 0;
}
