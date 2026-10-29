/* Host test for KZ3 patch 2037 (libs/input/pad_keybinds.h in the patched toolkit): the keybinds.ini parser, the
 * default table, the writer (round trip), the key evaluation into pad bits / stick axes, and the portable flag of
 * kz3-install.ini (same rule as tools/release/launch.py read_ini). Patch 2038: the Keyboard page. Patch 2061: key
 * positions against a fake French AZERTY layout table (no Windows needed): polling, capture, display legends and
 * the one-time migration of pre-2061 files. Platform-free; run by test_pad_keybinds.sh. */
#include "pad_keybinds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, logs = 0;
static char last[256];

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) {                                                                                                       \
            printf("[PASS] ");                                                                                         \
        } else {                                                                                                       \
            printf("[FAIL] ");                                                                                         \
            fails++;                                                                                                   \
        }                                                                                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static void logger(void* ctx, const char* m)
{
    (void)ctx;
    logs++;
    snprintf(last, sizeof last, "%s", m);
}

static int idx(const char* in)
{
    return pad_kb_input_by_name(in, strlen(in));
}

static const unsigned char* held_set;
static int held(void* ctx, int vk)
{
    const unsigned char* p;
    (void)ctx;
    for (p = held_set; *p; p++)
        if (*p == vk) return 1;
    return 0;
}

/* patch 2061: a fake layout. French AZERTY (KBDFR): scancode -> virtual key and the key's legend. */
typedef struct { int sc, vk; const char* lab; } FakeKey;
static const FakeKey azerty[] = {
    { 0x02, '1', "&" }, { 0x03, '2', "\xC3\x89" }, { 0x04, '3', "\"" }, { 0x05, '4', "'" }, { 0x06, '5', "(" },
    { 0x07, '6', "-" }, { 0x08, '7', "\xC3\x88" }, { 0x09, '8', "_" }, { 0x0A, '9', "\xC3\x87" }, { 0x0B, '0', "\xC3\x80" },
    { 0x0C, 0xDB, ")" }, { 0x0D, 0xBB, "=" },
    { 0x10, 'A', "A" }, { 0x11, 'Z', "Z" }, { 0x12, 'E', "E" }, { 0x13, 'R', "R" }, { 0x14, 'T', "T" }, { 0x15, 'Y', "Y" },
    { 0x16, 'U', "U" }, { 0x17, 'I', "I" }, { 0x18, 'O', "O" }, { 0x19, 'P', "P" }, { 0x1A, 0xDD, "^" }, { 0x1B, 0xBA, "$" },
    { 0x1E, 'Q', "Q" }, { 0x1F, 'S', "S" }, { 0x20, 'D', "D" }, { 0x21, 'F', "F" }, { 0x22, 'G', "G" }, { 0x23, 'H', "H" },
    { 0x24, 'J', "J" }, { 0x25, 'K', "K" }, { 0x26, 'L', "L" }, { 0x27, 'M', "M" }, { 0x28, 0xC0, "\xC3\x99" },
    { 0x29, 0xDE, "\xC2\xB2" }, { 0x2B, 0xDC, "*" },
    { 0x2C, 'W', "W" }, { 0x2D, 'X', "X" }, { 0x2E, 'C', "C" }, { 0x2F, 'V', "V" }, { 0x30, 'B', "B" }, { 0x31, 'N', "N" },
    { 0x32, 0xBC, "," }, { 0x33, 0xBE, ";" }, { 0x34, 0xBF, ":" }, { 0x35, 0xDF, "!" }, { 0x56, 0xE2, "<" },
};
static const FakeKey* fake_find(int sc)
{
    unsigned i;
    for (i = 0; i < sizeof azerty / sizeof azerty[0]; i++) if (azerty[i].sc == sc) return &azerty[i];
    return NULL;
}
static int fake_vk(void* ctx, int sc) { const FakeKey* k = fake_find(sc); (void)ctx; return k ? k->vk : 0; }
static int fake_name(void* ctx, int sc, char* out, size_t n)
{
    const FakeKey* k = fake_find(sc);
    (void)ctx;
    if (!k) return 0;
    snprintf(out, n, "%s", k->lab);
    return (int)strlen(out);
}
/* US: the position codes are the US virtual keys, the legends the US characters */
static int us_vk(void* ctx, int sc)
{
    int i;
    (void)ctx;
    for (i = 0; i < PKB_N_POS; i++) if (pad_kb_pos_keys[i].sc == sc) return pad_kb_pos_keys[i].code;
    return 0;
}
/* the poller's down(): position code -> the layout's virtual key -> held? (held_set holds virtual keys) */
static int held_layout(void* ctx, int code)
{
    const PadKbLayout* L = (const PadKbLayout*)ctx;
    int vk = L->vk[code & 255];
    return vk && held(NULL, vk);
}

