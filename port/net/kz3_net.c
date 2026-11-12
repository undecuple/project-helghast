/*
 * KZ3 port: online setup (see kz3_net.h). Pass 3 of the net work; plan: the RE notes section 5.
 */
#include "kz3_net.h"
#include "kz3_online_cfg.h"
#include "np_kz3.h"
#include "net_policy.h"   /* ps3recomp libs/network, patch 4001 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <netinet/in.h>
#endif

void kz3_net_config_defaults(Kz3NetConfig* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ranges[0][0] = 10070; cfg->ranges[0][1] = 10080;
    cfg->ranges[1][0] = 50000; cfg->ranges[1][1] = 50199;
    cfg->nranges = 2;
}

static void say(kz3_net_log_fn log, int line, const char* what)
{
    char m[160];
    if (!log) return;
    snprintf(m, sizeof(m), "online.ini line %d: %s", line, what);
    log(m);
}

/* DNS name (labels of [A-Za-z0-9-], 1-63 each, not starting/ending with '-') or dotted IPv4; at most 253 chars. */
static int valid_host(const char* h)
{
    size_t n = strlen(h), lab = 0;
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i <= n; i++) {
        char c = h[i];
        if (c == '.' || c == 0) {
            if (lab == 0 || lab > 63 || h[i - 1] == '-' || h[i - lab] == '-') return 0;
            lab = 0;
        } else if (isalnum((unsigned char)c) || c == '-') {
            lab++;
        } else {
            return 0;
        }
    }
    return 1;
}

/* "a-b, c, d-e": up to KZ3_NET_MAX_RANGES ranges of 1..65535. */
static int parse_ranges(const char* v, uint16_t (*out)[2], int* nout)
{
    uint16_t r[KZ3_NET_MAX_RANGES][2];
    int nr = 0;
    const char* p = v;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        if (nr >= KZ3_NET_MAX_RANGES || !isdigit((unsigned char)*p)) return 0;
        char* e;
        unsigned long lo = strtoul(p, &e, 10), hi = lo;
        p = e;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '-') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (!isdigit((unsigned char)*p)) return 0;
            hi = strtoul(p, &e, 10);
            p = e;
        }
        if (lo < 1 || hi > 65535 || lo > hi) return 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != ',') return 0;
        r[nr][0] = (uint16_t)lo; r[nr][1] = (uint16_t)hi; nr++;
    }
    if (nr == 0) return 0;
    memcpy(out, r, sizeof(r));
    *nout = nr;
    return 1;
}

int kz3_net_valid_region_label(const char* s)
{
    size_t chars = 0;
    const unsigned char* p = (const unsigned char*)s;
    if (!s) return 0;
    while (*p) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80) { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1Fu; n = 2; }
        else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0Fu; n = 3; }
        else return 0;                                   /* 4-byte forms (outside the BMP) and stray bytes */
        for (int k = 1; k < n; k++) {
            if ((p[k] & 0xC0) != 0x80) return 0;         /* also stops at the NUL */
            cp = cp << 6 | (p[k] & 0x3Fu);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800)) return 0;   /* overlong */
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || (cp >= 0xD800 && cp <= 0xDFFF) || cp >= 0xFFFE) return 0;
        if (cp == '"') return 0;
        if (++chars > KZ3_NET_LABEL_MAX_CHARS) return 0;
        p += n;
    }
    return chars > 0;
}

size_t kz3_net_xml_escape(const char* s, char* out, size_t cap)
{
    size_t o = 0;
    if (!out || !cap) return 0;
    for (; s && *s; s++) {
        const char* e = NULL;
        switch (*s) {
        case '&': e = "&amp;"; break;
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '"': e = "&quot;"; break;
        case '\'': e = "&apos;"; break;
        default: break;
        }
        size_t el = e ? strlen(e) : 1;
        if (o + el + 1 > cap) { out[0] = 0; return 0; }
        if (e) memcpy(out + o, e, el); else out[o] = *s;
        o += el;
    }
    out[o] = 0;
    return o;
}

/* [server NAME]: 1-32 of letters, digits, space, '-', '_', '.' (after trimming). */
static int valid_server_name(const char* n)
{
    size_t l = strlen(n);
    if (l == 0 || l > KZ3_NET_NAME_MAX) return 0;
    for (size_t i = 0; i < l; i++)
        if (!isalnum((unsigned char)n[i]) && !strchr(" -_.", n[i])) return 0;
    return 1;
}

