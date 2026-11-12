/* KZ3 port: the online method files (network pass 17). See kz3_online_cfg.h. */
#include "kz3_online_cfg.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define OC_LINE_MAX 512

static void oc_say(kz3_oc_log_fn log, int line, const char* what)
{
    char m[160];
    if (!log) return;
    snprintf(m, sizeof(m), "online file line %d rejected: %s", line, what);
    log(m);
}

static int oc_ieq(const char* a, const char* b)
{
    for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {}
    return *a == 0 && *b == 0;
}

void kz3_oc_defaults(Kz3OcConfig* c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->comm_id, sizeof(c->comm_id), "%s", KZ3_OC_COMM_ID);
}

int kz3_oc_valid_online_name(const char* s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) && ch < 128) && ch != '-' && ch != '_') return 0;
    }
    return 1;
}

int kz3_oc_valid_host(const char* s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 253) return 0;
    int all_num = 1, labels = 0;
    size_t lab = 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (i == n || ch == '.') {
            if (lab == 0 || lab > 63) return 0;
            if (s[i - 1] == '-') return 0;
            labels++; lab = 0;
            continue;
        }
        if (!((isalnum(ch) && ch < 128) || ch == '-')) return 0;
        if (lab == 0 && ch == '-') return 0;
        if (!isdigit(ch)) all_num = 0;
        lab++;
    }
    if (all_num) {   /* digits and dots only: must be a dotted IPv4 */
        if (labels != 4) return 0;
        const char* q = s;
        for (int k = 0; k < 4; k++) {
            unsigned v = 0; int d = 0;
            while (isdigit((unsigned char)*q)) { v = v * 10 + (unsigned)(*q - '0'); q++; d++; if (v > 255 || d > 3) return 0; }
            if (d == 0) return 0;
            if (*q == '.') q++;
        }
    }
    return 1;
}

int kz3_oc_parse_hostport(const char* s, unsigned default_port, char host[254], uint16_t* port)
{
    char b[300];
    if (!s || strlen(s) >= sizeof(b)) return 0;
    snprintf(b, sizeof(b), "%s", s);
    unsigned p = default_port;
    char* colon = strchr(b, ':');
    if (colon) {
        if (strchr(colon + 1, ':')) return 0;   /* IPv6 literals are not supported */
        *colon = 0;
        const char* ps = colon + 1;
        if (!*ps || strlen(ps) > 5) return 0;
        p = 0;
        for (; *ps; ps++) { if (!isdigit((unsigned char)*ps)) return 0; p = p * 10 + (unsigned)(*ps - '0'); }
    }
    if (p < 1 || p > 65535 || !kz3_oc_valid_host(b)) return 0;
    snprintf(host, 254, "%s", b);
    *port = (uint16_t)p;
    return 1;
}

int kz3_oc_valid_comm_id(const char* s)
{
    if (!s || strlen(s) != 12 || strncmp(s, "NPWR", 4) || s[9] != '_') return 0;
    for (int i = 4; i < 9; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return isdigit((unsigned char)s[10]) && isdigit((unsigned char)s[11]);
}

int kz3_oc_valid_preset_name(const char* s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > KZ3_OC_PRESET_NAME_MAX || s[0] == ' ' || s[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!(isalnum(ch) && ch < 128) && ch != ' ' && ch != '-' && ch != '_' && ch != '.') return 0;
    }
    return 1;
}

static int find_preset(const Kz3OcConfig* c, const char* name)
{
    for (int i = 0; i < c->npresets; i++) if (oc_ieq(c->presets[i].name, name)) return i;
    return -1;
}

static int parse_bool(const char* v, int* out)
{
    if (!strcmp(v, "1") || oc_ieq(v, "on") || oc_ieq(v, "yes") || oc_ieq(v, "true")) { *out = 1; return 1; }
    if (!strcmp(v, "0") || oc_ieq(v, "off") || oc_ieq(v, "no") || oc_ieq(v, "false")) { *out = 0; return 1; }
    return 0;
}

