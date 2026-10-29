/*
 * KZ3 port, network pass 45: translations of the port's own UI text (the RE notes 173.5, the RE notes s.39).
 *
 * One table, port/text/port_ui.tsv (key, max_chars, then one column per language, header names the columns), shipped
 * as lang\port_ui.tsv beside kz3.exe and embedded in the exe (C23 #embed). The file wins cell by cell; cells it lacks
 * (or a missing / truncated / unreadable file) come from the embedded copy; a key in neither, an empty cell or an
 * unknown language gives the caller's English literal, so the code keeps its English text and nothing changes when
 * the table is absent.
 *
 * Language = the game's own id (GetLanguage, 1.00 0x4C4C78 / 1.14 0x4D10A4 = [profile+0xE90], 173.2), reported by
 *   - kz3_tr_note_lang(id): a wrapper on the lifted GetLanguage (maintainer, work/net/r45/getlanguage-hook.diff);
 *   - the bridge command "kb:lang <id>" (kz3_tr_ext_cmd) that the port's pages send from Lua
 *     (gPlayerProfile:GetLanguage()) right before they open the next port page, and when they open;
 *   - KZ3_TR_LANG=<id | column name> (test switch, wins over both; "0"/"off" = English).
 * Pages are generated when they open (the serve hooks regenerate a page whose language changed), so a change in the
 * game's language list applies at the next page open.
 *
 * Page templates carry markers that kz3_tr_render expands (the English literal is part of the marker):
 *   @tr{key|English}            XML text / attribute value (escaped; `\n` -> <br>; @game:<id> -> @key{<id>})
 *   @tru{key|English}           the same, upper case (the column headers the game draws in capitals)
 *   @trl{key|English}           inside a Lua "..." string (escaped; @game cells -> the English literal)
 *   @trx{key|English|e0|e1|e2}  a whole Lua expression: "text"..(e0).."text" with {0}..{2} -> the Lua expressions
 * A translation whose placeholder set differs from the English literal's is not used (the literal is).
 * Every cell from the file is untrusted: bounded sizes, valid UTF-8 only, no control characters, at most
 * KZ3_TR_MAX_ROWS rows.
 *
 * Implementation: compiled once, in port/net/kz3_discord.c (always linked): `#define KZ3_TR_IMPLEMENTATION` there.
 * Other translation units (main.cpp, kz3_prompts_ui.h in kz3_ppu_overrides.cpp, kz3_overlay.c) include this header
 * for the declarations. Host test: tools/net/test_tr.c.
 */
#ifndef KZ3_TR_H
#define KZ3_TR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KZ3_TR_MAX_ROWS   1024
#define KZ3_TR_MAX_CELL   600      /* bytes of one cell after unescaping */
#define KZ3_TR_MAX_FILE   (1u << 20)

/* The UTF-8 text for key in the current language, or `en` (never NULL when en is not NULL). `@game:` cells give `en`
 * here unless a resolver is set (kz3_tr_set_game_resolver). */
const char* kz3_tr(const char* key, const char* en);
/* kz3_tr with {0}..{2} replaced by a0..a2 (NULL = ""), into out (always NUL-terminated); returns strlen(out). */
size_t kz3_trf(char* out, size_t cap, const char* key, const char* en, const char* a0, const char* a1, const char* a2);
/* Expand the markers of tmpl (n bytes); malloc'd NUL-terminated result in *out, returns its length; on failure (no
 * memory) *out = NULL and 0. Text outside markers is copied unchanged; a malformed marker is copied as it is. */
size_t kz3_tr_render(const char* tmpl, size_t n, char** out);

int  kz3_tr_lang(void);                 /* the language in force (1 = English when nothing reported) */
void kz3_tr_note_lang(int id);          /* report the game's language id (any thread) */
int  kz3_tr_lang_from_name(const char* s);   /* "es" / "4" / "ru" -> id; 0 when unknown */
/* The bridge command "lang [id]" (patch 4025's g_pad_kb_ext_cmd chain, kz3_discord.c): 1 when answered. */
int  kz3_tr_ext_cmd(const char* cmd, char* out, size_t n);
/* Resolver for `@game:<InGameID>` cells on the C side (the Localizer path); returns UTF-8 or NULL. */
void kz3_tr_set_game_resolver(const char* (*fn)(const char* id));
/* Load state for the log and tests: rows from the file / from the embedded copy; source path ("" = none). */
void kz3_tr_stats(unsigned* file_rows, unsigned* embedded_rows, const char** file_path);

