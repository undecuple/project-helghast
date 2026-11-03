/*
 * KZ3 port, network pass 24 (stage 2): the text override table on the game's Localizer lookup.
 *
 * Source: port/text/d83_overrides.tsv (RE pass 140; columns language, key, orig_fnv1a32, replacement, kind,
 * note). A row replaces the text the lookup returned for `key` when the FNV-1a 32 of that text's UTF-8 equals
 * orig_fnv1a32: kind a = the controller named device-neutrally ("Controller", not "Wireless Controller"), kind b =
 * "Use" instead of "Push/Press" on the stick move/look hints. The language column is not needed for the match: the
 * hash of the original tells the languages (and the lumps that share a key with different texts) apart; the parser
 * drops a key + hash that maps to two different replacements (none in the round-140 table). The table carries no
 * original text. Replacements keep the game's literal "\n" pairs and [ICON ...] tokens byte for byte.
 *
 * Motion tip: loading_text_30 ("keep the controller level", a SIXAXIS tip) is shown reworded when the device in use
 * has motion sensors; without them (keyboard, XInput, a pad whose sensors SDL does not report) the lookup answers
 * with another tip's text (KZ3_TX_MOTION_ALT_KEY, a story tip). ASSUMPTION: the four motion tutorials
 * (tutorial_dcharge_rotate, tutorial_valve*) are only shown by the game when it sees a SIXAXIS, so they are only
 * reworded (the RE notes pass 140).
 *
 * Pure functions, no game or runtime symbols: tools/net/test_text_overrides.c tests them on the host.
 */
#ifndef KZ3_TEXT_OVERRIDES_H
#define KZ3_TEXT_OVERRIDES_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KZ3_TX_KEY_MAX 63          /* bytes of a key */
#define KZ3_TX_TEXT_MAX 1024       /* UTF-16 units of a replacement or of an original that is hashed */
#define KZ3_TX_MOTION_KEY "loading_text_30"
#define KZ3_TX_MOTION_ALT_KEY "loading_text_31"

enum { KZ3_TX_NONE = 0, KZ3_TX_REPLACE = 1, KZ3_TX_OTHER_KEY = 2 };

typedef struct {
    char     key[KZ3_TX_KEY_MAX + 1];
    uint8_t  klen;
    char     kind;                 /* 'a' or 'b' */
    uint32_t fnv;                  /* of the original's UTF-8 */
    uint32_t off, n;               /* replacement: units[off .. off+n) */
} Kz3TxRow;

typedef struct {
    Kz3TxRow* row;
    uint32_t  nrows;
    uint16_t* units;               /* UTF-16 pool of every replacement */
    uint32_t  nunits;
    int       rejected;            /* lines the parser refused */
    int       conflicts;           /* key + hash with two replacements (both dropped) */
} Kz3TxTable;