static char* trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* kind 0 = repsn file, 1 = community file */
static int oc_parse(const char* text, size_t len, Kz3OcConfig* c, kz3_oc_log_fn log, int kind)
{
    int bad = 0, lineno = 0, sect = 1;   /* 1 = the kind's main section, 2 = a preset, 0 = ignored */
    int cur = -1;
    size_t pos = 0;
    if (!text) return 0;
    while (pos < len) {
        size_t e = pos;
        while (e < len && text[e] != '\n') e++;
        size_t n = e - pos;
        char line[OC_LINE_MAX + 1];
        lineno++;
        int too_long = n > OC_LINE_MAX;
        if (!too_long) { memcpy(line, text + pos, n); line[n] = 0; }
        pos = e + 1;
        if (too_long) { bad++; oc_say(log, lineno, "line too long"); sect = 0; continue; }
        if (memchr(line, 0, n)) { bad++; oc_say(log, lineno, "NUL byte"); sect = 0; continue; }
        char* cm = strpbrk(line, "#;");
        if (cm) *cm = 0;
        char* l = trim(line);
        if (!*l) continue;
        if (*l == '[') {
            size_t k = strlen(l);
            if (l[k - 1] != ']') { bad++; oc_say(log, lineno, "unterminated section"); sect = 0; continue; }
            l[k - 1] = 0;
            char* nm = trim(l + 1);
            if (oc_ieq(nm, kind ? "community" : "repsn")) { sect = 1; continue; }
            if (kind && !strncmp(nm, "preset", 6) && (nm[6] == ' ' || nm[6] == '\t')) {
                char* pn = trim(nm + 6);
                if (!kz3_oc_valid_preset_name(pn)) { bad++; oc_say(log, lineno, "bad preset name"); sect = 0; continue; }
                cur = find_preset(c, pn);
                if (cur < 0) {
                    if (c->npresets >= KZ3_OC_MAX_PRESETS) { bad++; oc_say(log, lineno, "too many presets"); sect = 0; continue; }
                    cur = c->npresets++;
                    memset(&c->presets[cur], 0, sizeof(c->presets[cur]));
                    snprintf(c->presets[cur].name, sizeof(c->presets[cur].name), "%s", pn);
                }
                sect = 2;
                continue;
            }
            bad++; oc_say(log, lineno, kind ? "not a community file section" : "not a repsn file section (files are never mixed)");
            sect = 0;
            continue;
        }
        char* eq = strchr(l, '=');
        if (!eq) { bad++; oc_say(log, lineno, "no key = value"); continue; }
        *eq = 0;
        char* key = trim(l);
        char* val = trim(eq + 1);
        if (sect == 0) { bad++; oc_say(log, lineno, "key in a rejected section"); continue; }
        int b;
        if (!kind && sect == 1) {
            if (oc_ieq(key, "enabled") && parse_bool(val, &b)) c->repsn_enabled = b;
            else if (oc_ieq(key, "server") && kz3_oc_parse_hostport(val, KZ3_OC_REPSN_DEFAULT_PORT, c->repsn_host, &c->repsn_port)) {}
            else if (oc_ieq(key, "name") && kz3_oc_valid_online_name(val)) snprintf(c->repsn_name, sizeof(c->repsn_name), "%s", val);
            else if (oc_ieq(key, "comm_id") && kz3_oc_valid_comm_id(val)) snprintf(c->comm_id, sizeof(c->comm_id), "%s", val);
            else if (oc_ieq(key, "p2p_port") && strlen(val) <= 5 && *val && strspn(val, "0123456789") == strlen(val) && atoi(val) <= 65535)
                c->p2p_port = (uint16_t)atoi(val);
            else if (oc_ieq(key, "report_rtt") && parse_bool(val, &b)) c->report_rtt = b;
            else { bad++; oc_say(log, lineno, "unknown key or bad value"); }
        } else if (kind && sect == 1) {
            if (oc_ieq(key, "enabled") && parse_bool(val, &b)) c->community_enabled = b;
            else if (oc_ieq(key, "active") && (!*val || kz3_oc_valid_preset_name(val))) snprintf(c->active, sizeof(c->active), "%s", val);
            else { bad++; oc_say(log, lineno, "unknown key or bad value"); }
        } else if (kind && sect == 2 && cur >= 0) {
            Kz3OcPreset* p = &c->presets[cur];
            if (oc_ieq(key, "address") && kz3_oc_parse_hostport(val, KZ3_OC_COMMUNITY_DEFAULT_PORT, p->host, &p->port)) {}
            else if (oc_ieq(key, "name") && kz3_oc_valid_online_name(val)) snprintf(p->online_name, sizeof(p->online_name), "%s", val);
            else { bad++; oc_say(log, lineno, "unknown key or bad value"); }
        } else { bad++; oc_say(log, lineno, "key outside a section it belongs to"); }
    }
    c->rejected += bad;
    return bad;
}

int kz3_oc_parse_repsn(const char* t, size_t n, Kz3OcConfig* c, kz3_oc_log_fn log) { return oc_parse(t, n, c, log, 0); }
int kz3_oc_parse_community(const char* t, size_t n, Kz3OcConfig* c, kz3_oc_log_fn log) { return oc_parse(t, n, c, log, 1); }

