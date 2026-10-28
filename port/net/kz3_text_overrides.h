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

#endif /* KZ3_TEXT_OVERRIDES_H */
