/* Host test for KZ3 patch 2039 (keycap prompts, RSX pass 39): rsx_keycaps.h of the patched toolkit.
 * Checks: BC3 encode/decode round trip, atlas fingerprints (a random texture is no atlas), pad mode leaves the atlas
 * alone, keyboard mode changes only the blocks under the game's cells, a cell decodes to an opaque dark keycap with
 * its face drawn, the signature follows device and bindings, labels. No game data: the atlases are synthetic.
 * KC_PREVIEW=<atlas.bin> <w> <h> <out.ppm>: patch a real atlas dump and write it (RGB over grey) to look at. */
#include "rsx_keycaps.h"
#include <assert.h>

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

static int s_kb = 0, s_keys_changed = 0, s_back_off = 0;   /* s_back_off: no menu tracking (2106) */
static int kb_active(void)
{
    return s_kb;
}
static int key_for(const char* in, int slot)
{
    static const struct {
        const char* in;
        int a, b;
    } d[] = {{"cross", 0x20, 0x0D}, {"circle", 0x45, 0},    {"square", 0x52, 0},   {"triangle", 0x51, 0},
             {"l1", 0x46, 0},       {"r1", 0x01, 0x4F},     {"l2", 0x43, 0xA2},    {"r2", 0x47, 0},
             {"l3", 0xA0, 0},       {"r3", 0x02, 0x56},     {"start", 0x1B, 0x50}, {"select", 0x09, 0},
             {"up", 0x26, 0},       {"down", 0x28, 0},      {"left", 0x25, 0},     {"right", 0x27, 0},
             {"ls_up", 0x57, 0},    {"ls_down", 0x53, 0},   {"ls_left", 0x41, 0},  {"ls_right", 0x44, 0},
             {"rs_up", 0x49, 0},    {"rs_down", 0x4B, 0},   {"rs_left", 0x4A, 0},  {"rs_right", 0x4C, 0},
             {"back", 0x1B, 0x08}};   /* patch 2106 defaults: circle = E, back = Escape, Backspace */
    if (s_back_off && !strcmp(in, "back")) return 0;
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
        if (!strcmp(in, d[i].in)) {
            int v = slot ? d[i].b : d[i].a;
            if (s_keys_changed && !strcmp(in, "square") && !slot) v = 0x54;
            return v;
        }
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
    case 0x08:
        return "Backspace";
    case 0x09:
        return "Tab";
    case 0x0D:
        return "Enter";
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
    case 0xA2:
        return "LeftCtrl";
    default:
        break;
    }
    if (vk >= 0x41 && vk <= 0x5A) {
        b[0] = (char)vk;
        b[1] = 0;
        return b;
    }
    return NULL;
}
static int s_text_calls;
static char s_last_text[32];
static int text_mask(const char* t, int max_px, unsigned char* m, int w, int h) /* a bar per character */
{
    s_text_calls++;
    snprintf(s_last_text, sizeof s_last_text, "%s", t);
    int n = (int)strlen(t), cw = max_px * 6 / 10, x0 = (w - n * cw) / 2, y0 = (h - max_px) / 2;
    for (int i = 0; i < n; i++)
        for (int y = y0; y < y0 + max_px; y++)
            for (int x = x0 + i * cw + 1; x < x0 + (i + 1) * cw - 1; x++)
                if (x >= 0 && y >= 0 && x < w && y < h) m[y * w + x] = 255;
    return 1;
}
static const rsx_keycap_source SRC = {kb_active, key_for, key_name, text_mask, NULL, NULL, NULL};

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

