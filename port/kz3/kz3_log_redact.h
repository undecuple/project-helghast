/* kz3_log_redact: removes the Windows user name and the player's folders from log text before it is written (log file,
 * crash report, ring). Same rules as the first-run log's redactor (tools/release/firstrun.py redactor(), the one rule
 * in tools/release/pathrules.py; 2026-11-02):
 *  - the install folder (KZ3_INSTALL_DIR, set by the launcher) becomes %INSTALL%, the rest of the path stays;
 *    %LOCALAPPDATA% and %APPDATA% folders keep those names;
 *  - the user's profile folder (USERPROFILE) in these forms, any letter case: as set, with backslashes, with forward
 *    slashes, doubled backslashes (repr / JSON), CMake's mangled object-path form (C_/Users/name, ':' and ' ' -> '_'),
 *    and URL-encoded forms (%5C, %2F, %3A, %20) becomes %USERPROFILE%;
 *  - the name in any other <drive>:\Users\<name> (same separator forms) becomes <user>;
 *  - the Unix home seen under Wine/Proton (Z:/home/<name>, Z:\home\<name>, doubled backslashes, %2F/%5C/%3A forms,
 *    Z_/home/<name>, and the plain /home/<name>) gets <name> replaced by <user>; a /home inside a longer path is left.
 *  - last: every other absolute path (a drive X:\ or X:/ in any case, a UNC path, a Unix path with two or more
 *    components) becomes <path>\ plus its LAST component; relative paths and the game's virtual /dev_*, /app_*,
 *    /host_root paths stay.
 * Header-only, no allocation beyond kz3_redact's scratch buffers, C11. A line that does not hold either is copied byte for byte. */
#ifndef KZ3_LOG_REDACT_H
#define KZ3_LOG_REDACT_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define KZ3_REDACT_VARIANTS 64
#define KZ3_REDACT_HOME_MAX 520

typedef struct {
    char   v[KZ3_REDACT_VARIANTS][KZ3_REDACT_HOME_MAX * 3];
    size_t len[KZ3_REDACT_VARIANTS];
    const char* label[KZ3_REDACT_VARIANTS];   /* what the spelling is replaced by */
    char   bound[KZ3_REDACT_VARIANTS];        /* the folder name must end there (the install folder) */
    int    n;
} kz3_redact_t;

static inline char kz3r_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static inline int kz3r_match(const char* s, size_t n, const char* lit, size_t ln)
{
    if (ln > n) return 0;
    for (size_t i = 0; i < ln; i++) if (kz3r_lc(s[i]) != kz3r_lc(lit[i])) return 0;
    return 1;
}

/* one spelling of the home folder: each char of h goes through the mapping (bs/slash/colon/space replacement text) */
static inline void kz3r_add(kz3_redact_t* r, const char* h, const char* bs, const char* sl, const char* co, const char* sp,
                            const char* label, int bound)
{
    if (r->n >= KZ3_REDACT_VARIANTS) return;
    char* d = r->v[r->n]; size_t o = 0;
    for (const char* p = h; *p; p++) {
        const char* t = NULL; char one[2] = { *p, 0 };
        if (*p == '\\') t = bs; else if (*p == '/') t = sl; else if (*p == ':') t = co; else if (*p == ' ') t = sp; else t = one;
        size_t tl = strlen(t);
        if (o + tl >= sizeof r->v[0]) return;
        memcpy(d + o, t, tl); o += tl;
    }
    for (int i = 0; i < r->n; i++) if (r->len[i] == o && memcmp(r->v[i], d, o) == 0) return;   /* duplicate spelling */
    d[o] = 0; r->len[r->n] = o; r->label[r->n] = label; r->bound[r->n] = (char)bound; r->n++;
}

static inline void kz3_redact_add_folder(kz3_redact_t* r, const char* folder, const char* label, int bound);

/* home = the USERPROFILE value (NULL or short: only the generic Users rule applies). Trailing separators are ignored. */
static inline void kz3_redact_init(kz3_redact_t* r, const char* home)
{
    r->n = 0;
    kz3_redact_add_folder(r, home, "%USERPROFILE%", 0);
}