/* Host tests only: forget the loaded tables and the reported language; load from this text instead of the file
 * (NULL = the file again). */
void kz3_tr_test_reset(const char* file_text, size_t file_n, int use_embedded);

#ifdef __cplusplus
}
#endif

#ifdef KZ3_TR_IMPLEMENTATION
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__has_embed)
#  if __has_embed("../text/port_ui.tsv")
static const unsigned char kz3_tr_embedded[] = {
#    embed "../text/port_ui.tsv"
};
#    define KZ3_TR_HAVE_EMBED 1
#  endif
#endif

typedef struct { const char* name; int id; } Kz3TrLangName;
static const Kz3TrLangName kz3_tr_langs[] = {
    { "en", 1 }, { "es", 4 }, { "es_la", 20 }, { "pt_br", 21 }, { "fr", 3 }, { "de", 5 }, { "it", 6 }, { "nl", 7 },
    { "pt", 8 }, { "zh_tw", 10 }, { "ko", 11 }, { "ru", 12 }, { "pl", 13 }, { "da", 14 }, { "fi", 15 },
    { "no", 16 }, { "sv", 17 }, { "ja", 18 }, { "tr", 22 },
};
#define KZ3_TR_NLANG ((int)(sizeof kz3_tr_langs / sizeof kz3_tr_langs[0]))
#define KZ3_TR_MAX_ID 32

typedef struct { char* key; char* cell[KZ3_TR_MAX_ID + 1]; } Kz3TrRow;   /* cell[id], NULL = empty */
typedef struct { Kz3TrRow* rows; unsigned n; char* store; } Kz3TrTable;

static Kz3TrTable s_tr_file, s_tr_emb;
static int s_tr_state;                    /* 0 not loaded, 1 loading, 2 ready */
static int s_tr_lang_reported;            /* 0 = none */
static int s_tr_lang_env = -1;            /* -1 not read, 0 none, else id */
static char s_tr_path[1100];
static const char* (*s_tr_resolver)(const char* id);
static const char* s_tr_test_text;
static size_t s_tr_test_n;
static int s_tr_test_mode;                /* 0 normal, 1 test text, 2 test text + embedded */

int kz3_tr_lang_from_name(const char* s)
{
    if (!s || !*s) return 0;
    if (s[0] >= '0' && s[0] <= '9') {
        char* e;
        long v = strtol(s, &e, 10);
        if (*e) return 0;
        for (int i = 0; i < KZ3_TR_NLANG; i++)
            if (kz3_tr_langs[i].id == v) return (int)v;
        return 0;
    }
    for (int i = 0; i < KZ3_TR_NLANG; i++)
        if (!strcmp(kz3_tr_langs[i].name, s)) return kz3_tr_langs[i].id;
    return 0;
}