static inline uint32_t kz3_tx_fnv1a32(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* Strict UTF-8 -> UTF-16 (no overlongs, no surrogates, <= U+10FFFF). Returns the units written, -1 on bad input or
 * when `cap` is too small. */
static inline long kz3_tx_utf8_to_utf16(const char* s, size_t n, uint16_t* out, size_t cap)
{
    const uint8_t* p = (const uint8_t*)s;
    size_t i = 0, o = 0;
    while (i < n) {
        uint32_t c = p[i], need, min;
        if (c < 0x80) { need = 0; min = 0; }
        else if ((c & 0xE0) == 0xC0) { need = 1; c &= 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; c &= 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; c &= 0x07; min = 0x10000; }
        else return -1;
        if (i + need >= n) return -1;   /* truncated sequence */
        for (uint32_t k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return -1;
            c = (c << 6) | (p[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
        i += need + 1;
        if (c >= 0x10000) {
            if (o + 2 > cap) return -1;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (c >> 10));
            out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (o + 1 > cap) return -1;
            out[o++] = (uint16_t)c;
        }
    }
    return (long)o;
}

/* UTF-16 -> UTF-8 (an unpaired surrogate becomes U+FFFD, as a decoder of the lump would show it). Returns the bytes
 * written, -1 when `cap` is too small. */
static inline long kz3_tx_utf16_to_utf8(const uint16_t* u, size_t n, char* out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        uint8_t b[4];
        size_t k;
        if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
        else if (c < 0x800) { b[0] = (uint8_t)(0xC0 | (c >> 6)); b[1] = (uint8_t)(0x80 | (c & 0x3F)); k = 2; }
        else if (c < 0x10000) { b[0] = (uint8_t)(0xE0 | (c >> 12)); b[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
                                b[2] = (uint8_t)(0x80 | (c & 0x3F)); k = 3; }
        else { b[0] = (uint8_t)(0xF0 | (c >> 18)); b[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
               b[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (c & 0x3F)); k = 4; }
        if (o + k > cap) return -1;
        memcpy(out + o, b, k);
        o += k;
    }
    return (long)o;
}

static inline int kz3_tx_hex32(const char* s, size_t n, uint32_t* v)
{
    uint32_t x = 0;
    if (n != 8) return 0;
    for (size_t i = 0; i < 8; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        x = x << 4 | (uint32_t)d;
    }
    *v = x;
    return 1;
}

static inline int kz3_tx_key_ok(const char* k, size_t n)
{
    if (n == 0 || n > KZ3_TX_KEY_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return 0;
    }
    return 1;
}

static inline void kz3_tx_free(Kz3TxTable* t)
{
    free(t->row);
    free(t->units);
    memset(t, 0, sizeof *t);
}

/* Parse the TSV text (`n` bytes, need not end in NUL). The header line (first field "language") is skipped; a line
 * with the wrong field count, a bad key, hash, kind, UTF-8 or an over-long text is counted in `rejected`. Returns the
 * rows kept. */
static inline int kz3_tx_parse(Kz3TxTable* t, const char* text, size_t n)
{
    memset(t, 0, sizeof *t);
    size_t cap_rows = 64, cap_units = 4096;
    t->row = (Kz3TxRow*)malloc(cap_rows * sizeof *t->row);
    t->units = (uint16_t*)malloc(cap_units * sizeof *t->units);
    if (!t->row || !t->units) { kz3_tx_free(t); return 0; }
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        size_t le = e;
        if (le > i && text[le - 1] == '\r') le--;
        const char* f[6];
        size_t fl[6];
        int nf = 0;
        size_t a = i;
        for (size_t k = i; k <= le && nf < 7; k++) {
            if (k == le || text[k] == '\t') {
                if (nf < 6) { f[nf] = text + a; fl[nf] = k - a; }
                nf++;
                a = k + 1;
            }
        }
        i = e + 1;
        if (nf == 1 && fl[0] == 0) continue;   /* blank line */
        if (nf >= 1 && fl[0] == 8 && !memcmp(f[0], "language", 8)) continue;
        uint32_t h;
        if (nf != 6 || !kz3_tx_key_ok(f[1], fl[1]) || !kz3_tx_hex32(f[2], fl[2], &h) || fl[4] != 1 ||
            (f[4][0] != 'a' && f[4][0] != 'b') || fl[3] == 0) { t->rejected++; continue; }
        if (t->nunits + KZ3_TX_TEXT_MAX > cap_units) {
            uint16_t* u = (uint16_t*)realloc(t->units, (cap_units * 2) * sizeof *u);
            if (!u) { t->rejected++; continue; }
            t->units = u; cap_units *= 2;
        }
        long nu = kz3_tx_utf8_to_utf16(f[3], fl[3], t->units + t->nunits, KZ3_TX_TEXT_MAX);
        if (nu <= 0) { t->rejected++; continue; }
        /* the same key + hash twice: an identical replacement is a duplicate, a different one a conflict */
        Kz3TxRow* dup = NULL;
        for (uint32_t r = 0; r < t->nrows && !dup; r++)
            if (t->row[r].fnv == h && t->row[r].klen == fl[1] && !memcmp(t->row[r].key, f[1], fl[1])) dup = &t->row[r];
        if (dup) {
            if (dup->n != (uint32_t)nu || memcmp(t->units + dup->off, t->units + t->nunits, (size_t)nu * 2)) {
                if (dup->kind != 'x') t->conflicts++;
                dup->kind = 'x';   /* dropped: kz3_tx_find never returns it */
            }
            continue;
        }
        if (t->nrows == cap_rows) {
            Kz3TxRow* r = (Kz3TxRow*)realloc(t->row, cap_rows * 2 * sizeof *r);
            if (!r) { t->rejected++; continue; }
            t->row = r; cap_rows *= 2;
        }
        Kz3TxRow* r = &t->row[t->nrows++];
        memcpy(r->key, f[1], fl[1]);
        r->key[fl[1]] = 0;
        r->klen = (uint8_t)fl[1];
        r->kind = f[4][0];
        r->fnv = h;
        r->off = t->nunits;
        r->n = (uint32_t)nu;
        t->nunits += (uint32_t)nu;
    }
    return (int)t->nrows;
}

/* Is any row keyed `key`? (The hook asks this first, so texts of other keys are never read or hashed.) */
static inline int kz3_tx_has_key(const Kz3TxTable* t, const char* key, size_t klen)
{
    for (uint32_t r = 0; r < t->nrows; r++)
        if (t->row[r].klen == klen && !memcmp(t->row[r].key, key, klen)) return 1;
    return klen == sizeof KZ3_TX_MOTION_KEY - 1 && !memcmp(key, KZ3_TX_MOTION_KEY, klen);
}

static inline const Kz3TxRow* kz3_tx_find(const Kz3TxTable* t, const char* key, size_t klen, uint32_t fnv)
{
    for (uint32_t r = 0; r < t->nrows; r++) {
        const Kz3TxRow* w = &t->row[r];
        if (w->fnv == fnv && w->kind != 'x' && w->klen == klen && !memcmp(w->key, key, klen)) return w;
    }
    return NULL;
}

/* What to do with the text `orig` (UTF-16, n units) the lookup found for `key`: KZ3_TX_REPLACE with *units / *nu,
 * KZ3_TX_OTHER_KEY (look up KZ3_TX_MOTION_ALT_KEY instead: the motion tip without motion sensors) or KZ3_TX_NONE. */
static inline int kz3_tx_decide(const Kz3TxTable* t, const char* key, size_t klen, const uint16_t* orig, size_t n,
                                int motion, const uint16_t** units, size_t* nu)
{
    *units = NULL; *nu = 0;
    if (!t || !t->nrows || klen > KZ3_TX_KEY_MAX || n > KZ3_TX_TEXT_MAX) return KZ3_TX_NONE;
    if (!motion && klen == sizeof KZ3_TX_MOTION_KEY - 1 && !memcmp(key, KZ3_TX_MOTION_KEY, klen)) return KZ3_TX_OTHER_KEY;
    char u8[KZ3_TX_TEXT_MAX * 3];
    long b = kz3_tx_utf16_to_utf8(orig, n, u8, sizeof u8);
    if (b < 0) return KZ3_TX_NONE;
    const Kz3TxRow* r = kz3_tx_find(t, key, klen, kz3_tx_fnv1a32(u8, (size_t)b));
    if (!r) return KZ3_TX_NONE;
    *units = t->units + r->off;
    *nu = r->n;
    return KZ3_TX_REPLACE;
}

/* ---- Net pass 50: the pad's own button names in the game's text --------------------------------------------------
 * The game names PlayStation buttons in plain words in a few localized strings (no glyph tag, so the glyph-set hook of
 * patch 4012 never sees them): menu_setting_controller_swapl1l2 / _swapr1r2 ("Swap L1 and L2" in every language, the
 * L1/R2 tokens stay Latin even in ja/ko/zh/ru), menu_unlock_Frag_Grenade_Description ("Hold down R2 ..."), and
 * menu_press_start ("PRESS START"). kz3_tx_buttons() rewrites those tokens for the glyph set (PAD_PROMPT_*: 0 PS3,
 * 1 PS4/PS5, 2 Xbox, 3 keycaps) of the pad in use: Xbox L1 -> LB, L2 -> LT, L3 -> LS, R1 -> RB, R2 -> RT, R3 -> RS,
 * START -> MENU, SELECT -> VIEW; PS4/PS5 START -> OPTIONS, SELECT -> TOUCHPAD (the labels patch 4012 draws); PS3 and
 * keycaps leave the text alone (the caller passes the connected pad's set, not keycaps). A token is L or R plus 1..3,
 * or the capitals START / SELECT, with no ASCII letter or digit on either side ("L1とL2", "R2-Taste" match, "AR2" and
 * "L10" do not). Language-independent, no table, no original text.
 */
enum { KZ3_TX_SET_PS3 = 0, KZ3_TX_SET_PS4 = 1, KZ3_TX_SET_XBOX = 2, KZ3_TX_SET_KBM = 3 };

static inline int kz3_tx_alnum(uint16_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* Writes the rewritten text to out and returns its length; -1 when nothing changes (or out is too small, or n is
 * past KZ3_TX_TEXT_MAX), so the caller keeps the game's own string. */
static inline long kz3_tx_buttons(const uint16_t* in, size_t n, int set, uint16_t* out, size_t cap)
{
    if ((set != KZ3_TX_SET_XBOX && set != KZ3_TX_SET_PS4) || n > KZ3_TX_TEXT_MAX || !in) return -1;
    size_t o = 0;
    int changed = 0;
    for (size_t i = 0; i < n;) {
        const char* rep = NULL;
        size_t skip = 0;
        uint16_t c = in[i];
        if ((i == 0 || !kz3_tx_alnum(in[i - 1])) && (c == 'L' || c == 'R' || c == 'S')) {
            if ((c == 'L' || c == 'R') && i + 1 < n && in[i + 1] >= '1' && in[i + 1] <= '3' &&
                (i + 2 >= n || !kz3_tx_alnum(in[i + 2])) && set == KZ3_TX_SET_XBOX) {
                static const char* const x[2][3] = { { "LB", "LT", "LS" }, { "RB", "RT", "RS" } };
                rep = x[c == 'R'][in[i + 1] - '1'];
                skip = 2;
            } else if (c == 'S') {
                static const char* const w[2][2] = { { "START", "MENU" }, { "SELECT", "VIEW" } };
                static const char* const w4[2] = { "OPTIONS", "TOUCHPAD" };
                for (int k = 0; k < 2; k++) {
                    size_t wl = strlen(w[k][0]);
                    if (i + wl > n || (i + wl < n && kz3_tx_alnum(in[i + wl]))) continue;
                    size_t j = 0;
                    while (j < wl && in[i + j] == (uint16_t)w[k][0][j]) j++;
                    if (j == wl) { rep = set == KZ3_TX_SET_XBOX ? w[k][1] : w4[k]; skip = wl; break; }
                }
            }
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl > cap) return -1;
            for (size_t k = 0; k < rl; k++) out[o++] = (uint16_t)rep[k];
            i += skip;
            changed = 1;
        } else {
            if (o + 1 > cap) return -1;
            out[o++] = in[i++];
        }
    }
    return changed ? (long)o : -1;
}

/* ---- Net pass 50: the valve and the D-Charge without motion sensors ---------------------------------------------
 * With the game's Motion Sensor Function Off (the default on a pad without a gyro, network pass 48) the valve
 * (ValveJoystickController) and the D-Charge rotation read the stick (atan2 of axes 2 and 3 = the look stick,
 * ASSUMPTION from 0x4D0650's two stick reads), but the game still shows the tutorial texts that tell the player to
 * turn the controller; its only other variants are the Move ones (*_mc). These replace the four texts when the device in
 * use has no motion sensors. English only for now (the port_ui table can carry the other languages under the keys
 * "stick.<key>"; kz3_tr falls back to these). "\n" stays the two characters the game's strings carry.
 */
static inline const char* kz3_tx_stick_en(const char* key, size_t klen)
{
    static const struct { const char* k; const char* en; } r[] = {
        { "tutorial_valve",
          "Hold [ICON group=gamefunction id=GrabLeftHand] or [ICON group=gamefunction id=GrabRightHand] and turn the valve by moving [ICON group=axisfunction id=LookX] around to the left\\nThen release the button, move it back to the right and repeat" },
        { "tutorial_valve_grab",
          "Hold [ICON group=gamefunction id=GrabLeftHand] or [ICON group=gamefunction id=GrabRightHand] to grab onto the valve" },
        { "tutorial_valve_rotate",
          "Move [ICON group=axisfunction id=LookX] around anti-clockwise and then release [ICON group=gamefunction id=GrabLeftHand] or [ICON group=gamefunction id=GrabRightHand] to turn the valve.\\nGrab and Turn until the valve dial points all the way down." },
        { "tutorial_dcharge_rotate",
          "Move [ICON group=axisfunction id=LookX] around to arm the D-Charge" },
    };
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        if (strlen(r[i].k) == klen && !memcmp(r[i].k, key, klen)) return r[i].en;
    return NULL;
}

#endif /* KZ3_TEXT_OVERRIDES_H */