static inline void kz3_redact_add_folder(kz3_redact_t* r, const char* home, const char* label, int bound)
{
    char h[KZ3_REDACT_HOME_MAX]; size_t l;
    if (!home) return;
    l = strlen(home);
    while (l && (home[l - 1] == '\\' || home[l - 1] == '/')) l--;
    if (l <= 3 || l >= sizeof h) return;                     /* "C:\" or nothing: too short to be a profile */
    memcpy(h, home, l); h[l] = 0;
    /* longest spellings first, so a longer one is never cut short by a shorter prefix */
    kz3r_add(r, h, "\\\\", "/", ":", " ", label, bound);                   /* doubled backslashes */
    kz3r_add(r, h, "%5C", "%2F", "%3A", "%20", label, bound);
    kz3r_add(r, h, "%5C", "%2F", ":", "%20", label, bound);
    kz3r_add(r, h, "%5C", "/", "%3A", "%20", label, bound);
    kz3r_add(r, h, "%5C", "/", ":", "%20", label, bound);
    kz3r_add(r, h, "\\", "%2F", "%3A", "%20", label, bound);
    kz3r_add(r, h, "/", "%2F", "%3A", "%20", label, bound);
    kz3r_add(r, h, "/", "%2F", ":", "%20", label, bound);
    kz3r_add(r, h, "/", "/", "%3A", "%20", label, bound);
    kz3r_add(r, h, "/", "/", ":", "%20", label, bound);
    kz3r_add(r, h, "\\", "\\", ":", "%20", label, bound);
    kz3r_add(r, h, "\\", "\\", ":", " ", label, bound);                    /* as set */
    kz3r_add(r, h, "\\", "/", ":", " ", label, bound);
    kz3r_add(r, h, "/", "/", ":", " ", label, bound);                      /* forward slashes */
    kz3r_add(r, h, "/", "/", "_", "_", label, bound);                      /* CMake's mangled form */
    kz3r_add(r, h, "\\", "\\", ":", " ", label, bound);
    /* the longest spelling must win at one position: order by length, descending */
    for (int i = 1; i < r->n; i++)
        for (int j = i; j > 0 && r->len[j] > r->len[j - 1]; j--) {
            char t[sizeof r->v[0]]; size_t tl = r->len[j]; const char* tlab = r->label[j]; char tb = r->bound[j];
            memcpy(t, r->v[j], tl + 1); memcpy(r->v[j], r->v[j - 1], r->len[j - 1] + 1); memcpy(r->v[j - 1], t, tl + 1);
            r->len[j] = r->len[j - 1]; r->len[j - 1] = tl;
            r->label[j] = r->label[j - 1]; r->label[j - 1] = tlab; r->bound[j] = r->bound[j - 1]; r->bound[j - 1] = tb;
        }
}

typedef struct { char* d; size_t cap, o; } kz3r_out;
static inline void kz3r_put(kz3r_out* w, const char* s, size_t n)
{
    if (w->o + n > w->cap) n = w->cap > w->o ? w->cap - w->o : 0;
    memcpy(w->d + w->o, s, n); w->o += n;
}

/* length of a separator at s: \ \\ / %5C %2F (0 = none) */
static inline size_t kz3r_sep(const char* s, size_t n)
{
    if (n >= 2 && s[0] == '\\' && s[1] == '\\') return 2;
    if (n >= 1 && (s[0] == '\\' || s[0] == '/')) return 1;
    if (n >= 3 && s[0] == '%' && (kz3r_match(s, n, "%5C", 3) || kz3r_match(s, n, "%2F", 3))) return 3;
    return 0;
}

/* the drive prefix "C:", "C_" or "C%3A" at s followed by a separator, "Users", a separator: -> length through the
 * separator after Users, else 0 */
static inline size_t kz3r_users_prefix(const char* s, size_t n)
{
    size_t i = 0, k;
    if (n < 1 || !(((s[0] | 32) >= 'a') && ((s[0] | 32) <= 'z'))) return 0;
    i = 1;
    if (i < n && (s[i] == ':' || s[i] == '_')) i++;
    else if (kz3r_match(s + i, n - i, "%3A", 3)) i += 3;
    else return 0;
    if (!(k = kz3r_sep(s + i, n - i))) return 0;
    i += k;
    if (!kz3r_match(s + i, n - i, "Users", 5)) return 0;
    i += 5;
    if (!(k = kz3r_sep(s + i, n - i))) return 0;
    return i + k;
}