/* ---- presets and method ------------------------------------------------------------------------------------ */

static int preset_fields(Kz3OcPreset* p, const char* address, const char* online_name)
{
    char h[254]; uint16_t port;
    if (!kz3_oc_parse_hostport(address, KZ3_OC_COMMUNITY_DEFAULT_PORT, h, &port)) return -1;
    if (online_name && *online_name && !kz3_oc_valid_online_name(online_name)) return -1;
    snprintf(p->host, sizeof(p->host), "%s", h);
    p->port = port;
    snprintf(p->online_name, sizeof(p->online_name), "%s", online_name ? online_name : "");
    return 0;
}

int kz3_oc_preset_add(Kz3OcConfig* c, const char* name, const char* address, const char* online_name)
{
    Kz3OcPreset p;
    if (!kz3_oc_valid_preset_name(name) || find_preset(c, name) >= 0 || c->npresets >= KZ3_OC_MAX_PRESETS) return -1;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "%s", name);
    if (preset_fields(&p, address, online_name)) return -1;
    c->presets[c->npresets++] = p;
    return 0;
}

int kz3_oc_preset_edit(Kz3OcConfig* c, const char* name, const char* address, const char* online_name)
{
    int i = find_preset(c, name);
    Kz3OcPreset p;
    if (i < 0) return -1;
    p = c->presets[i];
    if (preset_fields(&p, address, online_name)) return -1;
    c->presets[i] = p;
    return 0;
}

int kz3_oc_preset_delete(Kz3OcConfig* c, const char* name)
{
    int i = find_preset(c, name);
    if (i < 0) return -1;
    if (oc_ieq(c->active, c->presets[i].name)) c->active[0] = 0;
    for (; i + 1 < c->npresets; i++) c->presets[i] = c->presets[i + 1];
    memset(&c->presets[--c->npresets], 0, sizeof(c->presets[0]));
    return 0;
}

int kz3_oc_preset_pick(Kz3OcConfig* c, const char* name)
{
    int i = find_preset(c, name);
    if (i < 0) return -1;
    snprintf(c->active, sizeof(c->active), "%s", c->presets[i].name);
    return 0;
}

void kz3_oc_set_method(Kz3OcConfig* c, Kz3OcMethod m)
{
    c->repsn_enabled = m == KZ3_OC_REPSN;
    c->community_enabled = m == KZ3_OC_COMMUNITY;
}

/* ---- selection and egress ---------------------------------------------------------------------------------- */

static void refuse(Kz3OcSession* s, const char* log, const char* player)
{
    s->method = KZ3_OC_REFUSED;
    s->host[0] = 0; s->port = 0; s->port_features = 0;
    snprintf(s->log_line, sizeof(s->log_line), "%s", log);
    snprintf(s->player_message, sizeof(s->player_message), "%s", player);
}

void kz3_oc_select(const Kz3OcConfig* c, Kz3OcSession* s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < c->npresets && s->nother < KZ3_OC_MAX_PRESETS; i++)
        if (c->presets[i].host[0]) snprintf(s->other_hosts[s->nother++], 254, "%s", c->presets[i].host);
    snprintf(s->comm_id, sizeof(s->comm_id), "%s", c->comm_id[0] ? c->comm_id : KZ3_OC_COMM_ID);
    s->p2p_port = c->p2p_port;
    s->report_rtt = c->report_rtt;
    if (c->repsn_enabled && c->community_enabled) {
        refuse(s, "online refused: re-psn and a community server are both enabled; only one method may be on, none is picked for you",
               "Online play is off: re-psn and a community server are both turned on. Turn one of them off.");
        return;
    }
    if (c->repsn_enabled) {
        if (!c->repsn_port || !c->repsn_host[0]) {
            refuse(s, "online refused: re-psn is enabled but has no valid server", "Online play is off: the re-psn server is missing or invalid.");
            return;
        }
        if (!kz3_oc_valid_online_name(c->repsn_name)) {
            refuse(s, "online refused: re-psn is enabled but the online name is missing or invalid (3-16 letters, digits, - or _)",
                   "Online play is off: set an online name of 3 to 16 letters, digits, - or _.");
            return;
        }
        for (int i = 0; i < s->nother; i++)
            if (oc_ieq(s->other_hosts[i], c->repsn_host)) {
                refuse(s, "online refused: the re-psn server is also a community preset host; the two methods never share a host",
                       "Online play is off: the re-psn server is the same as a community server entry.");
                return;
            }
        s->method = KZ3_OC_REPSN;
        snprintf(s->host, sizeof(s->host), "%s", c->repsn_host);
        s->port = c->repsn_port;
        snprintf(s->online_name, sizeof(s->online_name), "%s", c->repsn_name);
        s->port_features = 1;
        snprintf(s->log_line, sizeof(s->log_line), "online method: re-psn (base game and port additions go through re-psn)");
        return;
    }
    if (c->community_enabled) {
        int i = c->active[0] ? find_preset(c, c->active) : -1;
        if (i < 0 || !c->presets[i].host[0]) {
            refuse(s, "online refused: a community server is enabled but no valid preset is picked",
                   "Online play is off: pick a community server entry first.");
            return;
        }
        s->method = KZ3_OC_COMMUNITY;
        snprintf(s->host, sizeof(s->host), "%s", c->presets[i].host);
        s->port = c->presets[i].port;
        snprintf(s->online_name, sizeof(s->online_name), "%s", c->presets[i].online_name);
        s->port_features = 0;
        snprintf(s->log_line, sizeof(s->log_line), "online method: community server (the game's own online only; port-added features are off)");
        snprintf(s->player_message, sizeof(s->player_message),
                 "Port features such as campaign co-op are not available on a community server.");
        return;
    }
    s->method = KZ3_OC_OFF;
    snprintf(s->log_line, sizeof(s->log_line), "online method: off");
}