/* Valid UTF-8, no C0 controls / DEL, no surrogates or overlongs. */
static int kz3_tr_utf8_ok(const unsigned char* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (c < 0x80) { i++; continue; }
        int k = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if (k < 0 || i + (size_t)k >= n) return 0;
        unsigned cp = c & (0x3F >> k);
        for (int j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

/* Parse TSV text into t (copies everything). Rows with a bad key or a short line keep the cells they have; cells that
 * are not clean UTF-8 or too long count as empty. Returns the number of rows. */
static unsigned kz3_tr_parse(Kz3TrTable* t, const char* text, size_t n)
{
    memset(t, 0, sizeof *t);
    if (!text || n == 0 || n > KZ3_TR_MAX_FILE) return 0;
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3; n -= 3;
    }
    t->store = (char*)malloc(n + 1);
    t->rows = (Kz3TrRow*)calloc(KZ3_TR_MAX_ROWS, sizeof(Kz3TrRow));
    if (!t->store || !t->rows) { free(t->store); free(t->rows); memset(t, 0, sizeof *t); return 0; }
    memcpy(t->store, text, n);
    t->store[n] = 0;
    int colid[64];
    int ncol = 0, line = 0;
    char* p = t->store;
    char* end = t->store + n;
    while (p < end) {
        char* eol = (char*)memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;                       /* a truncated last line still counts */
        char* le = eol;
        if (le > p && le[-1] == '\r') le--;
        *le = 0;
        char* cells[64];
        int nc = 0;
        char* c = p;
        for (;;) {
            char* tab = (char*)memchr(c, '\t', (size_t)(le - c));
            if (nc < 64) cells[nc++] = c;
            if (!tab) break;
            *tab = 0;
            c = tab + 1;
        }
        if (line == 0) {
            if (nc < 3 || strcmp(cells[0], "key")) return 0;        /* not this table: nothing from it */
            for (int i = 0; i < nc; i++) {
                colid[i] = (i >= 2 && cells[i][0] > '9') ? kz3_tr_lang_from_name(cells[i]) : 0;   /* names, not ids */
            }
            ncol = nc;
        } else if (nc >= 1 && cells[0][0] && t->n < KZ3_TR_MAX_ROWS && strlen(cells[0]) < 64 &&
                   kz3_tr_utf8_ok((const unsigned char*)cells[0], strlen(cells[0]))) {
            Kz3TrRow* r = &t->rows[t->n++];
            r->key = cells[0];
            for (int i = 2; i < nc && i < ncol; i++) {
                char* s = cells[i];
                size_t k = strlen(s);
                if (!colid[i] || k == 0 || k > KZ3_TR_MAX_CELL || !kz3_tr_utf8_ok((const unsigned char*)s, k)) continue;
                char* w = s;                       /* unescape \n (line break) and \\ in place */
                for (char* q = s; *q; q++) {
                    if (q[0] == '\\' && q[1] == 'n') { *w++ = '\n'; q++; }
                    else if (q[0] == '\\' && q[1] == '\\') { *w++ = '\\'; q++; }
                    else *w++ = *q;
                }
                *w = 0;
                r->cell[colid[i]] = s;
            }
        }
        line++;
        p = eol + 1;
    }
    return t->n;
}

static void kz3_tr_free(Kz3TrTable* t)
{
    free(t->store);
    free(t->rows);
    memset(t, 0, sizeof *t);
}

static void kz3_tr_exe_dir(char* dir, size_t cap)
{
    dir[0] = 0;
#ifdef _WIN32
    char exe[1024];
    DWORD m = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (m == 0 || m >= sizeof exe) return;
    char* sl = strrchr(exe, '\\');
    char* s2 = strrchr(exe, '/');
    if (s2 > sl) sl = s2;
    if (!sl) return;
    *sl = 0;
    snprintf(dir, cap, "%s", exe);
#else
    snprintf(dir, cap, ".");
#endif
}

static char* kz3_tr_read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* b = NULL;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long m = ftell(f);
        if (m > 0 && (unsigned long)m <= KZ3_TR_MAX_FILE && fseek(f, 0, SEEK_SET) == 0 && (b = (char*)malloc((size_t)m)))
            *n = fread(b, 1, (size_t)m, f);
    }
    fclose(f);
    return b;
}

static void kz3_tr_load(void)
{
    int st = __atomic_load_n(&s_tr_state, __ATOMIC_ACQUIRE);
    if (st == 2) return;
    int zero = 0;
    if (!__atomic_compare_exchange_n(&s_tr_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s_tr_state, __ATOMIC_ACQUIRE) != 2) { }   /* another thread is parsing (once, short) */
        return;
    }
    if (s_tr_test_mode) {
        kz3_tr_parse(&s_tr_file, s_tr_test_text, s_tr_test_n);
#ifdef KZ3_TR_HAVE_EMBED
        if (s_tr_test_mode == 2) kz3_tr_parse(&s_tr_emb, (const char*)kz3_tr_embedded, sizeof kz3_tr_embedded);
#endif
    } else {
        const char* e = getenv("KZ3_TR_FILE");     /* test switch: another table file ("0" = none) */
        if (e && *e) {
            if (strcmp(e, "0")) snprintf(s_tr_path, sizeof s_tr_path, "%s", e);
        } else {
            char dir[1024];
            kz3_tr_exe_dir(dir, sizeof dir);
            if (dir[0]) snprintf(s_tr_path, sizeof s_tr_path, "%s/lang/port_ui.tsv", dir);
        }
        size_t n = 0;
        char* b = s_tr_path[0] ? kz3_tr_read_file(s_tr_path, &n) : NULL;
        if (b) kz3_tr_parse(&s_tr_file, b, n);
        free(b);
        if (!s_tr_file.n) s_tr_path[0] = 0;
#ifdef KZ3_TR_HAVE_EMBED
        kz3_tr_parse(&s_tr_emb, (const char*)kz3_tr_embedded, sizeof kz3_tr_embedded);
#endif
        fprintf(stderr, "[tr] port UI text: %u row(s) from %s, %u embedded\n", s_tr_file.n,
                s_tr_path[0] ? s_tr_path : "no lang/port_ui.tsv", s_tr_emb.n);
    }
    __atomic_store_n(&s_tr_state, 2, __ATOMIC_RELEASE);
}

