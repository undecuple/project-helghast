/* kz3_menu_version.h: the port's release version in the main menu (ENHANCEMENT, display, default on).
 *
 * A text block in the bottom corner opposite the button legend of mainmenu.gum:
 *   line 1  "Project Helghast <port_version>"
 *   line 2  "Version <newer_version> is available"   (only when the launcher wrote newer_version)
 * The values come from kz3.ini (`port_version`, `newer_version`, written by the launcher; the game itself opens no
 * connection for this) or from KZ3_PORT_VERSION / KZ3_NEWER_VERSION (they win, for tests). No port_version: nothing
 * is drawn. KZ3_MENU_VERSION=0 turns the block off.
 *
 * The pure parts (value choice, text, GUM fragment) have no ps3recomp types: tools/net/test_menu_version.c includes
 * this file on the host. The miss-hook glue is in kz3_prompts_ui.h (page served by main.cpp, plus this block).
 *
 * The newer line is translated through the port text table (key menu.version_newer, `%s` = the version; the glue
 * passes kz3_tr's text, English is the fallback); line 1 is a name and stays as it is. A template that is not exactly
 * one `%s` and otherwise plain text is not used (kmv_fmt_ok). */
#ifndef KZ3_MENU_VERSION_H
#define KZ3_MENU_VERSION_H

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define KMV_VER_MAX 24

/* A version string as drawn: 1..KMV_VER_MAX-1 characters of [0-9A-Za-z.+_-] after trimming, else 0. The value is
 * written into a GUM text, so nothing that could be markup or a @-marker passes. */
static int kmv_clean(const char* in, char* out, size_t cap)
{
    size_t n = in ? strlen(in) : 0, i;
    out[0] = 0;
    while (n && isspace((unsigned char)in[n - 1])) n--;
    while (n && isspace((unsigned char)*in)) { in++; n--; }
    if (n == 0 || n >= KMV_VER_MAX || n >= cap) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (!(isalnum(c) || c == '.' || c == '+' || c == '_' || c == '-')) return 0;
    }
    memcpy(out, in, n);
    out[n] = 0;
    return 1;
}

/* The first non-empty `key = value` of the ini text (any section, case-insensitive key, ';'/'#' comments, optional
 * double quotes: kz3_ini.h's rules) into out (raw, unvalidated), or "". */
static void kmv_ini_value(const char* text, const char* key, char* out, size_t cap)
{
    const size_t kl = strlen(key);
    const char* p = text;
    out[0] = 0;
    if (!p) return;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        const char* s = p;
        while (s < eol && (*s == ' ' || *s == '\t')) s++;
        const char* eq = s;
        while (eq < eol && *eq != '=') eq++;
        if (s < eol && *s != ';' && *s != '#' && *s != '[' && eq < eol) {
            const char* ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            size_t i = 0;
            if ((size_t)(ke - s) == kl)
                while (i < kl && tolower((unsigned char)s[i]) == tolower((unsigned char)key[i])) i++;
            if (i == kl && (size_t)(ke - s) == kl) {
                const char* v = eq + 1;
                const char* ve = eol;
                while (v < ve && (*v == ' ' || *v == '\t')) v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                if (ve - v >= 2 && *v == '"' && ve[-1] == '"') { v++; ve--; }
                if (ve > v && (size_t)(ve - v) < cap) { memcpy(out, v, (size_t)(ve - v)); out[ve - v] = 0; return; }
            }
        }
        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
}

/* One value: the environment text when it is non-empty (valid or not: a bad test value draws nothing, it does not
 * fall back to the file), else the ini key. -> 1 and `out` set when it is a valid version string. */
static int kmv_value(const char* env, const char* ini_text, const char* key, char* out, size_t cap)
{
    char raw[64];
    if (env && *env) return kmv_clean(env, out, cap);
    kmv_ini_value(ini_text, key, raw, sizeof raw);
    return kmv_clean(raw, out, cap);
}

/* The English text of the newer line; also the fallback of every rejected template. */
#define KMV_NEWER_EN "Version %s is available"

/* A newer-line template is usable when it has exactly one `%s`, no other `%`, and none of the characters that would
 * be markup or a page marker in a GUM text (< > & " @) or a control character; at most 120 bytes. (Table cells are
 * untrusted: the file in lang\ can be edited.) */
static int kmv_fmt_ok(const char* f)
{
    int n = 0;
    size_t i, len = f ? strlen(f) : 0;
    if (!len || len > 120) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)f[i];
        if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '&' || c == '"' || c == '@') return 0;
        if (c == '%') {
            if (f[i + 1] != 's') return 0;
            n++;
            i++;
        }
    }
    return n == 1;
}

/* The newer line: the template (or English when it is not usable) with the version put in place of `%s`. */
static void kmv_newer_text(const char* fmt, const char* nv, char* out, size_t cap)
{
    const char* at;
    if (!kmv_fmt_ok(fmt)) fmt = KMV_NEWER_EN;
    at = strstr(fmt, "%s");
    snprintf(out, cap, "%.*s%s%s", (int)(at - fmt), fmt, nv, at + 2);
}

/* The lines to draw: 0 = nothing, 1 = the version line, 2 = also the newer line. A newer version equal to the
 * current one draws no second line. */