static int game_port_ok(const Kz3OcSession* s, unsigned port)
{
    return port == s->port || (port >= 10070 && port <= 10080) || (port >= 50000 && port <= 50199);
}

int kz3_oc_egress_check(const Kz3OcSession* s, Kz3OcRoute route, const char* host, unsigned port)
{
    if (!s || !host || (s->method != KZ3_OC_REPSN && s->method != KZ3_OC_COMMUNITY)) return 1;
    if (s->method == KZ3_OC_COMMUNITY && route == KZ3_ROUTE_PORT) return 2;   /* port additions never ride a community session */
    if (s->method == KZ3_OC_REPSN)
        for (int i = 0; i < s->nother; i++)
            if (oc_ieq(s->other_hosts[i], host)) return 4;                    /* a re-psn session never reaches a community host */
    if (!oc_ieq(s->host, host)) return 3;
    if (route == KZ3_ROUTE_PORT) return port == s->port ? 0 : 5;
    return game_port_ok(s, port) ? 0 : 5;
}

/* ---- text -------------------------------------------------------------------------------------------------- */

static size_t app(char* out, size_t cap, size_t n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static size_t app(char* out, size_t cap, size_t n, const char* fmt, ...)
{
    va_list ap;
    int k;
    if (n == (size_t)-1) return n;
    va_start(ap, fmt);
    k = vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= cap - n) return (size_t)-1;
    return n + (size_t)k;
}

size_t kz3_oc_format_repsn(const Kz3OcConfig* c, int scope, char* out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    n = app(out, cap, n, "%s", scope == KZ3_OC_SCOPE_SHARED
        ? "; re-psn, player-wide (shared by every port). One online method at a time: not together with community.ini.\n"
        : "; re-psn, this game's own values.\n");
    n = app(out, cap, n, "[repsn]\n");
    if (scope == KZ3_OC_SCOPE_SHARED) {
        n = app(out, cap, n, "enabled = %d\n", c->repsn_enabled ? 1 : 0);
        if (c->repsn_port) n = app(out, cap, n, "server = %s:%u\n", c->repsn_host, (unsigned)c->repsn_port);
        if (c->repsn_name[0]) n = app(out, cap, n, "name = %s\n", c->repsn_name);
    } else {
        n = app(out, cap, n, "comm_id = %s\n", c->comm_id[0] ? c->comm_id : KZ3_OC_COMM_ID);
        n = app(out, cap, n, "p2p_port = %u\n", (unsigned)c->p2p_port);
        n = app(out, cap, n, "report_rtt = %d\n", c->report_rtt ? 1 : 0);
    }
    if (n == (size_t)-1) { out[0] = 0; return 0; }
    return n;
}