static void preview(const char* path, int w, int h, const char* out)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[FAIL] preview: no %s\n", path);
        fails++;
        return;
    }
    uint8_t* a = (uint8_t*)malloc((size_t)w * h);
    size_t got = fread(a, 1, (size_t)w * h, f);
    fclose(f);
    int kind = rsx_keycaps_kind(a, w, h, (uint32_t)got), cells = 0;
    s_kb = 1;
    uint8_t* p = rsx_keycaps_patch(a, w, h, (uint32_t)got, kind ? kind : (w == 1024 ? KC_HUD : KC_MENU), &cells);
    FILE* o = fopen(out, "wb");
    fprintf(o, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px[16][4];
            kc_bc3_decode(p + ((size_t)(y / 4) * (w / 4) + x / 4) * 16, px);
            const uint8_t* q = px[(y & 3) * 4 + (x & 3)];
            for (int k = 0; k < 3; k++) fputc((q[k] * q[3] + 40 * (255 - q[3])) / 255, o);
        }
    fclose(o);
    printf("[INFO] preview kind=%d cells=%d -> %s\n", kind, cells, out);
    free(a);
    free(p);
}

int main(int argc, char** argv)
{
    rsx_keycaps_set_source(&SRC);
    if (argc == 5) {
        preview(argv[1], atoi(argv[2]), atoi(argv[3]), argv[4]);
        return fails != 0;
    }

    /* 1. BC3 round trip of a two-tone block is exact (565-representable colours, alpha endpoints) */
    uint8_t px[16][4], back[16][4], blk[16];
    for (int i = 0; i < 16; i++) {
        int on = (i * 7) & 1;
        px[i][0] = on ? 255 : 0;
        px[i][1] = on ? 255 : 0;
        px[i][2] = on ? 255 : 0;
        px[i][3] = on ? 255 : 0;
    }
    kc_bc3_encode(px, blk);
    kc_bc3_decode(blk, back);
    int exact = 1;
    for (int i = 0; i < 16; i++)
        if (px[i][3] != back[i][3] || (px[i][3] && memcmp(px[i], back[i], 3))) exact = 0;
    CHECK(exact, "BC3 round trip of a black/white block is exact (colour of transparent texels is free)");
    /* a grey ramp stays within 12 levels */
    int worst = 0;
    for (int i = 0; i < 16; i++) {
        px[i][0] = px[i][1] = px[i][2] = (uint8_t)(20 + i * 8);
        px[i][3] = (uint8_t)(255 - i * 9);
    }
    kc_bc3_encode(px, blk);
    kc_bc3_decode(blk, back);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) {
            int d = abs(px[i][k] - back[i][k]);
            if (d > worst) worst = d;
        }
    CHECK(worst <= 22, "BC3 grey ramp error %d <= 22 (4 colour levels over a 120-level ramp)", worst);

    /* 2. fingerprints: random 1024x512 and 512x128 textures are no atlas */
    const uint32_t W = 1024, H = 512, N = W * H;
    uint8_t* tex = (uint8_t*)malloc(N);
    for (uint32_t i = 0; i < N; i++) tex[i] = (uint8_t)rnd();
    CHECK(rsx_keycaps_kind(tex, W, H, N) == KC_NONE, "a random 1024x512 DXT5 is not the HUD atlas");
    CHECK(rsx_keycaps_kind(tex, 512, 128, 65536) == KC_NONE, "a random 512x128 DXT5 is not the menu atlas");
    CHECK(rsx_keycaps_kind(tex, 256, 256, 65536) == KC_NONE, "other sizes are never hashed");

    /* 3. pad mode: no copy */
    int cells = -1;
    s_kb = 0;
    CHECK(rsx_keycaps_sig() == 1, "pad mode signature is 1");
    CHECK(rsx_keycaps_patch(tex, W, H, N, KC_HUD, &cells) == NULL && cells == 0, "pad mode leaves the atlas alone");

    /* 4. keyboard mode: only blocks under cells change; a face cell is a dark opaque keycap with a light face */
    s_kb = 1;
    const uint32_t sig1 = rsx_keycaps_sig();
    CHECK(sig1 >= 2, "keyboard signature %08X >= 2", sig1);
    uint8_t* p = rsx_keycaps_patch(tex, W, H, N, KC_HUD, &cells);
    CHECK(p != NULL && cells == 18, "keyboard mode patches 18 HUD cells (%d)", cells);
    int outside_changed = 0, inside_changed = 0;
    for (uint32_t by = 0; by < H / 4; by++)
        for (uint32_t bx = 0; bx < W / 4; bx++) {
            int under = 0;
            for (unsigned c = 0; c < sizeof kc_hud_cells / sizeof kc_hud_cells[0]; c++) {
                const kc_cell* k = &kc_hud_cells[c];
                if (bx * 4 + 3 >= k->l && bx * 4 <= k->r && by * 4 + 3 >= k->t && by * 4 <= k->b) under = 1;
            }
            const int diff =
                memcmp(p + ((size_t)by * (W / 4) + bx) * 16, tex + ((size_t)by * (W / 4) + bx) * 16, 16) != 0;
            if (diff && !under) outside_changed++;
            if (diff && under) inside_changed++;
        }
    CHECK(outside_changed == 0, "no block outside the cells changed (%d)", outside_changed);
    CHECK(inside_changed > 300, "blocks under the cells changed (%d)", inside_changed);