void kz3_tr_test_reset(const char* file_text, size_t file_n, int use_embedded)
{
    kz3_tr_free(&s_tr_file);
    kz3_tr_free(&s_tr_emb);
    s_tr_path[0] = 0;
    s_tr_test_text = file_text;
    s_tr_test_n = file_n;
    s_tr_test_mode = file_text ? (use_embedded ? 2 : 1) : 0;
    s_tr_lang_reported = 0;
    s_tr_lang_env = -1;
    __atomic_store_n(&s_tr_state, 0, __ATOMIC_RELEASE);
}

void kz3_tr_stats(unsigned* file_rows, unsigned* embedded_rows, const char** file_path)
{
    kz3_tr_load();
    if (file_rows) *file_rows = s_tr_file.n;
    if (embedded_rows) *embedded_rows = s_tr_emb.n;
    if (file_path) *file_path = s_tr_path;
}

int kz3_tr_lang(void)
{
    if (s_tr_lang_env < 0) {
        const char* e = getenv("KZ3_TR_LANG");
        int v = 0;
        if (e && *e) v = (!strcmp(e, "0") || !strcmp(e, "off")) ? 1 : kz3_tr_lang_from_name(e);
        s_tr_lang_env = v;
    }
    if (s_tr_lang_env > 0) return s_tr_lang_env;
    int r = __atomic_load_n(&s_tr_lang_reported, __ATOMIC_ACQUIRE);
    return r > 0 ? r : 1;
}

void kz3_tr_note_lang(int id)
{
    if (id <= 0 || id > KZ3_TR_MAX_ID) return;
    int old = __atomic_exchange_n(&s_tr_lang_reported, id, __ATOMIC_ACQ_REL);
    if (old != id) fprintf(stderr, "[tr] game language %d (was %d)\n", id, old);
}

int kz3_tr_ext_cmd(const char* cmd, char* out, size_t n)
{
    if (!cmd || strncmp(cmd, "lang", 4) || (cmd[4] && cmd[4] != ' ')) return 0;
    if (cmd[4] == ' ') {
        char* e;
        long v = strtol(cmd + 5, &e, 10);
        if (e != cmd + 5 && !*e && v > 0 && v <= KZ3_TR_MAX_ID) kz3_tr_note_lang((int)v);
    }
    if (out && n) snprintf(out, n, "%d", kz3_tr_lang());
    return 1;
}

void kz3_tr_set_game_resolver(const char* (*fn)(const char* id)) { s_tr_resolver = fn; }

static const char* kz3_tr_find(const Kz3TrTable* t, const char* key, int id)
{
    size_t kl = strlen(key);
    for (unsigned i = 0; i < t->n; i++) {
        /* network pass 46: a row's key may list aliases ("disp.v.default,lp.v.default": one cell, two keys) */
        const char* k = t->rows[i].key;
        while (k && *k) {
            const char* e = strchr(k, ',');
            size_t l = e ? (size_t)(e - k) : strlen(k);
            if (l == kl && !memcmp(k, key, kl)) return t->rows[i].cell[id];   /* first row of a key wins */
            k = e ? e + 1 : NULL;
        }
    }
    return NULL;
}
/* Placeholder mask of s: bit k for each {k}, k 0..2; bit 3 for {3}..{9} (never allowed). */
static unsigned kz3_tr_phmask(const char* s)
{
    unsigned m = 0;
    for (; s && *s; s++)
        if (s[0] == '{' && s[1] >= '0' && s[1] <= '9' && s[2] == '}') m |= s[1] <= '2' ? 1u << (s[1] - '0') : 8u;
    return m;
}

/* The raw cell (may be "@game:..."), or NULL for "use en". */
static const char* kz3_tr_cell(const char* key, const char* en)
{
    int id = kz3_tr_lang();
    if (!key || id <= 1 || id > KZ3_TR_MAX_ID) return NULL;
    kz3_tr_load();
    const char* c = kz3_tr_find(&s_tr_file, key, id);   /* cell by cell: the file, then the embedded copy */
    if (!c) c = kz3_tr_find(&s_tr_emb, key, id);
    if (!c || !*c) return NULL;
    if (strncmp(c, "@game:", 6) && kz3_tr_phmask(c) != kz3_tr_phmask(en)) return NULL;
    return c;
}