static int kmv_lines(const char* env_pv, const char* env_nv, const char* ini_text, const char* newer_fmt,
                     char* l1, size_t c1, char* l2, size_t c2)
{
    char pv[KMV_VER_MAX], nv[KMV_VER_MAX];
    l1[0] = l2[0] = 0;
    if (!kmv_value(env_pv, ini_text, "port_version", pv, sizeof pv)) return 0;
    snprintf(l1, c1, "Project Helghast %s", pv);
    if (!kmv_value(env_nv, ini_text, "newer_version", nv, sizeof nv) || !strcmp(nv, pv)) return 1;
    kmv_newer_text(newer_fmt, nv, l2, c2);
    return 2;
}

/* Style: the game's `nofilter` text class is the one that draws like the legend's labels (dim red, no glow, the same
 * face and about the same size; the default text class and `legalline` are cream with a glow). Chosen by comparing the
 * classes' rendering with the legend in a Proton run (the classes live in the binary basemenu styles lump, which has
 * no readable property list). ASSUMPTION: the legend's alpha equals that class's.
 *
 * Placement (page coordinates inside the safe-area panel; the panel's origin sits about 86 px right of and 81 px below
 * the screen's corner at 1280x720): the lines end at the right margin the legend's left margin mirrors. The last line
 * sits one line above the legend's baseline, not on it: a text whose box reaches below about panel y 580 at 1280x720
 * is drawn dimmed (peak red 74..129 against 154..162 above that line; a second line at KMV_Y 584 was nearly
 * invisible, the first was not), the screen-overlay textures of the page's hud elements (type1.gum: HudElement5 at
 * 764,616 and the glows around it) are drawn over the panel's contents there. ASSUMPTION: that overlay is the cause
 * (the dimming is deterministic per page and run time, and it is gone when both lines sit at 528/556); the draw
 * order is not read from the binary. */
#define KMV_X 516
#define KMV_W 560
#define KMV_Y 556
#define KMV_STEP 28

/* GUM fragment for the lines, bottom right inside the page's safe-area panel (the legend is bottom left). */
static int kmv_fragment(const char* l1, const char* l2, char* out, size_t cap)
{
    int n = snprintf(out, cap,
        "\t\t<text id=\"txt_kz3_version\" class=\"nofilter\" x=\"%d\" y=\"%d\" width=\"%d\" text-align=\"right\">%s</text>\n",
        KMV_X, l2[0] ? KMV_Y - KMV_STEP : KMV_Y, KMV_W, l1);
    if (n < 0 || (size_t)n >= cap) return 0;
    if (l2[0]) {
        int m = snprintf(out + n, cap - (size_t)n,
            "\t\t<text id=\"txt_kz3_newer\" class=\"nofilter\" x=\"%d\" y=\"%d\" width=\"%d\" text-align=\"right\">%s</text>\n",
            KMV_X, KMV_Y, KMV_W, l2);
        if (m < 0 || (size_t)m >= cap - (size_t)n) return 0;
        n += m;
    }
    return n;
}

/* The offset of the </panel> that closes the page's safe-area panel (class="safearea"; nested panels, such as the main
 * menu's hidden info panel, are skipped by counting <panel>/</panel> pairs), or -1. */
static long kmv_safearea_end(const char* text)
{
    const char* at = strstr(text, "class=\"safearea\"");
    const char* p;
    int depth = 1;
    if (!at) return -1;
    p = at;
    while (p > text && strncmp(p, "<panel", 6)) p--;
    if (strncmp(p, "<panel", 6)) return -1;
    p = strchr(at, '>');
    if (!p || p[-1] == '/') return -1;
    for (p++; *p; p++) {
        if (!strncmp(p, "</panel>", 8)) {
            if (--depth == 0) return (long)(p - text);
            p += 7;
        } else if (!strncmp(p, "<panel", 6) && (p[6] == ' ' || p[6] == '>' || p[6] == '\t' || p[6] == '\n')) {
            const char* e = strchr(p, '>');
            if (!e) return -1;
            if (e[-1] != '/') depth++;
            p = e;
        }
    }
    return -1;
}

/* Insert the fragment at the end of the safe-area panel; the new text (malloc) and *pn, or NULL (nothing to draw, no
 * such panel). env/ini as kmv_lines. */
static char* kmv_patch_text(const char* text, size_t len, size_t* pn, const char* env_pv, const char* env_nv,
                            const char* ini_text, const char* newer_fmt)
{
    char l1[64], l2[256], frag[2048];
    size_t fl;
    long off;
    char* out;
    if (kmv_lines(env_pv, env_nv, ini_text, newer_fmt, l1, sizeof l1, l2, sizeof l2) == 0) return NULL;
    fl = (size_t)kmv_fragment(l1, l2, frag, sizeof frag);
    if (!fl) return NULL;
    off = kmv_safearea_end(text);
    if (off < 0) return NULL;
    out = (char*)malloc(len + fl + 1);
    if (!out) return NULL;
    memcpy(out, text, (size_t)off);
    memcpy(out + off, frag, fl);
    memcpy(out + off + fl, text + off, len - (size_t)off);
    out[len + fl] = 0;
    *pn = len + fl;
    return out;
}

#endif /* KZ3_MENU_VERSION_H */