/* Square cell (0,248)-(37,285): centre opaque, corner transparent, face pixel light, body pixel dark */
#define PX(x, y)                                                                                                       \
    (kc_bc3_decode(p + ((size_t)((y) / 4) * (W / 4) + (x) / 4) * 16, back), back[((y) & 3) * 4 + ((x) & 3)])
    const uint8_t* q;
    q = PX(0, 248);
    CHECK(q[3] < 40, "keycap corner is transparent (a=%d)", q[3]);
    q = PX(19, 266);
    CHECK(q[3] > 200, "keycap centre is opaque (a=%d)", q[3]);
    int light = 0, dark = 0;
    for (int y = 254; y < 280; y++)
        for (int x = 6; x < 32; x++) {
            q = PX(x, y);
            if (q[0] > 180)
                light++;
            else if (q[0] < 60)
                dark++;
        }
    CHECK(light > 20 && dark > 100, "keycap has a light face on a dark body (light %d, dark %d)", light, dark);
    free(p);

    /* 5. labels and signature */
    char lab[24];
    kc_label(0x20, lab, sizeof lab);
    CHECK(!strcmp(lab, "SPACE"), "Space -> SPACE (%s)", lab);
    kc_label(0xA0, lab, sizeof lab);
    CHECK(!strcmp(lab, "SHIFT"), "LeftShift -> SHIFT (%s)", lab);
    kc_label(0x08, lab, sizeof lab);
    CHECK(!strcmp(lab, "BKSP"), "Backspace -> BKSP (%s)", lab);
    kc_stick_label("ls", lab, sizeof lab);
    CHECK(!strcmp(lab, "WASD"), "left stick -> WASD (%s)", lab);
    CHECK(kc_cell_key("cross", 1) == 0x0D && kc_cell_key("cross", 0) == 0x20,
          "menu cross shows Enter, HUD cross Space");
    CHECK(kc_cell_key("start", 1) == 0x1B, "menu start shows Escape");
    /* patch 2106: menu Back (Circle in the menu atlas) shows the back input's key; HUD Circle keeps E */
    CHECK(kc_cell_key("circle", 1) == 0x1B && kc_cell_key("circle", 0) == 0x45, "menu circle shows Esc (back), HUD circle E");
    s_back_off = 1;
    CHECK(kc_cell_key("circle", 1) == 0x45, "no menu tracking: menu circle shows circle's own key (E)");
    s_back_off = 0;
    s_keys_changed = 1;
    CHECK(rsx_keycaps_sig() != sig1, "a binding change changes the signature");
    s_keys_changed = 0;
    CHECK(rsx_keycaps_sig() == sig1, "the same bindings give the same signature");

    /* 6. menu atlas: 18 cells, the text rasteriser was used */
    uint8_t* mt = (uint8_t*)malloc(65536);
    for (int i = 0; i < 65536; i++) mt[i] = (uint8_t)rnd();
    s_text_calls = 0;
    p = rsx_keycaps_patch(mt, 512, 128, 65536, KC_MENU, &cells);
    CHECK(p && cells == 18 && s_text_calls > 8, "menu atlas: %d cells, %d text labels", cells, s_text_calls);
    free(p);
    free(mt);

    /* 8. (RSX pass 57) device flips: the guest bytes are never written; keyboard mode changes no texel outside the
     * prompt cells (blocks shared with other art keep their endpoints and those texels' indices); pad mode uploads
     * the original bytes; every keyboard upload is the same. Random blocks cover both colour endpoint orders. */
    for (int kind = KC_HUD; kind <= KC_MENU; kind++) {
        rsx_keycaps_set_source(&SRC);
        const uint32_t aw = kind == KC_HUD ? 1024 : 512, ah = kind == KC_HUD ? 512 : 128, an = aw * ah;
        const kc_cell* cl = kind == KC_HUD ? kc_hud_cells : kc_menu_cells;
        const unsigned ncl = kind == KC_HUD ? sizeof kc_hud_cells / sizeof kc_hud_cells[0]
                                            : sizeof kc_menu_cells / sizeof kc_menu_cells[0];
        uint8_t* g = (uint8_t*)malloc(an);
        uint8_t* g0 = (uint8_t*)malloc(an);
        uint8_t* first = NULL;
        for (uint32_t i = 0; i < an; i++) g[i] = (uint8_t)rnd();
        memcpy(g0, g, an);
        int guest_written = 0, pad_copy = 0, outside_texels = 0, inside_texels = 0, differs = 0, partial = 0;
        for (int flip = 0; flip < 8; flip++) {
            s_kb = flip & 1;
            int nc = -1;
            uint8_t* up = rsx_keycaps_patch(g, aw, ah, an, kind, &nc);
            if (memcmp(g, g0, an)) guest_written++;
            if (!s_kb) { if (up) pad_copy++; free(up); continue; }
            if (!up) { differs++; continue; }
            if (!first) {
                first = up;
                for (uint32_t by = 0; by < ah / 4; by++)
                    for (uint32_t bx = 0; bx < aw / 4; bx++) {
                        uint8_t o[16][4], n[16][4];
                        kc_bc3_decode(g0 + ((size_t)by * (aw / 4) + bx) * 16, o);
                        kc_bc3_decode(up + ((size_t)by * (aw / 4) + bx) * 16, n);
                        int n_in = 0;
                        for (int i = 0; i < 16; i++) {
                            const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                            int in = 0;
                            for (unsigned c = 0; c < ncl; c++)
                                if (x >= cl[c].l && x <= cl[c].r && y >= cl[c].t && y <= cl[c].b) in = 1;
                            n_in += in;
                            if (!in && memcmp(o[i], n[i], 4)) outside_texels++;
                            if (in && memcmp(o[i], n[i], 4)) inside_texels++;
                        }
                        if (n_in && n_in < 16) partial++;
                    }
            } else {
                if (memcmp(up, first, an)) differs++;
                free(up);
            }
        }
        const char* nm = kind == KC_HUD ? "HUD" : "menu";
        CHECK(guest_written == 0, "%s flips: guest bytes never written (%d)", nm, guest_written);
        CHECK(pad_copy == 0, "%s flips: pad mode uploads the original bytes (%d copies)", nm, pad_copy);
        CHECK(differs == 0, "%s flips: every keyboard upload is the same (%d differ)", nm, differs);
        CHECK(outside_texels == 0, "%s: no texel outside the prompt cells changed (%d; %d shared blocks)", nm,
              outside_texels, partial);
        CHECK(inside_texels > 1000, "%s: texels inside the cells changed (%d)", nm, inside_texels);
        free(first); free(g); free(g0);
    }

    /* 7. no source: never patched */
    rsx_keycaps_set_source(NULL);
    CHECK(rsx_keycaps_sig() == 0 && rsx_keycaps_patch(tex, W, H, N, KC_HUD, &cells) == NULL, "no source: off");
    free(tex);
    printf("%s\n", fails ? "[test_keycaps] FAILED" : "[test_keycaps] all passed");
    return fails != 0;
}