const char* kz3_tr(const char* key, const char* en)
{
    const char* c = kz3_tr_cell(key, en);
    if (!c) return en;
    if (!strncmp(c, "@game:", 6)) {
        const char* r = s_tr_resolver ? s_tr_resolver(c + 6) : NULL;
        return r ? r : en;
    }
    return c;
}

size_t kz3_trf(char* out, size_t cap, const char* key, const char* en, const char* a0, const char* a1, const char* a2)
{
    const char* s = kz3_tr(key, en);
    const char* a[3] = { a0 ? a0 : "", a1 ? a1 : "", a2 ? a2 : "" };
    size_t w = 0;
    if (!out || !cap) return 0;
    for (; s && *s; s++) {
        const char* ins = NULL;
        if (s[0] == '{' && s[1] >= '0' && s[1] <= '2' && s[2] == '}') { ins = a[s[1] - '0']; s += 2; }
        if (ins) {
            for (; *ins && w + 1 < cap; ins++) out[w++] = *ins;
        } else if (w + 1 < cap) out[w++] = *s;
    }
    out[w] = 0;
    return w;
}

/* --- render ------------------------------------------------------------------------------------------------- */
typedef struct { char* p; size_t n, cap; int err; } Kz3TrBuf;
static void kz3_tr_put(Kz3TrBuf* b, const char* s, size_t k)
{
    if (b->err) return;
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + k + 1) nc *= 2;
        char* q = (char*)realloc(b->p, nc);
        if (!q) { b->err = 1; return; }
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, s, k);
    b->n += k;
    b->p[b->n] = 0;
}
static void kz3_tr_puts(Kz3TrBuf* b, const char* s) { kz3_tr_put(b, s, strlen(s)); }

/* One code point of s (valid UTF-8 from the table or a literal) -> its upper case for the scripts the table uses
 * (Latin-1, Latin Extended-A, Greek and Cyrillic basics; Turkish dotted i when lang is 22). */