int main(void)
{
    PadKeybinds d, k;
    char buf[8192];
    int n, bad;

    /* 1. defaults written and read back give the same table, with no problem lines */
    pad_kb_defaults(&d);
    n = pad_kb_format(&d, buf, sizeof buf);
    CHECK(n > 0 && n < (int)sizeof buf, "format fits (%d bytes)", n);
    memset(&k, 0, sizeof k);
    for (n = 0; n < PRM_N; n++) k.padc[n] = (unsigned char)n;   /* 4013: the parser only writes the [pad] lines that differ */
    pac_defaults(&k.pac);   /* 4023: likewise the [pad actions] lines (the caller starts from pad_kb_defaults) */
    logs = 0;
    bad = pad_kb_parse(&k, buf, logger, NULL);
    CHECK(bad == 0 && logs == 0 && memcmp(&k, &d, sizeof k) == 0, "round trip of the defaults (bad %d, logs %d)", bad,
          logs);
    CHECK(d.key[idx("start")][0] == 0x1B && d.key[idx("ls_up")][0] == 'W' && d.key[idx("r1")][0] == 0x01,
          "defaults: Escape = start, W = ls_up, Mouse1 = r1");
    {
        int i, s, unbound_primary = 0;
        for (i = 0; i < PKB_N_INPUTS; i++)
            if (!d.key[i][0]) unbound_primary++;
        for (i = 0; i < PKB_N_INPUTS; i++)
            for (s = 0; s < PKB_SLOTS; s++)
                if (d.key[i][s] && !strcmp(pad_kb_key_name(d.key[i][s]), "none")) unbound_primary += 100;
        CHECK(unbound_primary == 0, "every input has a named primary key by default");
    }

    /* 2. edits, mistakes and other sections */
    k = d;
    logs = 0;
    bad = pad_kb_parse(&k,
                       "\xEF\xBB\xBF# comment\r\n"
                       "  CROSS = z , x   # inline comment\r\n"
                       "r1 = Mouse1\n"
                       "l2 = none\n"
                       "bogus = A\n"
                       "circle = Foo\n"
                       "square = R, T, Y\n"
                       "triangle Q\n"
                       "select = Esc ; other spelling\n"
                       "start = Return, Nope\n"
                       "[gamepad]\n"
                       "up = A\n"
                       "[keyboard]\n"
                       "rs_left = numpad4\n",
                       logger, NULL);
    CHECK(k.key[idx("cross")][0] == 'Z' && k.key[idx("cross")][1] == 'X', "case, blanks, CRLF, BOM, inline comment");
    CHECK(k.key[idx("r1")][0] == 0x01 && k.key[idx("r1")][1] == 0, "one key clears the secondary");
    CHECK(k.key[idx("l2")][0] == 0 && k.key[idx("l2")][1] == 0, "none unbinds");
    CHECK(k.key[idx("circle")][0] == d.key[idx("circle")][0] && k.key[idx("circle")][1] == d.key[idx("circle")][1],
          "a line with only unknown keys keeps the default");
    CHECK(k.key[idx("square")][0] == 'R' && k.key[idx("square")][1] == 'T', "a third key is dropped");
    CHECK(k.key[idx("select")][0] == 0x1B, "Esc spelling");
    CHECK(k.key[idx("start")][0] == 0x0D && k.key[idx("start")][1] == 0,
          "a bad key next to a good one: the good one kept");
    CHECK(k.key[idx("up")][0] == d.key[idx("up")][0], "other sections are skipped");
    CHECK(k.key[idx("rs_left")][0] == 0x64, "back in [keyboard] after another section");
    CHECK(bad == 5,
          "problems counted: bogus input, Foo, Y, no =, Nope; the section note is not one (bad %d, logs %d, last '%s')",
          bad, logs, last);
    CHECK(pad_kb_parse(&k, "", logger, NULL) == 0 && pad_kb_parse(&k, NULL, logger, NULL) == 0, "empty and NULL text");
    {
        static char big[70000];
        memset(big, 'x', sizeof big - 1);
        CHECK(pad_kb_parse(&k, big, logger, NULL) == 1, "one 70 kB garbage line = one problem, no overflow");
    }

    /* 3. key evaluation: buttons, stick directions, diagonals, opposite keys */
    {
        static const unsigned char keys1[] = {'W', 'D', 0x20, 0x01, 0};
        static const unsigned char keys2[] = {'A', 'D', 'J', 0x0D, 0};
        PadKbState st;
        held_set = keys1;
        st = pad_kb_eval(&d, held, NULL);
        CHECK(st.buttons == (PKB_CROSS | PKB_R1), "Space + Mouse1 = cross + r1 (0x%04X)", st.buttons);
        CHECK(pad_kb_axis(st.dirs, PKB_LS_LEFT, PKB_LS_RIGHT) == 255 &&
                  pad_kb_axis(st.dirs, PKB_LS_UP, PKB_LS_DOWN) == 0,
              "W + D = left stick up-right (diagonal)");
        CHECK(pad_kb_axis(st.dirs, PKB_RS_LEFT, PKB_RS_RIGHT) == 128, "right stick centred");
        held_set = keys2;
        st = pad_kb_eval(&d, held, NULL);
        CHECK(pad_kb_axis(st.dirs, PKB_LS_LEFT, PKB_LS_RIGHT) == 128, "A + D cancel");
        CHECK(pad_kb_axis(st.dirs, PKB_RS_LEFT, PKB_RS_RIGHT) == 0, "J = right stick left");
        CHECK(st.buttons == PKB_CROSS, "Enter = cross (secondary key)");
    }

    /* 4. kz3-install.ini portable flag (launch.py: configparser, [install] portable == "1") */
    CHECK(pad_kb_ini_portable("[install]\nportable = 1\n") == 1, "portable = 1");
    CHECK(pad_kb_ini_portable("[install]\r\nPortable=1\r\n") == 1, "key case-insensitive, CRLF");
    CHECK(pad_kb_ini_portable("[install]\nportable = 0\n") == 0, "portable = 0");
    CHECK(pad_kb_ini_portable("[install]\nportable = yes\n") == 0, "only \"1\" counts");
    CHECK(pad_kb_ini_portable("[Install]\nportable = 1\n") == 0, "section name case-sensitive like configparser");
    CHECK(pad_kb_ini_portable("[online]\nportable = 1\n") == 0, "other section");
    CHECK(pad_kb_ini_portable("") == 0 && pad_kb_ini_portable(NULL) == 0, "empty / missing");

    /* 5. the Keyboard page (patch 2038): commands and the capture of the next key */
    {
        PadKbUi u;
        PadKeybinds kb;
        char out[256];
        int ch, r, row_fire = -1, row_jump = -1;
        static const unsigned char none[] = { 0 }, enter[] = { 0x0D, 0 }, kk[] = { 'K', 0 }, del[] = { 0x2E, 0 },
                                   esc[] = { 0x1B, 0 };
        memset(&u, 0, sizeof u);
        pad_kb_defaults(&kb);
        for (r = 0; r < PKB_N_ROWS; r++) {
            CHECK(pad_kb_row_input(r) >= 0, "row %d names an input (%s)", r, pad_kb_rows[r].input);
            if (!strcmp(pad_kb_rows[r].input, "r1")) row_fire = r;
            if (!strcmp(pad_kb_rows[r].input, "cross")) row_jump = r;
        }
        for (r = 0; r < PKB_N_KEYS; r++)
            CHECK(strlen(pad_kb_key_short(pad_kb_keys[r].vk)) <= 9, "display name of %s fits a column (%s)",
                  pad_kb_keys[r].name, pad_kb_key_short(pad_kb_keys[r].vk));
        CHECK(!strcmp(pad_kb_key_short(0), "---") && !strcmp(pad_kb_key_short(0x68), "Num 8"), "unbound / numpad names");
        for (r = 0; r < PKB_N_ROWS; r++)
            CHECK(strlen(pad_kb_rows[r].label) <= 26, "row label fits the label column (%s)", pad_kb_rows[r].label);
        CHECK(PKB_N_ROWS == PKB_N_INPUTS, "every input has a row (%d/%d)", PKB_N_ROWS, PKB_N_INPUTS);
        pad_kb_ui_command(&u, &kb, "rows", out, sizeof out, 0);
        CHECK(atoi(out) == PKB_N_ROWS, "rows -> %s", out);
        pad_kb_ui_command(&u, &kb, "key 0 0", out, sizeof out, 0);
        CHECK(!strcmp(out, "W"), "key 0 0 -> W (%s)", out);
        snprintf(buf, sizeof buf, "key %d 1", row_fire);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 0);
        CHECK(!strcmp(out, "O"), "fire secondary -> O (%s)", out);
        pad_kb_ui_command(&u, &kb, "key 99 0", out, sizeof out, 0);
        CHECK(!strcmp(out, "?"), "bad row -> ? (%s)", out);
        /* capture fire primary: Enter (the key that opened it) is held first and must not be taken */
        snprintf(buf, sizeof buf, "capture %d 0", row_fire);
        ch = pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 1000);
        CHECK(!ch && !strcmp(out, "ok") && u.phase == PKB_UI_RELEASE, "capture starts in the release phase");
        snprintf(buf, sizeof buf, "key %d 0", row_fire);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 1000);
        CHECK(!strcmp(out, "..."), "the captured slot shows ... (%s)", out);
        held_set = enter;
        CHECK(!pad_kb_ui_step(&u, &kb, held, NULL, 1010) && u.phase == PKB_UI_RELEASE, "held Enter is not taken");
        CHECK(pad_kb_ui_busy(&u), "the game gets no keys while capturing");
        held_set = none;
        pad_kb_ui_step(&u, &kb, held, NULL, 1020);
        CHECK(u.phase == PKB_UI_WAIT, "all keys up -> waiting");
        held_set = kk;
        ch = pad_kb_ui_step(&u, &kb, held, NULL, 1030);
        CHECK(ch && kb.key[idx("r1")][0] == 'K' && u.result == PKB_R_BOUND, "K bound to fire");
        CHECK(pad_kb_ui_busy(&u), "K stays away from the game until released");
        held_set = none;
        pad_kb_ui_step(&u, &kb, held, NULL, 1040);
        CHECK(!pad_kb_ui_busy(&u), "released -> keys go to the game again");
        pad_kb_ui_command(&u, &kb, "poll", out, sizeof out, 1050);
        CHECK(!strcmp(out, "changed"), "poll reports the change (%s)", out);
        pad_kb_ui_command(&u, &kb, "poll", out, sizeof out, 1060);
        CHECK(!strcmp(out, ""), "poll reports it once (%s)", out);
        /* K was rs_down's key: a conflict is shown */
        snprintf(buf, sizeof buf, "key %d 0", row_fire);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 0);
        CHECK(!strcmp(out, "K"), "key name without a mark (%s)", out);
        snprintf(buf, sizeof buf, "dup %d 0", row_fire);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 0);
        CHECK(!strcmp(out, "1"), "dup marks the conflict (%s)", out);
        pad_kb_ui_command(&u, &kb, "dup 0 0", out, sizeof out, 0);
        CHECK(!strcmp(out, ""), "no dup for W (%s)", out);
        pad_kb_ui_command(&u, &kb, "state", out, sizeof out, 0);
        CHECK(!strcmp(out, "conflict"), "state conflict (%s)", out);
        pad_kb_ui_command(&u, &kb, "cf key", out, sizeof out, 0);
        CHECK(!strcmp(out, "K"), "cf key (%s)", out);
        pad_kb_ui_command(&u, &kb, "cf a", out, sizeof out, 0);
        CHECK(pad_kb_rows[atoi(out)].input[0] == 'r', "cf a is a K row (%s)", out);
        pad_kb_ui_command(&u, &kb, "cf b", out, sizeof out, 0);
        CHECK(atoi(out) == row_fire, "cf b is the fire row (%s)", out);
        pad_kb_ui_command(&u, &kb, "status", out, sizeof out, 0);
        CHECK(strstr(out, "K is bound to both") != NULL, "status names the conflict (%s)", out);
        /* the input's other slot gets the same key -> moved there */
        snprintf(buf, sizeof buf, "capture %d 1", row_fire);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 2000);
        pad_kb_ui_step(&u, &kb, held, NULL, 2010);
        held_set = kk;
        pad_kb_ui_step(&u, &kb, held, NULL, 2020);
        CHECK(kb.key[idx("r1")][1] == 'K' && kb.key[idx("r1")][0] == 0, "same key in the other slot moves");
        held_set = none; pad_kb_ui_step(&u, &kb, held, NULL, 2030);
        /* Delete clears, Escape cancels, a timeout leaves it */
        snprintf(buf, sizeof buf, "capture %d 0", row_jump);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 3000);
        pad_kb_ui_step(&u, &kb, held, NULL, 3010);
        held_set = del;
        CHECK(pad_kb_ui_step(&u, &kb, held, NULL, 3020) && kb.key[idx("cross")][0] == 0 && u.result == PKB_R_CLEARED,
              "Delete clears");
        held_set = none; pad_kb_ui_step(&u, &kb, held, NULL, 3030);
        snprintf(buf, sizeof buf, "capture %d 1", row_jump);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 4000);
        pad_kb_ui_step(&u, &kb, held, NULL, 4010);
        held_set = esc;
        CHECK(!pad_kb_ui_step(&u, &kb, held, NULL, 4020) && kb.key[idx("cross")][1] == 0x0D && u.result == PKB_R_CANCEL,
              "Escape cancels");
        held_set = none; pad_kb_ui_step(&u, &kb, held, NULL, 4030);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 5000);
        pad_kb_ui_step(&u, &kb, held, NULL, 5010);
        pad_kb_ui_step(&u, &kb, held, NULL, 5000 + PKB_CAPTURE_MS + 1);
        CHECK(u.phase == PKB_UI_IDLE && u.result == PKB_R_TIMEOUT, "a capture times out");
        pad_kb_ui_command(&u, &kb, "status", out, sizeof out, 0);
        CHECK(strstr(out, "No key") != NULL, "status after a timeout (%s)", out);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 6000);
        pad_kb_ui_command(&u, &kb, "cancel", out, sizeof out, 6001);
        CHECK(u.phase == PKB_UI_IDLE && u.result == PKB_R_CANCEL, "cancel ends a capture (page closed)");
        ch = pad_kb_ui_command(&u, &kb, "reset", out, sizeof out, 7000);
        CHECK(ch && memcmp(&kb, &d, sizeof kb) == 0, "reset -> defaults, save");
        pad_kb_ui_command(&u, &kb, "status", out, sizeof out, 0);
        CHECK(!strcmp(out, "Enter change, Delete clear, Left / Right column, R reset, Esc back."),
              "defaults: the idle line names the bound keys (%s)", out);
        pad_kb_ui_command(&u, &kb, "state", out, sizeof out, 0);
        CHECK(!strcmp(out, "idle"), "state idle (%s)", out);
        /* pass 38: Delete on the page clears the selected cell, once per press, only while the page is open */
        held_set = del;
        CHECK(!pad_kb_ui_step(&u, &kb, held, NULL, 7100) && kb.key[idx("ls_up")][0] == 'W', "Delete without a selection");
        held_set = none; pad_kb_ui_step(&u, &kb, held, NULL, 7110);
        pad_kb_ui_command(&u, &kb, "sel 0 0", out, sizeof out, 7120);
        held_set = del;
        CHECK(pad_kb_ui_step(&u, &kb, held, NULL, 7130) && kb.key[idx("ls_up")][0] == 0, "Delete clears the selection");
        CHECK(pad_kb_ui_busy(&u), "that Delete stays away from the game");
        pad_kb_ui_command(&u, &kb, "sel 0 1", out, sizeof out, 7135);
        CHECK(!pad_kb_ui_step(&u, &kb, held, NULL, 7140), "held Delete clears once");
        held_set = none; pad_kb_ui_step(&u, &kb, held, NULL, 7150);
        pad_kb_ui_command(&u, &kb, "cancel", out, sizeof out, 7160);
        CHECK(u.sel == 0, "cancel (page off) ends the selection");
        pad_kb_defaults(&kb);
        for (r = 0; r < PKB_N_ROWS; r++) {
            pad_kb_ui_command(&u, &kb, (snprintf(buf, sizeof buf, "src %d", r), buf), out, sizeof out, 0);
            CHECK(!strcmp(out, pad_kb_rows[r].button ? pad_kb_rows[r].button : ""), "src %d (%s)", r, out);
        }
        pad_kb_ui_command(&u, &kb, "caphint", out, sizeof out, 0);
        CHECK(strstr(out, "Esc") && strstr(out, "Delete"), "capture hint names Esc and Delete (%s)", out);
        snprintf(buf, sizeof buf, "clear %d 1", row_fire);
        CHECK(pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 0) && kb.key[idx("r1")][1] == 0, "clear R S");
        pad_kb_ui_command(&u, &kb, "bogus", out, sizeof out, 0);
        CHECK(!strcmp(out, "unknown"), "unknown command");
        pad_kb_ui_command(&u, &kb, "", out, sizeof out, 0);
        CHECK(!strcmp(out, ""), "empty command");
    }

    /* 6. patch 2061: key positions on a French AZERTY layout */
    {
        static PadKbLayout az, us;
        PadKeybinds kb, old;
        PadKbState st;
        PadKbUi u;
        char out[256], cmd[64];
        int r, row_up = -1, row_fire = -1, moved;
        static const unsigned char vz[] = { 'Z', 0 }, vw[] = { 'W', 0 }, vq[] = { 'Q', 0 }, va[] = { 'A', 0 },
                                   none[] = { 0 }, vbang[] = { 0xDF, 0 };
        pad_kb_layout_build(&az, fake_vk, fake_name, NULL);
        pad_kb_layout_build(&us, us_vk, NULL, NULL);
        CHECK(az.vk[0x57] == 'Z' && az.vk[0x41] == 'Q' && az.vk[0x53] == 'S' && az.vk[0x44] == 'D',
              "AZERTY: positions W A S D poll Z Q S D (%02x %02x %02x %02x)", az.vk[0x57], az.vk[0x41], az.vk[0x53], az.vk[0x44]);
        CHECK(az.pos['Z'] == 0x57 && az.pos['W'] == 0x5A && az.pos['A'] == 0x51 && az.pos[0xDF] == 0xBF && az.pos[0xBA] == 0xDD,
              "AZERTY: virtual keys back to positions (Z->W, W->Z, A->Q, OEM_8->Slash, OEM_1->RightBracket)");
        CHECK(!strcmp(az.label[0x57], "Z") && !strcmp(az.label[0xBF], "!") && !strcmp(az.label[0x32], "\xC3\x89"),
              "AZERTY legends (W pos = Z, Slash pos = !, 2 pos = E acute)");
        CHECK(az.vk[0x26] == 0x26 && az.vk[0x01] == 0x01 && az.vk[0xA0] == 0xA0 && az.vk[0x68] == 0x68 && az.vk[0x20] == 0x20
              && az.pos[0x26] == 0x26 && az.label[0x26][0] == 0, "layout-independent keys map to themselves, no legend");
        for (r = 0; r < 256; r++) if (us.vk[r] != r || us.pos[r] != r) break;
        CHECK(r == 256, "US layout: identity (stopped at %d)", r);
        CHECK(pad_kb_pos_sc(0x57) == 0x11 && pad_kb_pos_sc(0xE2) == 0x56 && pad_kb_pos_sc(0x26) == 0, "position scancodes");

        pad_kb_defaults(&kb);
        held_set = vz; st = pad_kb_eval(&kb, held_layout, &az);
        CHECK(st.dirs == (1u << PKB_LS_UP) && !st.buttons, "AZERTY: the Z key moves forward (default W position)");
        held_set = vq; st = pad_kb_eval(&kb, held_layout, &az);
        CHECK(st.dirs == (1u << PKB_LS_LEFT) && !st.buttons, "AZERTY: the Q key strafes left (default A position)");
        held_set = vw; st = pad_kb_eval(&kb, held_layout, &az);
        CHECK(!st.dirs && !st.buttons, "AZERTY: the W key (US Z position) is unbound");
        held_set = vz; st = pad_kb_eval(&kb, held_layout, &us);
        CHECK(!st.dirs && !st.buttons, "US: Z does not move");

        /* the page: legends of the active layout, capture stores the position */
        pad_kb_display_layout = &az;
        memset(&u, 0, sizeof u);
        for (r = 0; r < PKB_N_ROWS; r++) {
            if (!strcmp(pad_kb_rows[r].input, "ls_up")) row_up = r;
            if (!strcmp(pad_kb_rows[r].input, "r1")) row_fire = r;
        }
        CHECK(row_up >= 0 && row_fire >= 0, "rows ls_up %d, r1 %d", row_up, row_fire);
        snprintf(cmd, sizeof cmd, "key %d 0", row_up);
        pad_kb_ui_command(&u, &kb, cmd, out, sizeof out, 0);
        CHECK(!strcmp(out, "Z"), "AZERTY page shows Z for move forward (%s)", out);
        snprintf(cmd, sizeof cmd, "capture %d 1", row_fire);
        pad_kb_ui_command(&u, &kb, cmd, out, sizeof out, 0);
        held_set = none; pad_kb_ui_step(&u, &kb, held_layout, &az, 10);
        held_set = va; CHECK(pad_kb_ui_step(&u, &kb, held_layout, &az, 20) == 1, "capture ends on the A key");
        CHECK(kb.key[idx("r1")][1] == 0x51, "AZERTY A key stored as position Q (%02x)", kb.key[idx("r1")][1]);
        snprintf(cmd, sizeof cmd, "key %d 1", row_fire);
        pad_kb_ui_command(&u, &kb, cmd, out, sizeof out, 0);
        CHECK(!strcmp(out, "A"), "page shows the captured key as A (%s)", out);
        held_set = none; pad_kb_ui_step(&u, &kb, held_layout, &az, 30);
        snprintf(cmd, sizeof cmd, "capture %d 1", row_fire);
        pad_kb_ui_command(&u, &kb, cmd, out, sizeof out, 40);
        pad_kb_ui_step(&u, &kb, held_layout, &az, 50);
        held_set = vbang; pad_kb_ui_step(&u, &kb, held_layout, &az, 60);
        CHECK(kb.key[idx("r1")][1] == 0xBF, "AZERTY ! key (VK_OEM_8, no US key) stored as position Slash (%02x)", kb.key[idx("r1")][1]);
        pad_kb_display_layout = NULL;
        CHECK(!strcmp(pad_kb_key_short(0x57), "W"), "no layout: the port's own names");

        /* the file: marker, round trip, migration */
        pad_kb_defaults(&kb);
        n = pad_kb_format(&kb, buf, sizeof buf);
        CHECK(pad_kb_text_positional(buf), "written files carry keys = positions");
        logs = 0; memset(&old, 0, sizeof old);
        CHECK(pad_kb_parse(&old, buf, logger, NULL) == 0 && logs == 0, "the marker line is no problem line (%s)", logs ? last : "");
        CHECK(!pad_kb_text_positional("[keyboard]\nls_up = W\n") && !pad_kb_text_positional("# keys = positions\n")
              && pad_kb_text_positional("\xEF\xBB\xBF[keyboard]\r\n  keys=positions\r\n") == 1, "marker detection");
        /* an old file on AZERTY: the player put forward on Z and left on Q (their WASD), fire on ! ; jump default */
        pad_kb_defaults(&old);
        pad_kb_parse(&old, "[keyboard]\nls_up = Z\nls_left = Q\nls_down = S\nls_right = D\nr1 = Mouse1, Slash\n", NULL, NULL);
        old.key[idx("r1")][1] = 0xDF;                       /* VK_OEM_8: a file could not name it, the page could not */
        moved = pad_kb_migrate(&old, &az);
        CHECK(old.key[idx("ls_up")][0] == 0x57 && old.key[idx("ls_left")][0] == 0x41 && old.key[idx("ls_down")][0] == 0x53
              && old.key[idx("ls_right")][0] == 0x44, "migration: AZERTY ZQSD become positions W A S D");
        CHECK(old.key[idx("r1")][0] == 0x01 && moved == 2, "migration: mouse unchanged, %d slot(s) moved", moved);
        pad_kb_defaults(&old);                              /* an old untouched default file */
        moved = pad_kb_migrate(&old, &az);
        pad_kb_defaults(&kb);
        CHECK(moved == 0 && memcmp(&old, &kb, sizeof kb) == 0, "migration: inputs on the old defaults keep the default positions");
        pad_kb_defaults(&old);
        pad_kb_parse(&old, "[keyboard]\nl1 = Semicolon\nl2 = Z\n", NULL, NULL);
        moved = pad_kb_migrate(&old, &us);
        CHECK(moved == 0 && old.key[idx("l1")][0] == 0xBA && old.key[idx("l2")][0] == 0x5A, "migration on US: nothing moves");
    }

    /* 2106. menu Back: Esc goes back in a menu and pauses in play; Backspace is menu-only now */
    {
        static const unsigned char esc[] = {0x1B, 0}, bksp[] = {0x08, 0}, pk[] = {'P', 0}, e[] = {'E', 0};
        PadKeybinds kb;
        PadKbUi u;
        PadKbState st;
        char out[256];
        int r, row_back = -1, row_start = -1;
        pad_kb_defaults(&kb);
        CHECK(kb.key[idx("back")][0] == 0x1B && kb.key[idx("back")][1] == 0x08 && kb.key[idx("circle")][0] == 'E' &&
              kb.key[idx("circle")][1] == 0, "2106 defaults: back = Escape, Backspace; circle = E");
        held_set = esc; st = pad_kb_eval_menu(&kb, held, NULL, 0);
        CHECK(st.buttons == PKB_START, "2106: Esc in play = start (0x%04X)", st.buttons);
        st = pad_kb_eval_menu(&kb, held, NULL, 1);
        CHECK(st.buttons == PKB_CIRCLE, "2106: Esc in a menu = circle only (0x%04X)", st.buttons);
        held_set = bksp; st = pad_kb_eval(&kb, held, NULL);
        CHECK(st.buttons == 0, "2106: Backspace in play = nothing (0x%04X)", st.buttons);
        st = pad_kb_eval_menu(&kb, held, NULL, 1);
        CHECK(st.buttons == PKB_CIRCLE, "2106: Backspace in a menu = circle (0x%04X)", st.buttons);
        held_set = pk; st = pad_kb_eval_menu(&kb, held, NULL, 1);
        CHECK(st.buttons == PKB_START, "2106: P in a menu = start (0x%04X)", st.buttons);
        held_set = e; st = pad_kb_eval_menu(&kb, held, NULL, 1);
        CHECK(st.buttons == PKB_CIRCLE, "2106: E in a menu = circle (0x%04X)", st.buttons);
        memset(&u, 0, sizeof u);
        for (r = 0; r < PKB_N_ROWS; r++) {
            if (!strcmp(pad_kb_rows[r].input, "back")) row_back = r;
            if (!strcmp(pad_kb_rows[r].input, "start")) row_start = r;
        }
        CHECK(row_back >= 0 && row_start >= 0, "2106: the page has a Back (menus) row (%d)", row_back);
        pad_kb_ui_command(&u, &kb, "state", out, sizeof out, 0);
        CHECK(!strcmp(out, "idle"), "2106: Esc on back and start is no conflict (state %s)", out);
        snprintf(buf, sizeof buf, "dup %d 0", row_back);
        pad_kb_ui_command(&u, &kb, buf, out, sizeof out, 0);
        CHECK(!strcmp(out, ""), "2106: back's Esc is not marked as a duplicate (%s)", out);
        pad_kb_parse(&kb, "circle = E, Backspace\n", NULL, NULL);   /* a pre-2106 file: no conflict either */
        pad_kb_ui_command(&u, &kb, "state", out, sizeof out, 0);
        CHECK(!strcmp(out, "idle"), "2106: an old file's circle Backspace is no conflict (state %s)", out);
        pad_kb_parse(&kb, "back = Space\n", NULL, NULL);             /* Space = cross: a real conflict */
        pad_kb_ui_command(&u, &kb, "state", out, sizeof out, 0);
        CHECK(!strcmp(out, "conflict"), "2106: back on Space (cross) is a conflict (state %s)", out);
        pad_kb_defaults(&kb);
        pad_kb_ui_command(&u, &kb, "hint", out, sizeof out, 0);
        CHECK(strstr(out, "Esc back") != NULL, "2106: the hint names Esc back (%s)", out);
    }

    printf(fails ? "[test_pad_keybinds] FAIL (%d)\n" : "[test_pad_keybinds] PASS\n", fails);
    return fails ? 1 : 0;
}