size_t kz3_oc_format_community(const Kz3OcConfig* c, char* out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    n = app(out, cap, n, "; Community server (a PSN replacement run by someone else), player-wide.\n");
    n = app(out, cap, n, "; Port-exclusive features (campaign co-op and other port additions) do not work on a community server.\n");
    n = app(out, cap, n, "; One online method at a time: not together with repsn.ini. No entry comes with the port; add your own.\n");
    n = app(out, cap, n, "[community]\nenabled = %d\nactive = %s\n", c->community_enabled ? 1 : 0, c->active);
    for (int i = 0; i < c->npresets; i++) {
        const Kz3OcPreset* p = &c->presets[i];
        n = app(out, cap, n, "\n[preset %s]\n", p->name);
        if (p->host[0]) n = app(out, cap, n, "address = %s:%u\n", p->host, (unsigned)p->port);
        if (p->online_name[0]) n = app(out, cap, n, "name = %s\n", p->online_name);
    }
    if (n == (size_t)-1) { out[0] = 0; return 0; }
    return n;
}

/* ---- files ------------------------------------------------------------------------------------------------- */

#ifdef _WIN32
static wchar_t* oc_wide(const char* s)
{
    int k = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = k > 0 ? (wchar_t*)malloc((size_t)k * sizeof(wchar_t)) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, k) <= 0) { free(w); w = NULL; }
    return w;
}
#endif

char* kz3_oc_read_file(const char* path, size_t* len)
{
    FILE* f;
    long n;
    char* buf = NULL;
#ifdef _WIN32
    wchar_t* w = oc_wide(path);
    f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && n <= KZ3_OC_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
        buf = (char*)malloc((size_t)n + 1);
        if (buf) {
            size_t r = fread(buf, 1, (size_t)n, f);
            if (r != (size_t)n) { free(buf); buf = NULL; }
            else { buf[r] = 0; *len = r; }
        }
    }
    fclose(f);
    return buf;
}

int kz3_oc_write_file(const char* path, const char* data, size_t len)
{
    char tmp[1100];
    FILE* f;
    if (!path || !data || strlen(path) > 1000) return -1;
#ifdef _WIN32
    snprintf(tmp, sizeof(tmp), "%s.tmp%lu", path, (unsigned long)GetCurrentProcessId());
    wchar_t* wt = oc_wide(tmp);
    wchar_t* wp = oc_wide(path);
    int rc = -1;
    f = wt ? _wfopen(wt, L"wb") : NULL;
    if (f && wp) {
        int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && _commit(_fileno(f)) == 0;
        ok = (fclose(f) == 0) && ok;
        if (ok && MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) rc = 0;
    } else if (f) fclose(f);
    if (rc && wt) DeleteFileW(wt);
    free(wt); free(wp);
    return rc;
#else
    snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path, (long)getpid());
    f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
#endif
}

size_t kz3_oc_shared_dir(const char* portable_dir, char* out, size_t cap)
{
    const char* e;
    int k = -1;
    if (!cap) return 0;
    out[0] = 0;
    if ((e = getenv("KZ3_SHARED_DIR")) && *e) k = snprintf(out, cap, "%s", e);
    else if ((e = getenv("PS3RECOMP_SHARED_DIR")) && *e) k = snprintf(out, cap, "%s", e);   /* the name J-Stars' port reads (shared format) */
    else if (portable_dir && *portable_dir) k = snprintf(out, cap, "%s/shared", portable_dir);
#ifdef _WIN32
    else if ((e = getenv("APPDATA")) && *e) k = snprintf(out, cap, "%s\\PS3Recomp", e);
#else
    else if ((e = getenv("XDG_CONFIG_HOME")) && *e) k = snprintf(out, cap, "%s/ps3recomp", e);
    else if ((e = getenv("HOME")) && *e) k = snprintf(out, cap, "%s/.config/ps3recomp", e);
#endif
    if (k < 0 || (size_t)k >= cap) { out[0] = 0; return 0; }
    return (size_t)k;
}

static int load_one(const char* dir, const char* file, Kz3OcConfig* c, kz3_oc_log_fn log, int kind)
{
    char path[1100];
    size_t len = 0;
    if (!dir || !*dir) return 0;
    size_t dl = strlen(dir);
    if (dl > 1000) return 0;
    snprintf(path, sizeof(path), "%s%s%s", dir, (dir[dl - 1] == '/' || dir[dl - 1] == '\\') ? "" : "/", file);
    char* t = kz3_oc_read_file(path, &len);
    if (!t) return 0;
    oc_parse(t, len, c, log, kind);
    free(t);
    return 1;
}

int kz3_oc_load(const char* shared_dir, const char* game_dir, Kz3OcConfig* c, kz3_oc_log_fn log)
{
    int n = 0;
    n += load_one(shared_dir, "repsn.ini", c, log, 0);
    n += load_one(game_dir, "repsn.ini", c, log, 0);
    n += load_one(shared_dir, "community.ini", c, log, 1);
    n += load_one(game_dir, "community.ini", c, log, 1);
    return n;
}