/* "<drive>:<sep>home<sep>" or "<sep>home<sep>" at s -> length through the separator after home, else 0 */
static inline size_t kz3r_home_prefix(const char* s, size_t n)
{
    size_t i = 0, k;
    if (n >= 2 && (((s[0] | 32) >= 'a') && ((s[0] | 32) <= 'z')) && (s[1] == ':' || s[1] == '_' || kz3r_match(s + 1, n - 1, "%3A", 3))) {
        i = (s[1] == ':' || s[1] == '_') ? 2 : 4;
        if (!(k = kz3r_sep(s + i, n - i))) return 0;
        i += k;
    } else if ((k = kz3r_sep(s, n))) {
        i = k;
    } else {
        return 0;
    }
    if (!kz3r_match(s + i, n - i, "home", 4)) return 0;
    i += 4;
    if (!(k = kz3r_sep(s + i, n - i))) return 0;
    return i + k;
}

static inline int kz3r_pathchar(char c)
{
    return ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

static inline void kz3_redact_add_home(kz3_redact_t* r, const char* dir) { kz3_redact_add_folder(r, dir, "%USERPROFILE%", 0); }
static inline void kz3_redact_add_install(kz3_redact_t* r, const char* dir) { kz3_redact_add_folder(r, dir, "%INSTALL%", 1); }
static inline void kz3_redact_add_localappdata(kz3_redact_t* r, const char* dir) { kz3_redact_add_folder(r, dir, "%LOCALAPPDATA%", 0); }
static inline void kz3_redact_add_appdata(kz3_redact_t* r, const char* dir) { kz3_redact_add_folder(r, dir, "%APPDATA%", 0); }

/* A Unix home (HOME, Wine's WINEHOMEDIR): added as a literal only when the generic <drive>:/home/<name> and /home/<name>
 * rule would not already cover it (/root, /var/lib/x). */
static inline void kz3_redact_add_unix_home(kz3_redact_t* r, const char* home)
{
    if (!home) return;
    if (kz3r_home_prefix(home, strlen(home))) return;
    kz3_redact_add_home(r, home);
}

static inline int kz3r_isword(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
static inline int kz3r_isletter(char c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
/* the folders in the middle of a path stop at a quote, | * ? : a tab or a line end (not at a space) */
static inline int kz3r_pstop(char c) { return c == '"' || c == '\'' || c == '|' || c == '*' || c == '?' || c == ':' || c == '\t' || c == '\r' || c == '\n'; }
static inline int kz3r_fstop(char c) { return kz3r_pstop(c) || c == ' ' || c == ',' || c == ';' || c == ')'; }

/* A path starts at s[i]? -> length of its prefix (through the first separator of a drive, through the separator after a
 * UNC server, the slash of a Unix path), *kind = 1 drive, 2 UNC, 3 Unix; 0 = none. */
static inline size_t kz3r_path_prefix(const char* s, size_t n, size_t i, int* kind)
{
    size_t k;
    *kind = 0;
    if (kz3r_isletter(s[i])) {
        if (i > 0 && kz3r_isword(s[i - 1])) return 0;
        if (i + 2 <= n && s[i + 1] == ':' && (k = kz3r_sep(s + i + 2, n - i - 2))) { *kind = 1; return 2 + k; }
        if (i + 4 <= n && kz3r_match(s + i + 1, n - i - 1, "%3A", 3) && (k = kz3r_sep(s + i + 4, n - i - 4))) { *kind = 1; return 4 + k; }
        if (i + 3 <= n && s[i + 1] == '_' && s[i + 2] == '/') { *kind = 1; return 3; }
        return 0;
    }
    if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\\') {
        size_t j = i + 2;
        if (i > 0) { char c = s[i - 1]; if (kz3r_isword(c) || c == '\\' || c == '/' || c == '.' || c == ':' || c == '%' || c == '-') return 0; }
        while (j < n && !kz3r_sep(s + j, n - j) && !kz3r_fstop(s[j])) j++;
        if (j > i + 2 && j < n && (k = kz3r_sep(s + j, n - j))) { *kind = 2; return j - i + k; }
        return 0;
    }
    if (s[i] == '/') {
        if (i > 0) {
            char c = s[i - 1];
            if (!(c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '=' || c == '(' || c == '[' || c == ',' || c == ';' || c == '{' ||
                  (c == '/' && i >= 7 && kz3r_match(s + i - 7, 7, "file://", 7)))) return 0;
        }
        if (kz3r_match(s + i + 1, n - i - 1, "dev_", 4) || kz3r_match(s + i + 1, n - i - 1, "app_", 4) ||
            kz3r_match(s + i + 1, n - i - 1, "host_root", 9)) return 0;   /* the game's virtual paths */
        *kind = 3;
        return 1;
    }
    return 0;
}

/* Every other absolute path -> <path>\ + its last component (the folders may hold spaces and parentheses; the last
 * component ends at white space, a quote, ',' ';' ')' ':'). Same rule as tools/release/pathrules.py. */
static inline void kz3r_paths(const char* s, size_t n, kz3r_out* w)
{
    for (size_t i = 0; i < n;) {
        int kind; size_t pl = kz3r_path_prefix(s, n, i, &kind), last = 0;
        if (pl) {
            size_t k = i + pl;
            if (kind != 3) last = i + pl;          /* a Unix path needs one more separator */
            while (k < n && !kz3r_pstop(s[k]) && !(s[k] == ' ' && k + 1 < n && s[k + 1] == '/')) {
                size_t sl = kz3r_sep(s + k, n - k);
                if (sl) { k += sl; last = k; } else k++;
            }
        }
        if (last) {
            size_t e = last;
            while (e < n && !kz3r_fstop(s[e]) && !kz3r_sep(s + e, n - e)) e++;
            kz3r_put(w, "<path>\\", 7);
            kz3r_put(w, s + last, e - last);
            i = e;
        } else {
            kz3r_put(w, s + i++, 1);
        }
    }
}

/* Redacts src[0..n) into dst (capacity cap, no terminator written). Returns the length written (cut at cap). */
static inline size_t kz3_redact(const kz3_redact_t* r, const char* src, size_t n, char* dst, size_t cap)
{
    /* pass 1: the install, AppData and home folders; pass 2: other users' names; pass 3: every other absolute path */
    size_t scr = n * 4 + 64;
    char* mid = (char*)malloc(scr);
    char* mid2 = (char*)malloc(scr);
    kz3r_out w1 = { mid, scr, 0 };
    if (!mid || !mid2) { free(mid); free(mid2); size_t m = n < cap ? n : cap; memcpy(dst, src, m); return m; }
    for (size_t i = 0; i < n;) {
        int hit = 0;
        for (int v = 0; v < r->n && !hit; v++)
            if (kz3r_match(src + i, n - i, r->v[v], r->len[v])) {
                size_t e = i + r->len[v];
                if (r->bound[v] && e < n && (kz3r_isword(src[e]) || src[e] == '.' || src[e] == '-')) continue;   /* C:\KZ3-old is not C:\KZ3 */
                kz3r_put(&w1, r->label[v], strlen(r->label[v])); i = e; hit = 1;
            }
        if (!hit) kz3r_put(&w1, src + i++, 1);
    }
    kz3r_out w2 = { mid2, scr, 0 };
    for (size_t i = 0; i < w1.o;) {
        size_t pl = 0;
        int word_start = (i == 0) || !(((mid[i - 1] | 32) >= 'a' && (mid[i - 1] | 32) <= 'z') || (mid[i - 1] >= '0' && mid[i - 1] <= '9') || mid[i - 1] == '_');
        if (word_start && (pl = kz3r_users_prefix(mid + i, w1.o - i)) != 0 &&
            !kz3r_match(mid + i + pl, w1.o - i - pl, "%USERPROFILE%", 13)) {
            size_t e = i + pl, k = e;
            while (k < w1.o && mid[k] != '"' && mid[k] != '\'' && mid[k] != '\r' && mid[k] != '\n' && !kz3r_sep(mid + k, w1.o - k)) k++;
            if (k > e) { kz3r_put(&w2, mid + i, pl); kz3r_put(&w2, "<user>", 6); i = k; continue; }
        }
        if ((i == 0 || !kz3r_pathchar(mid[i - 1])) && (pl = kz3r_home_prefix(mid + i, w1.o - i)) != 0) {
            size_t e = i + pl, k = e;
            while (k < w1.o && mid[k] != '"' && mid[k] != '\'' && mid[k] != '\r' && mid[k] != '\n' && !kz3r_sep(mid + k, w1.o - k)) k++;
            if (k > e) { kz3r_put(&w2, mid + i, pl); kz3r_put(&w2, "<user>", 6); i = k; continue; }
        }
        kz3r_put(&w2, mid + i++, 1);
    }
    free(mid);
    kz3r_out w3 = { dst, cap, 0 };
    kz3r_paths(mid2, w2.o, &w3);
    free(mid2);
    return w3.o;
}
#endif