static int name_eq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* True when the key (or a whole line) contains "server" in any case: such a line is selector-shaped, so a typo or a
 * quote around it must never leave the file looking valid (idea from public PR #39 by Tzeentchnet). */
static int selector_shaped(const char* s)
{
    static const char w[] = "server";
    for (; *s; s++) {
        size_t k = 0;
        while (w[k] && s[k] && tolower((unsigned char)s[k]) == w[k]) k++;
        if (!w[k]) return 1;
    }
    return 0;
}

int kz3_net_parse_config(const char* text, size_t len, Kz3NetConfig* cfg, kz3_net_log_fn log)
{
    int bad = 0, line = 0, in_quote = 0;
    int other_section = 0; /* inside an unknown [section]: ordinary keys stay, a selector never does */
    int sec = -1;          /* -1 = top level, 0.. = cfg->servers[sec], -2 = a server section that was refused */
    size_t i = 0;
    if (!text || !cfg) return 0;
    if (len > KZ3_NET_CONFIG_MAX) { cfg->server_invalid = cfg->config_unsafe = 1; say(log, 0, "file larger than 64 KiB, ignored"); return 1; }
    while (i < len) {
        char buf[512];
        size_t k = 0;
        int too_long = 0;
        line++;
        while (i < len && text[i] != '\n') {
            if (text[i] == 0) { too_long = 2; }
            if (k < sizeof(buf) - 1) buf[k++] = text[i]; else too_long = too_long ? too_long : 1;
            i++;
        }
        i++;   /* the newline */
        if (too_long) { cfg->server_invalid = 1;   /* the discarded bytes may have held a selector */
            say(log, line, too_long == 2 ? "NUL byte, line ignored" : "line too long, ignored"); bad++; continue; }
        buf[k] = 0;
        /* trim, strip CR and comments ('#'/';' outside double quotes) */
        char* s = buf;
        if (line == 1 && !strncmp(s, "\xEF\xBB\xBF", 3)) s += 3;   /* UTF-8 BOM */
        for (char* c = s; *c; c++) {
            if (*c == '"') in_quote = !in_quote;
            else if (!in_quote && (*c == '#' || *c == ';')) { *c = 0; break; }
        }
        in_quote = 0;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        size_t n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
        if (!n) continue;
        if (s[0] == '[') {
            sec = -1;
            other_section = 0;
            char head[8] = {0};
            for (int k = 0; k < 7 && s[k]; k++) head[k] = (char)tolower((unsigned char)s[k]);
            if (strncmp(head, "[server", 7) || (s[7] != ' ' && s[7] != '\t' && s[7] != ']')) {   /* other section */
                other_section = 1;
                cfg->config_unsafe = 1;
                if (selector_shaped(s)) { cfg->server_invalid = 1; say(log, line, "server-shaped section header"); bad++; sec = -2; }
                continue;
            }
            char* close = strchr(s, ']');
            if (!close || close[1]) { cfg->server_invalid = 1; say(log, line, "expected [server NAME]"); bad++; sec = -2; continue; }
            *close = 0;
            char* nm = s + 7;
            while (*nm == ' ' || *nm == '\t') nm++;
            size_t nl = strlen(nm);
            while (nl && (nm[nl - 1] == ' ' || nm[nl - 1] == '\t')) nm[--nl] = 0;
            if (!nl) continue;   /* "[server]" with no name: the top level, as before pass 9 */
            if (!valid_server_name(nm)) {
                cfg->server_invalid = 1;
                say(log, line, "server name must be 1-32 letters, digits, spaces, - _ or ."); bad++; sec = -2; continue;
            }
            int dup = 0;
            for (int k = 0; k < cfg->nservers; k++) if (name_eq(cfg->servers[k].name, nm)) dup = 1;
            if (dup) { cfg->server_invalid = 1; say(log, line, "a server with this name is already listed; section ignored"); bad++; sec = -2; continue; }
            if (cfg->nservers >= KZ3_NET_MAX_SERVERS) { cfg->server_invalid = 1; say(log, line, "more than 8 servers; section ignored"); bad++; sec = -2; continue; }
            Kz3NetServer* sv = &cfg->servers[cfg->nservers];
            memset(sv, 0, sizeof(*sv));
            snprintf(sv->name, sizeof(sv->name), "%s", nm);
            Kz3NetConfig d;
            kz3_net_config_defaults(&d);
            memcpy(sv->ranges, d.ranges, sizeof(sv->ranges));
            sv->nranges = d.nranges;
            sec = cfg->nservers++;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) {
            if (selector_shaped(s)) cfg->server_invalid = 1;
            if (sec != -2 || selector_shaped(s)) { say(log, line, "expected key = value"); bad++; }
            continue;
        }
        char* v = eq + 1;
        *eq = 0;
        n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char* q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
        int selector = selector_shaped(s);
        if (selector && (strcmp(s, "server") || sec != -1 || other_section)) {   /* misspelled, nested or misplaced */
            cfg->server_invalid = 1;
            say(log, line, "unknown or misplaced server selector"); bad++; continue;
        }
        if (sec == -2) continue;   /* other keys of a refused section (already reported once) */
        if (!strcmp(s, "ticket_file") && *v) cfg->ticket_requested = 1;
        if (selector) {
            if (cfg->server_seen) { cfg->server_invalid = 1; say(log, line, "server selector given more than once"); bad++; continue; }
            cfg->server_seen = 1;
        }
        int quoted = 0;
        if (*v == '"') {                         /* "value": the quotes go, what is between them stays */
            size_t vl = strlen(v);
            if (vl < 2 || v[vl - 1] != '"') { if (selector) cfg->server_invalid = 1; say(log, line, "unterminated double quote"); bad++; continue; }
            v[vl - 1] = 0;
            v++;
            quoted = 1;
        }
        char* host = sec >= 0 ? cfg->servers[sec].host : cfg->host;
        size_t host_cap = sec >= 0 ? sizeof(cfg->servers[sec].host) : sizeof(cfg->host);
        char* region = sec >= 0 ? cfg->servers[sec].region : cfg->region;
        size_t region_cap = sec >= 0 ? sizeof(cfg->servers[sec].region) : sizeof(cfg->region);
        if (sec >= 0 && strcmp(s, "host") && strcmp(s, "ports") && strcmp(s, "region")) {
            say(log, line, "a [server] section holds only host, ports and region; ignored");
            bad++;
        } else if (quoted && strcmp(s, "region")) {
            if (selector) cfg->server_invalid = 1;
            say(log, line, "only region takes a quoted value"); bad++;
        } else if (!strcmp(s, "host")) {
            if (*v == 0) { host[0] = 0; continue; }
            if (!valid_host(v)) { say(log, line, "host is not a DNS name or IPv4 address"); bad++; continue; }
            snprintf(host, host_cap, "%s", v);
        } else if (!strcmp(s, "ports")) {
            int ok = sec >= 0 ? parse_ranges(v, cfg->servers[sec].ranges, &cfg->servers[sec].nranges)
                              : parse_ranges(v, cfg->ranges, &cfg->nranges);
            if (!ok) { say(log, line, "ports must be up to 8 of N or N-M (1-65535), comma separated"); bad++; }
        } else if (!strcmp(s, "region")) {
            if (*v == 0) { region[0] = 0; continue; }
            if (!kz3_net_valid_region_label(v)) {
                say(log, line, "region must be 1-24 characters of UTF-8 text (no control characters, no \")"); bad++; continue;
            }
            snprintf(region, region_cap, "%s", v);
        } else if (!strcmp(s, "server")) {
            if (!valid_server_name(v)) { cfg->server_invalid = 1; say(log, line, "server must name a [server NAME] section"); bad++; continue; }
            snprintf(cfg->server, sizeof(cfg->server), "%s", v);
        } else if (!strcmp(s, "online_id")) {
            if (*v == 0) { cfg->online_id[0] = 0; continue; }
            if (!np_ticket_valid_online_id(v)) { say(log, line, "online_id must be 3-16 letters, digits, - or _, starting with a letter"); bad++; continue; }
            snprintf(cfg->online_id, sizeof(cfg->online_id), "%s", v);
        } else if (!strcmp(s, "ticket_file")) {
            size_t vl = strlen(v);
            int ok = vl < sizeof(cfg->ticket_file);
            for (size_t q = 0; ok && q < vl; q++) if ((unsigned char)v[q] < 0x20) ok = 0;
            if (!ok) { say(log, line, "ticket_file must be a path of at most 259 bytes"); bad++; continue; }
            snprintf(cfg->ticket_file, sizeof(cfg->ticket_file), "%s", v);
        } else {
            say(log, line, "unknown key, ignored");
            bad++;
        }
    }
    if (bad) cfg->config_unsafe = 1;
    return bad;
}

int kz3_net_config_select(Kz3NetConfig* cfg, kz3_net_log_fn log)
{
    int pick = -1;
    if (!cfg) return -1;
    if (cfg->server_invalid) {
        if (log) log("online.ini: invalid or ambiguous server selection");
        return -1;
    }
    if ((cfg->ticket_requested || cfg->ticket_file[0] || cfg->online_id[0]) && cfg->config_unsafe) {
        if (log) log("online.ini: a ticket or online identity needs a fully recognised configuration");
        return -1;
    }
    if (cfg->server[0]) {
        for (int k = 0; k < cfg->nservers; k++) if (name_eq(cfg->servers[k].name, cfg->server)) pick = k;
        if (pick < 0) {
            if (log) {
                char m[120];
                snprintf(m, sizeof(m), "online.ini: server = %s names no [server] section", cfg->server);
                log(m);
            }
            return -1;
        }
    } else if (!cfg->host[0] && cfg->nservers > 0) {
        pick = 0;
    }
    if (pick < 0) return 0;   /* the top-level server */
    const Kz3NetServer* sv = &cfg->servers[pick];
    memcpy(cfg->host, sv->host, sizeof(cfg->host));
    memcpy(cfg->ranges, sv->ranges, sizeof(cfg->ranges));
    cfg->nranges = sv->nranges;
    memcpy(cfg->region, sv->region, sizeof(cfg->region));
    return 0;
}

/* The label of the server in use, set once by kz3_net_register when online (read by the Localizer hook). */
static char s_region_label[KZ3_NET_LABEL_MAX_BYTES + 1];

const char* kz3_net_region_label_for_key(const char* key, size_t len)
{
    static const char pre[] = "menu_region_";
    const size_t pl = sizeof(pre) - 1;
    if (!s_region_label[0] || !key || len <= pl || len > pl + 9 || memcmp(key, pre, pl)) return NULL;
    for (size_t k = pl; k < len; k++) if (key[k] < '0' || key[k] > '9') return NULL;
    return s_region_label;
}

static int strcasecmp_ascii(const char* a, const char* b)
{
    for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {}
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int kz3_net_apply(const Kz3NetConfig* cfg, const uint8_t server_ip[4])
{
    int rc = 0;
    ps3_net_policy_set_strict(1);
    if (ps3_net_policy_deny_host(KZ3_NET_TELEMETRY_HOST)) rc = -1;
    if (!cfg || !server_ip || !cfg->host[0]) return rc;
    if (ps3_net_policy_map_host(KZ3_NET_MUIS_HOST, server_ip)) rc = -1;
    /* A stock Medius-compatible server names itself in its own answers (universe DNS, SVO URL): the configured
     * host name and the server's dotted address resolve to the one selected server and to nothing else. */
    char dotted[16];
    snprintf(dotted, sizeof(dotted), "%u.%u.%u.%u", server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    if (ps3_net_policy_map_host(dotted, server_ip)) rc = -1;
    if (strcmp(dotted, cfg->host) && strcasecmp_ascii(cfg->host, KZ3_NET_MUIS_HOST) && valid_host(cfg->host) &&
        ps3_net_policy_map_host(cfg->host, server_ip)) rc = -1;
    for (int i = 0; i < cfg->nranges; i++)
        if (ps3_net_policy_allow(server_ip, cfg->ranges[i][0], cfg->ranges[i][1],
                                 PS3_NET_PROTO_TCP | PS3_NET_PROTO_UDP)) rc = -1;
    return rc;
}

/* ---- the file ---------------------------------------------------------------------------------------------- */

static void log_line(const char* m) { printf("[kz3net] %s\n", m); }

/* The folder online.ini was looked for in (relative ticket_file paths are taken from there). */
static char s_cfg_dir[1024];
static char s_portable_dir[1024];   /* the install folder when portable (shared files then live in <it>/shared) */

static void set_cfg_dir_from(const char* file)
{
    snprintf(s_cfg_dir, sizeof(s_cfg_dir), "%s", file);
    char* a = strrchr(s_cfg_dir, '/');
    char* b = strrchr(s_cfg_dir, '\\');
    char* cut = a > b ? a : b;
    if (cut) cut[1] = 0; else s_cfg_dir[0] = 0;
}

static char* read_small(FILE* f, size_t* len)
{
    char* buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > KZ3_NET_CONFIG_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char*)malloc((size_t)n + 1);
    if (buf) {
        *len = fread(buf, 1, (size_t)n, f);
        if (*len != (size_t)n) { free(buf); buf = NULL; *len = 0; }   /* short read: not a config to trust */
        else buf[*len] = 0;
    }
    fclose(f);
    return buf;
}

#ifdef _WIN32
/* The data folder rule of patch 2037 (keybinds.ini) and tools/release/launch.py data_dir(): the install folder
 * when kz3-install.ini (next to the exe or one level up) says [install] portable = 1, else
 * %APPDATA%\Project Helghast. */
static int dir_portable(const wchar_t* dir)
{
    wchar_t p[1024];
    size_t len = 0;
    int r = 0;
    if (_snwprintf(p, 1024, L"%ls\\kz3-install.ini", dir) < 0) return 0;
    p[1023] = 0;
    char* t = read_small(_wfopen(p, L"rb"), &len);
    if (!t) return 0;
    /* "[install]" then "portable = 1" (case-insensitive, spaces free) */
    int in_install = 0;
    for (char* line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char b[128]; size_t k = 0;
        for (char* q = line; *q && k < sizeof(b) - 1; q++)
            if (*q != ' ' && *q != '\t') b[k++] = (char)tolower((unsigned char)*q);
        b[k] = 0;
        if (b[0] == '[') in_install = !strcmp(b, "[install]");
        else if (in_install && !strcmp(b, "portable=1")) r = 1;
    }
    free(t);
    return r;
}

static FILE* open_config(char* shown, size_t shown_cap)
{
    wchar_t exe[1024], path[1100];
    const char* env = getenv("KZ3_NET_CONFIG");
    if (env && *env) { snprintf(shown, shown_cap, "%s", env); set_cfg_dir_from(env); return fopen(env, "rb"); }
    DWORD n = GetModuleFileNameW(NULL, exe, 1024);
    if (n > 0 && n < 1024) {
        for (int up = 0; up < 2; up++) {
            wchar_t* sl = wcsrchr(exe, L'\\');
            if (!sl) break;
            *sl = 0;
            if (dir_portable(exe)) {
                _snwprintf(path, 1100, L"%ls\\online.ini", exe); path[1099] = 0;
                snprintf(shown, shown_cap, "online.ini in the install folder");
                if (WideCharToMultiByte(CP_UTF8, 0, exe, -1, s_portable_dir, (int)sizeof(s_portable_dir), NULL, NULL) <= 0)
                    s_portable_dir[0] = 0;
                if (WideCharToMultiByte(CP_UTF8, 0, path, -1, s_cfg_dir, (int)sizeof(s_cfg_dir), NULL, NULL) > 0)
                    set_cfg_dir_from(s_cfg_dir);
                return _wfopen(path, L"rb");
            }
        }
    }
    wchar_t appdata[1024];
    n = GetEnvironmentVariableW(L"APPDATA", appdata, 1024);
    if (n > 0 && n < 1000) {
        _snwprintf(path, 1100, L"%ls\\Project Helghast\\online.ini", appdata); path[1099] = 0;
        snprintf(shown, shown_cap, "%%APPDATA%%\\Project Helghast\\online.ini");
        if (WideCharToMultiByte(CP_UTF8, 0, path, -1, s_cfg_dir, (int)sizeof(s_cfg_dir), NULL, NULL) > 0)
            set_cfg_dir_from(s_cfg_dir);
        return _wfopen(path, L"rb");
    }
    snprintf(shown, shown_cap, "online.ini");
    return fopen("online.ini", "rb");
}
#else
static FILE* open_config(char* shown, size_t shown_cap)
{
    const char* env = getenv("KZ3_NET_CONFIG");
    const char* p = (env && *env) ? env : "online.ini";
    snprintf(shown, shown_cap, "%s", p);
    set_cfg_dir_from(p);
    return fopen(p, "rb");
}
#endif

/* The server's IPv4 address (the host resolver may be asked: the configured server is the one allowed name). */
static int resolve_server(const char* host, uint8_t ip[4])
{
    struct addrinfo hints, *res = NULL;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   /* reference-counted; sysNet starts it too */
#endif
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    memcpy(ip, &((const struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return 1;
}

/* UTF-8 path -> FILE* (wide on Windows, so non-ASCII folders work). */
static FILE* fopen_utf8(const char* path)
{
#ifdef _WIN32
    wchar_t w[1300];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1300) <= 0) return NULL;
    return _wfopen(w, L"rb");
#else
    return fopen(path, "rb");
#endif
}

static int path_absolute(const char* p)
{
    return p[0] == '/' || p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':');
}

int kz3_net_load_ticket(const char* path, Kz3NetConfig* cfg, kz3_net_log_fn log)
{
    char full[1300], m[300];
    uint8_t buf[NP_TICKET_FILE_MAX + 1];
    if (!log) log = log_line;
    if (path_absolute(path) || !s_cfg_dir[0]) snprintf(full, sizeof(full), "%s", path);
    else snprintf(full, sizeof(full), "%s%s", s_cfg_dir, path);
    FILE* f = fopen_utf8(full);
    if (!f) { log("ticket_file: cannot open the file; not signed in"); return -1; }
    size_t n = fread(buf, 1, sizeof(buf), f);   /* one byte more than allowed: detects oversized files */
    fclose(f);
    NpTicketInfo t;
    int rc = np_ticket_check(buf, n, &t);
    if (rc == 0 && cfg->online_id[0]) {
        /* online ids are case-insensitive on PSN */
        int same = strlen(cfg->online_id) == strlen(t.online_id);
        for (size_t i = 0; same && cfg->online_id[i]; i++)
            if (tolower((unsigned char)cfg->online_id[i]) != tolower((unsigned char)t.online_id[i])) same = 0;
        if (!same) {
            snprintf(m, sizeof(m), "ticket_file: issued to \"%s\", but online_id is \"%s\"; not signed in",
                     t.online_id, cfg->online_id);
            log(m);
            memset(buf, 0, sizeof(buf));
            return -1;
        }
    }
    if (rc == 0) rc = np_kz3_set_ticket_file(buf, n, NULL);
    memset(buf, 0, sizeof(buf));                /* the copy np_kz3 keeps is the only one */
    if (rc) {
        snprintf(m, sizeof(m), "ticket_file: rejected (%s); not signed in", np_ticket_reason(rc));
        log(m);
        return -1;
    }
    snprintf(cfg->online_id, sizeof(cfg->online_id), "%s", t.online_id);
    uint64_t now = (uint64_t)time(NULL) * 1000u;
    snprintf(m, sizeof(m), "ticket_file: %u bytes, version %u.%u, online id \"%s\", service \"%s\"%s", t.size,
             (unsigned)(t.version >> 28), (unsigned)((t.version >> 24) & 0xF), t.online_id, t.service_id,
             (t.expire_ms && t.expire_ms < now) ? " (expired: the server may refuse it)" : "");
    log(m);
    return 0;
}

/* Setup went wrong: back to offline with every destination refused, so no half-built selection can carry traffic
 * (idea from public PR #39 by Tzeentchnet). */
static void fail_online_setup(void)
{
    ps3_net_set_online(0);
    ps3_net_set_np_signed_in(0);
    ps3_net_policy_reset();
    ps3_net_policy_set_strict(1);
    s_region_label[0] = 0;
}

/* Port-added online features (re-psn, ps3recomp's np_psnr) use the host's own sockets and never pass the guest
 * egress policy; the Medius server entry is for the game's own Medius code only. A PSNR_SERVER that names the
 * selected Medius server (same host text or same IPv4 address) is removed from the environment. Returns 1 if removed. */
int kz3_net_psnr_guard(const uint8_t server_ip[4])
{
    const char* v = getenv("PSNR_SERVER");
    if (!v || !*v) return 0;
    char h[260];
    snprintf(h, sizeof(h), "%s", v);
    char* c = strrchr(h, ':');
    if (c && strchr(h, ':') == c) *c = 0;       /* host:port (a bare IPv6 literal is left whole) */
    uint8_t pip[4];
    int same = 0;
    if (h[0] && valid_host(h) && resolve_server(h, pip) && !memcmp(pip, server_ip, 4)) same = 1;
    if (!same) return 0;
#ifdef _WIN32
    _putenv("PSNR_SERVER=");
#else
    unsetenv("PSNR_SERVER");
#endif
    printf("[kz3net] PSNR_SERVER names the Medius server: removed (port features never use the Medius host)\n");
    return 1;
}

/* ---- the online method files (network pass 17): repsn.ini / community.ini, shared then game ------------------------- */

static Kz3OcSession s_session;
static int s_have_session;

int kz3_net_port_features_allowed(void) { return s_have_session && s_session.port_features; }

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    char b[400];
    snprintf(b, sizeof(b), "%s=%s", k, v ? v : "");
    _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* Returns 0 when no method file exists (the old online.ini flow stands), 1 when a method was applied to cfg, and -1
 * when online must stay off (both methods on, an incomplete choice, or the method is off): never a silent pick. */
static int apply_method_files(Kz3NetConfig* cfg)
{
    Kz3OcConfig oc;
    char shared[1024];
    char m[400];
    s_have_session = 0;
    kz3_oc_defaults(&oc);
    kz3_oc_shared_dir(s_portable_dir[0] ? s_portable_dir : NULL, shared, sizeof(shared));
    if (kz3_oc_load(shared[0] ? shared : NULL, s_cfg_dir[0] ? s_cfg_dir : NULL, &oc, log_line) == 0) return 0;
    kz3_oc_select(&oc, &s_session);
    s_have_session = 1;
    log_line(s_session.log_line);
    if (s_session.player_message[0]) { snprintf(m, sizeof(m), "player message: %s", s_session.player_message); log_line(m); }
    if (s_session.method != KZ3_OC_REPSN && s_session.method != KZ3_OC_COMMUNITY) {
        log_line("online was asked for (KZ3_ONLINE) but no single online method is on: every destination is refused");
        set_env("PSNR_SERVER", NULL);
        return -1;
    }
    if (cfg->host[0] && strcasecmp_ascii(cfg->host, s_session.host))
        log_line("online.ini names another server: ignored, the online method files decide");
    snprintf(cfg->host, sizeof(cfg->host), "%s", s_session.host);
    if (s_session.online_name[0]) snprintf(cfg->online_id, sizeof(cfg->online_id), "%s", s_session.online_name);
    if (s_session.method == KZ3_OC_COMMUNITY) {
        set_env("PSNR_SERVER", NULL);   /* port-added features have no server on a community session */
        log_line("PSNR_SERVER cleared: port-added traffic is off on a community server");
    } else {
        int covered = 0;
        for (int i = 0; i < cfg->nranges; i++)
            if (s_session.port >= cfg->ranges[i][0] && s_session.port <= cfg->ranges[i][1]) covered = 1;
        if (!covered && cfg->nranges < KZ3_NET_MAX_RANGES) {
            cfg->ranges[cfg->nranges][0] = cfg->ranges[cfg->nranges][1] = s_session.port;
            cfg->nranges++;
        }
        snprintf(m, sizeof(m), "%s:%u", s_session.host, (unsigned)s_session.port);
        set_env("PSNR_SERVER", m);
        snprintf(m, sizeof(m), "re-psn: comm_id %s, p2p_port %u, report_rtt %d (the client call is the port glue's)",
                 s_session.comm_id, (unsigned)s_session.p2p_port, s_session.report_rtt);
        log_line(m);
    }
    return 1;
}

void kz3_net_register(void)
{
    if (!ps3_net_online()) return;   /* offline (default): nothing here runs */
    ps3_net_policy_set_strict(1);    /* until a selection is installed, refuse everything */

    Kz3NetConfig cfg;
    char shown[200], m[300];
    size_t len = 0;
    kz3_net_config_defaults(&cfg);
    errno = 0;
    FILE* cf = open_config(shown, sizeof(shown));
    int cf_err = errno, cf_opened = cf != NULL;
    char* text = read_small(cf, &len);
    if (text) {
        int bad = kz3_net_parse_config(text, len, &cfg, log_line);
        snprintf(m, sizeof(m), "config: %s (%d line(s) rejected)", shown, bad);
        free(text);
    } else if (cf_opened || (cf_err != ENOENT && cf_err != 0)) {
        cfg.server_invalid = 1;   /* a file is there but unreadable, oversized or cut short: not "no config" */
        snprintf(m, sizeof(m), "config: %s cannot be read", shown);
    } else {
        snprintf(m, sizeof(m), "config: %s not found", shown);
    }
    log_line(m);
    const char* e = getenv("KZ3_NET_ONLINE_ID");    /* tests; read before the selection so it counts as an identity */
    if (e && *e) {
        if (np_ticket_valid_online_id(e)) snprintf(cfg.online_id, sizeof(cfg.online_id), "%s", e);
        else log_line("KZ3_NET_ONLINE_ID is not a valid online id, ignored");
    }
    if (kz3_net_config_select(&cfg, log_line) != 0) {
        log_line("invalid server selection: online setup abandoned, every destination is refused");
        fail_online_setup();
        return;
    }
    if (cfg.nservers) {
        snprintf(m, sizeof(m), "%d server entr%s in online.ini; using %s", cfg.nservers, cfg.nservers == 1 ? "y" : "ies",
                 cfg.server[0] ? cfg.server : (cfg.host[0] ? "the selected entry" : "none"));
        log_line(m);
    }
    e = getenv("KZ3_NET_SERVER");                   /* tests; set but invalid = no server, never the file's */
    if (e && *e) {
        if (valid_host(e)) snprintf(cfg.host, sizeof(cfg.host), "%s", e);
        else {
            log_line("KZ3_NET_SERVER is not a host name or IPv4 address: every destination is refused");
            fail_online_setup();
            return;
        }
    }
    e = getenv("KZ3_NET_REGION");                   /* tests: the region label, validated like the file's */
    if (e && *e) {
        if (kz3_net_valid_region_label(e)) snprintf(cfg.region, sizeof(cfg.region), "%s", e);
        else log_line("KZ3_NET_REGION is not a valid region label, ignored");
    }
    int method_files = apply_method_files(&cfg);
    if (method_files < 0) { fail_online_setup(); return; }
    memcpy(s_region_label, cfg.region, sizeof(s_region_label));
    if (cfg.region[0]) log_line("region label configured (shown for the server's regions)");
    uint8_t ip[4];
    if (!cfg.host[0] || !resolve_server(cfg.host, ip)) {
        snprintf(m, sizeof(m), "online, but %s: every destination is refused",
                 cfg.host[0] ? "the server name does not resolve" : "no server is configured (host = ...)");
        log_line(m);
        fail_online_setup();
        return;
    }
    if (!(s_have_session && s_session.method == KZ3_OC_REPSN)) kz3_net_psnr_guard(ip);   /* a re-psn session sets PSNR_SERVER itself */
    if (kz3_net_apply(&cfg, ip)) {
        log_line("network policy table full: online setup abandoned, every destination is refused");
        fail_online_setup();
        return;
    }
    snprintf(m, sizeof(m), "online: %s -> %s (%u.%u.%u.%u), %d port range(s); everything else refused",
             KZ3_NET_MUIS_HOST, cfg.host, ip[0], ip[1], ip[2], ip[3], cfg.nranges);
    log_line(m);

    if (cfg.ticket_file[0] && kz3_net_load_ticket(cfg.ticket_file, &cfg, log_line) != 0) { fail_online_setup(); return; }
    if (cfg.online_id[0]) np_kz3_sign_in(cfg.online_id);
    else log_line("no online_id configured: not signed in to NP");

    /* Test only (plan step 5): the loopback capture, never with a non-loopback server. */
    e = getenv("KZ3_NET_CAPTURE");
    if (e && *e && strcmp(e, "0")) {
        if (ip[0] == 127) {   /* MUIS, then the MAS the universe answer points at (pass 6) */
            kz3_net_capture_start(10071, getenv("KZ3_NET_CAPTURE_HELLO"), cfg.online_id);
            kz3_net_capture_start(10075, NULL, cfg.online_id);
            kz3_net_capture_set_region_label(cfg.region);
            kz3_net_capture_start_kind(10070, NULL, NULL, 1);   /* SVO HTTP: region list, nplogin.do */
            kz3_net_capture_start(10078, NULL, cfg.online_id);   /* MLS, from the TicketLogin answer (pass 8) */
            kz3_net_capture_start(10079, NULL, cfg.online_id);   /* DME, from the party join answer 4/0x8E (pass 10) */
        }
        else log_line("KZ3_NET_CAPTURE ignored: the server is not on 127.0.0.0/8");
    }
}

/* The loopback capture is its own file; included here so the exe's source list (port/kz3/CMakeLists.txt) and the
 * host test (tools/net/test_kz3_net.c includes this file) need no change. */
#include "kz3_online_cfg.c"
#include "rt_capture.c"