static unsigned kz3_tr_upper_cp(unsigned c, int lang)
{
    if (c >= 'a' && c <= 'z') return (lang == 22 && c == 'i') ? 0x130 : c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
static void kz3_tr_put_cp(Kz3TrBuf* b, unsigned c)
{
    char u[4];
    size_t k;
    if (c < 0x80) { u[0] = (char)c; k = 1; }
    else if (c < 0x800) { u[0] = (char)(0xC0 | (c >> 6)); u[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
    else if (c < 0x10000) { u[0] = (char)(0xE0 | (c >> 12)); u[1] = (char)(0x80 | ((c >> 6) & 0x3F)); u[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
    else { u[0] = (char)(0xF0 | (c >> 18)); u[1] = (char)(0x80 | ((c >> 12) & 0x3F)); u[2] = (char)(0x80 | ((c >> 6) & 0x3F)); u[3] = (char)(0x80 | (c & 0x3F)); k = 4; }
    kz3_tr_put(b, u, k);
}

/* mode 0 XML, 1 XML upper case, 2 Lua string contents. */
static void kz3_tr_put_text(Kz3TrBuf* b, const char* s, size_t k, int mode)
{
    int lang = kz3_tr_lang();
    size_t i = 0;
    while (i < k) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp = c;
        size_t len = 1;
        if (c >= 0xC0) {
            len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (i + len > k) break;
            cp = c & (0x3F >> (len - 1));
            for (size_t j = 1; j < len; j++) cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
        }
        i += len;
        if (mode == 2) {
            if (cp == '\\') kz3_tr_puts(b, "\\\\");
            else if (cp == '"') kz3_tr_puts(b, "\\\"");
            else if (cp == '\n') kz3_tr_puts(b, "<br>");
            else if (cp == ']' && i < k && s[i] == ']') kz3_tr_puts(b, "]\"..\"");   /* never "]]>" inside CDATA */
            else kz3_tr_put_cp(b, cp);
            continue;
        }
        if (mode == 1) cp = kz3_tr_upper_cp(cp, lang);
        if (cp == '&') kz3_tr_puts(b, "&amp;");
        else if (cp == '<') kz3_tr_puts(b, "&lt;");
        else if (cp == '>') kz3_tr_puts(b, "&gt;");
        else if (cp == '"') kz3_tr_puts(b, "&quot;");
        else if (cp == '\n') kz3_tr_puts(b, "&lt;br&gt;");
        else kz3_tr_put_cp(b, cp);
    }
}

size_t kz3_tr_render(const char* tmpl, size_t n, char** out)
{
    Kz3TrBuf b = { 0, 0, 0, 0 };
    size_t i = 0;
    *out = NULL;
    kz3_tr_put(&b, "", 0);
    while (i < n) {
        const char* at = (const char*)memchr(tmpl + i, '@', n - i);
        if (!at) { kz3_tr_put(&b, tmpl + i, n - i); break; }
        size_t a = (size_t)(at - tmpl);
        kz3_tr_put(&b, tmpl + i, a - i);
        int mode = -1;
        size_t hl = 0;
        if (a + 4 <= n && !memcmp(at, "@tr{", 4)) { mode = 0; hl = 4; }
        else if (a + 5 <= n && !memcmp(at, "@tru{", 5)) { mode = 1; hl = 5; }
        else if (a + 5 <= n && !memcmp(at, "@trl{", 5)) { mode = 2; hl = 5; }
        else if (a + 5 <= n && !memcmp(at, "@trx{", 5)) { mode = 3; hl = 5; }
        const char* close = mode >= 0 ? (const char*)memchr(at + hl, '}', n - a - hl) : NULL;
        /* {0}..{2} inside the English literal are not the closing brace: skip "{d}" pairs */
        while (close && close > at + hl && close[-1] >= '0' && close[-1] <= '9' && close - 2 >= at + hl && close[-2] == '{')
            close = (const char*)memchr(close + 1, '}', n - (size_t)(close + 1 - tmpl));
        if (mode < 0 || !close) { kz3_tr_put(&b, at, 1); i = a + 1; continue; }
        /* fields: key|en[|e0|e1|e2] */
        const char* f[5];
        size_t fl[5];
        int nf = 0;
        const char* s = at + hl;
        while (nf < 5) {
            const char* bar = (const char*)memchr(s, '|', (size_t)(close - s));
            f[nf] = s;
            fl[nf] = (size_t)((bar ? bar : close) - s);
            nf++;
            if (!bar) break;
            s = bar + 1;
        }
        if (nf < 2 || fl[0] == 0 || fl[0] >= 64) { kz3_tr_put(&b, at, 1); i = a + 1; continue; }
        char key[64], en[KZ3_TR_MAX_CELL + 1];
        memcpy(key, f[0], fl[0]); key[fl[0]] = 0;
        size_t el = fl[1] < KZ3_TR_MAX_CELL ? fl[1] : KZ3_TR_MAX_CELL;
        memcpy(en, f[1], el); en[el] = 0;
        const char* c = kz3_tr_cell(key, en);
        int game = c && !strncmp(c, "@game:", 6);
        if (mode <= 1) {
            if (game) { kz3_tr_puts(&b, "@key{"); kz3_tr_puts(&b, c + 6); kz3_tr_puts(&b, "}"); }
            else { const char* t = c ? c : en; kz3_tr_put_text(&b, t, strlen(t), mode); }
        } else {
            const char* t = game ? kz3_tr(key, en) : c ? c : en;
            if (mode == 2) kz3_tr_put_text(&b, t, strlen(t), 2);
            else {
                /* "a"..(e0).."b": split at {k}; an empty text part is left out */
                const char* p = t;
                int first = 1;
                while (*p) {
                    const char* q = p;
                    while (*q && !(q[0] == '{' && q[1] >= '0' && q[1] <= '2' && q[2] == '}')) q++;
                    if (q > p) {
                        if (!first) kz3_tr_puts(&b, "..");
                        kz3_tr_puts(&b, "\"");
                        kz3_tr_put_text(&b, p, (size_t)(q - p), 2);
                        kz3_tr_puts(&b, "\"");
                        first = 0;
                    }
                    if (!*q) break;
                    int k = q[1] - '0';
                    if (!first) kz3_tr_puts(&b, "..");
                    kz3_tr_puts(&b, "(");
                    if (k + 2 < nf) kz3_tr_put(&b, f[k + 2], fl[k + 2]);
                    else kz3_tr_puts(&b, "\"\"");
                    kz3_tr_puts(&b, ")");
                    first = 0;
                    p = q + 3;
                }
                if (first) kz3_tr_puts(&b, "\"\"");
            }
        }
        i = (size_t)(close - tmpl) + 1;
    }
    if (b.err) { free(b.p); return 0; }
    *out = b.p;
    return b.n;
}
#endif /* KZ3_TR_IMPLEMENTATION */

#endif /* KZ3_TR_H */
