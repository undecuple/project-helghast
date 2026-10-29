/* kz3_discord: see kz3_discord.h. Sends to the local Discord app only: the activity text below, the process id
 * and the application id. No account data, no network call of ours. */
#include "kz3_discord.h"
#define KZ3_TR_IMPLEMENTATION   /* network pass 45: the port UI text table lives in this always-linked file */
#include "kz3_tr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KD_DEFAULT_CLIENT_ID "1562976444953526292"

size_t kd_frame_encode(uint32_t op, const char* json, size_t json_len, uint8_t* out, size_t cap)
{
    if (json_len > KD_MAX_PAYLOAD || cap < 8 + json_len) return 0;
    for (int i = 0; i < 4; i++) { out[i] = (uint8_t)(op >> (8 * i)); out[4 + i] = (uint8_t)((uint32_t)json_len >> (8 * i)); }
    if (json_len) memcpy(out + 8, json, json_len);
    return 8 + json_len;
}

int kd_frame_decode(const uint8_t* buf, size_t len, uint32_t* op, const uint8_t** payload, uint32_t* plen)
{
    if (len < 8) return 0;
    uint32_t o = 0, n = 0;
    for (int i = 0; i < 4; i++) { o |= (uint32_t)buf[i] << (8 * i); n |= (uint32_t)buf[4 + i] << (8 * i); }
    if (n > KD_MAX_PAYLOAD) return -1; /* a hostile or corrupt length: the caller drops the connection */
    if (len < 8 + (size_t)n) return 0;
    *op = o; *payload = buf + 8; *plen = n;
    return (int)(8 + n);
}

static int id_ok(const char* s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* JSON string body: quotes, backslashes and control bytes escaped; non-ASCII bytes pass through (UTF-8). */
static int esc(char* o, size_t cap, size_t* pos, const char* s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        int n;
        if (c == '"' || c == '\\') n = snprintf(tmp, sizeof tmp, "\\%c", c);
        else if (c < 0x20) n = snprintf(tmp, sizeof tmp, "\\u%04x", c);
        else { tmp[0] = (char)c; tmp[1] = 0; n = 1; }
        if (*pos + (size_t)n >= cap) return 0;
        memcpy(o + *pos, tmp, (size_t)n);
        *pos += (size_t)n;
    }
    return 1;
}

size_t kd_build_handshake(char* out, size_t cap, const char* client_id)
{
    if (!id_ok(client_id)) return 0;
    int n = snprintf(out, cap, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static size_t put(char* out, size_t cap, size_t pos, const char* s)
{
    size_t n = strlen(s);
    if (pos + n >= cap) return 0;
    memcpy(out + pos, s, n + 1);
    return pos + n;
}

size_t kd_build_activity(char* out, size_t cap, uint32_t pid, unsigned nonce, const kd_activity_t* a)
{
    char b[96];
    size_t p = 0;
    snprintf(b, sizeof b, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":{", (unsigned)pid);
    if (!(p = put(out, cap, p, b))) return 0;
    int first = 1;
    if (a->details) {
        if (!(p = put(out, cap, p, "\"details\":\""))) return 0;
        if (!esc(out, cap, &p, a->details)) return 0;
        if (!(p = put(out, cap, p, "\""))) return 0;
        first = 0;
    }
    if (a->state) {
        if (!(p = put(out, cap, p, first ? "\"state\":\"" : ",\"state\":\""))) return 0;
        if (!esc(out, cap, &p, a->state)) return 0;
        if (!(p = put(out, cap, p, "\""))) return 0;
        first = 0;
    }
    if (a->start > 0) {
        snprintf(b, sizeof b, "%s\"timestamps\":{\"start\":%lld}", first ? "" : ",", (long long)a->start);
        if (!(p = put(out, cap, p, b))) return 0;
        first = 0;
    }
    if (!(p = put(out, cap, p, first ? "\"assets\":{\"large_image\":\"kz3\",\"large_text\":\"Project Helghast\"}"
                                     : ",\"assets\":{\"large_image\":\"kz3\",\"large_text\":\"Project Helghast\"}"))) return 0;
    snprintf(b, sizeof b, "}},\"nonce\":\"%u\"}", nonce);
    return put(out, cap, p, b);
}

size_t kd_build_clear(char* out, size_t cap, uint32_t pid, unsigned nonce)
{
    int n = snprintf(out, cap, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u},\"nonce\":\"%u\"}", (unsigned)pid, nonce);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* ASSUMPTION: the English chapter names (directory order = chapter order, from the level-select gate in
 * the RE notes s.37); mission_0 is the prologue. Not read from the game's text tables. */
static const struct { const char* dir; const char* name; } kd_chapters[] = {
    {"mission_0", "Prologue"},
    {"flooded_pyrrhus", "Flooded Pyrrhus"},
    {"pyrrhus_outskirts", "Pyrrhus Outskirts"},
    {"jungle", "The Jungle"},
    {"frozen_shores", "Frozen Shores"},
    {"stahl_arms_research", "Stahl Arms Research"},
    {"scrapyard", "Scrapyard"},
    {"escape_from_helghan", "Escape from Helghan"},
    {"space_station", "Space Station"},
};

const char* kd_chapter_name(const char* dir)
{
    for (size_t i = 0; i < sizeof kd_chapters / sizeof kd_chapters[0]; i++)
        if (!strcmp(dir, kd_chapters[i].dir)) return kd_chapters[i].name;
    return NULL;
}

int kd_classify_path(const char* path, int* kind, char* name, size_t cap)
{
    static const char sp[] = "levels.single_player.", mp[] = "levels.multi_player.", ld[] = ".leveldescription_optimized.core";
    if (!path || !cap) return 0;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t bl = strlen(base);
    if (bl < sizeof ld || strcmp(base + bl - (sizeof ld - 1), ld)) return 0; /* the gate lump only (the RE notes s.37) */
    const char* d;
    if (!strncmp(base, sp, sizeof sp - 1)) { d = base + sizeof sp - 1; *kind = 1; }
    else if (!strncmp(base, mp, sizeof mp - 1)) { d = base + sizeof mp - 1; *kind = 2; }
    else return 0;
    size_t dl = bl - (sizeof ld - 1) - (size_t)(d - base);
    if (dl == 0 || dl >= 64) return 0;
    char dir[64];
    memcpy(dir, d, dl); dir[dl] = 0;
    if (strchr(dir, '.')) return 0; /* a section or sub-lump, not the level's own description */
    if (*kind == 2) { snprintf(name, cap, "Multiplayer"); return 1; }
    const char* c = kd_chapter_name(dir);
    snprintf(name, cap, "%s", c ? c : "Campaign");
    return 1;
}

/* ---- network pass 42: the Options row (pure; host test tools/net/test_discord.c) ---- */

int kd_parse_onoff(const char* s)
{
    static const char* on[] = {"1", "on", "true", "yes"}, *off[] = {"0", "off", "false", "no"};
    char b[8];
    size_t n = s ? strlen(s) : 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    if (n == 0 || n >= sizeof b) return -1;
    for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)s[i]);
    b[n] = 0;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(b, on[i])) return 1;
        if (!strcmp(b, off[i])) return 0;
    }
    return -1;
}

/* The first non-empty `key = value` of the ini text (kz3_ini.h's rules: any section, case-insensitive key, ';'/'#'
 * comments, optional double quotes), or "" when absent. */
static void kd_ini_value(const char* text, const char* key, char* out, size_t cap)
{
    const size_t kl = strlen(key);
    const char* p = text;
    out[0] = 0;
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

int kd_initial(const char* env, const char* ini_text, int* env_forced)
{
    char v[16];
    int file = 0, e;
    if (ini_text) { kd_ini_value(ini_text, "discord_presence", v, sizeof v); file = kd_parse_onoff(v) == 1; }
    *env_forced = 0;
    if (!env || !*env) return file;
    e = kd_parse_onoff(env);
    if (e < 0) e = 1;   /* kz3_discord_start's old rule: any value but "0" is on */
    if (e != file) *env_forced = 1;
    return e;
}

int kd_ini_upsert(const char* in, char* out, size_t cap, const char* key, const char* val)
{
    const size_t kl = strlen(key), il = strlen(in), vl = strlen(val);
    const char* p = in;
    const char* line_v = NULL; const char* line_e = NULL;   /* the key's first line: from after '=' to end of line */
    const char* sec_e = NULL;                               /* just after the [game] header line */
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        const char* nx = eol;
        if (*nx == '\r') nx++;
        if (*nx == '\n') nx++;
        const char* s = p;
        while (s < eol && (*s == ' ' || *s == '\t')) s++;
        if (s < eol && *s == '[') {
            if (!sec_e && (size_t)(eol - s) >= 6 && !strncmp(s, "[game]", 6)) sec_e = nx;
        } else if (!line_v && s < eol && *s != ';' && *s != '#') {
            const char* eq = s;
            while (eq < eol && *eq != '=') eq++;
            const char* ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if (eq < eol && (size_t)(ke - s) == kl) {
                size_t i = 0;
                while (i < kl && tolower((unsigned char)s[i]) == tolower((unsigned char)key[i])) i++;
                if (i == kl) { line_v = eq + 1; line_e = eol; }
            }
        }
        p = nx;
    }
    size_t o = 0;
#define KD_PUT(ptr, len) do { if (o + (len) + 1 > cap) return -1; memcpy(out + o, (ptr), (len)); o += (len); } while (0)
    if (line_v) {
        KD_PUT(in, (size_t)(line_v - in));
        KD_PUT(" ", 1);
        KD_PUT(val, vl);
        KD_PUT(line_e, il - (size_t)(line_e - in));
    } else if (sec_e) {
        KD_PUT(in, (size_t)(sec_e - in));
        if (sec_e > in && sec_e[-1] != '\n' && sec_e[-1] != '\r') KD_PUT("\n", 1);
        KD_PUT(key, kl); KD_PUT(" = ", 3); KD_PUT(val, vl); KD_PUT("\n", 1);
        KD_PUT(sec_e, il - (size_t)(sec_e - in));
    } else {
        KD_PUT(in, il);
        if (il && in[il - 1] != '\n') KD_PUT("\n", 1);
        if (il) KD_PUT("\n", 1);
        KD_PUT("[game]\n", 7);
        KD_PUT(key, kl); KD_PUT(" = ", 3); KD_PUT(val, vl); KD_PUT("\n", 1);
    }
#undef KD_PUT
    out[o] = 0;
    return (int)o;
}

int kd_cmd_parse(const char* cmd, int running, int* want, char* reply, size_t cap)
{
    if (!cmd || strncmp(cmd, "discord", 7) || (cmd[7] && cmd[7] != ' ')) return 0;
    int v = cmd[7] ? kd_parse_onoff(cmd + 8) : -1;
    if (v < 0) { snprintf(reply, cap, "%s", running ? "on" : "off"); return 1; }
    *want = v;
    snprintf(reply, cap, "%s", v ? "on" : "off");
    return 2;
}

/* The row, the help text, the Lua. The values are the bridge's words; the labels the game's own On/Off keys, in the
 * game's order (Off first, as on Crosshair / HUD / Subtitles / Hints). */
static const char kd_row[] =
    "<select id=\"slc_kz3_discord\" label=\"@tr{discord.label|Discord status}\" onchange=\"script(settingscript,KZ3ChangeDiscord)\"\n"
    "\t\t\t\tdefault-value=\"off\">\n"
    "\t\t\t\t<selectitem value=\"off\">@key{menu_value_off}</selectitem>\n"
    "\t\t\t\t<selectitem value=\"on\">@key{menu_value_on}</selectitem>\n"
    "\t\t\t</select>\n\n\t\t\t";
static const char kd_tip[] =
    "\t\t\tslc_kz3_discord = \"@trl{discord.tip|Shows \"Playing Project Helghast\" and your chapter on your Discord profile through the "
    "Discord app on this PC; nothing is sent when it is off.}\",\n";
/* network pass 45: the page also reports the game's language (kz3_tr.h) for the next port page it opens */
static const char kd_on[] = "\t\t\tgPlayerProfile:SetTimeZone(\"kb:lang \"..gPlayerProfile:GetLanguage())\n\t\t\tKZ3DiscordShow()\n";
static const char kd_reset[] = "\t\t\tKZ3DiscordCmd(\"off\")\n";
static const char kd_lua[] =
    "\n\t\tgKz3Dc = 0\n"
    "\t\tfunction KZ3DiscordCmd(c)\n"
    "\t\t\tgPlayerProfile:SetTimeZone(\"kb:discord \"..c)\n"
    "\t\t\treturn gPlayerProfile:GetTimeZone()\n"
    "\t\tend\n\n"
    "\t\tfunction KZ3DiscordShow()\n"
    "\t\t\tgKz3Dc = 0\n"
    "\t\t\tgRoot:GetTag(\"slc_kz3_discord\"):SetValue(KZ3DiscordCmd(\"state\"))\n"
    "\t\t\tgKz3Dc = 1\n"
    "\t\tend\n\n"
    "\t\tfunction KZ3ChangeDiscord(e)\n"
    "\t\t\te:SetHandled(true)\n"
    "\t\t\tif (gKz3Dc == 1) then KZ3DiscordCmd(e:GetSender():GetValue()) end\n"
    "\t\tend\n";

/* Position just after the first `needle` at or after `from`, or 0 when absent. */
static size_t kd_after(const char* in, size_t n, size_t from, const char* needle)
{
    size_t l = strlen(needle);
    for (size_t i = from; i + l <= n; i++)
        if (!memcmp(in + i, needle, l)) return i + l;
    return 0;
}

char* kd_patch_display_gum(const char* in, size_t n, size_t* out_n)
{
    if (!in || kd_after(in, n, 0, "slc_kz3_discord")) return NULL;   /* already there: leave it */
    size_t gamma = kd_after(in, n, 0, "<listitem id=\"lst_gamma_screen\"");
    size_t tip = kd_after(in, n, 0, "gKz3Tip = {");
    size_t on = kd_after(in, n, 0, "function OnPageOn()");
    size_t rst = kd_after(in, n, 0, "function KZ3ResetDisplay()");
    size_t end = rst ? kd_after(in, n, rst, "]]>") : 0;
    if (!gamma || !tip || !on || !rst || !end) return NULL;
    gamma -= strlen("<listitem id=\"lst_gamma_screen\"");
    end -= 3;
    while (end > rst && (in[end - 1] == '\n' || in[end - 1] == '\r')) end--;   /* the Lua goes before the script's last newline */
    /* each insert after the end of its anchor's line */
    size_t pos[5] = { gamma, tip, on, rst, end };
    /* network pass 45: the row label and the help text in the game's language (kz3_tr.h markers) */
    char* row = NULL;
    char* tipt = NULL;
    kz3_tr_render(kd_row, sizeof kd_row - 1, &row);
    kz3_tr_render(kd_tip, sizeof kd_tip - 1, &tipt);
    if (!row || !tipt) { free(row); free(tipt); return NULL; }
    const char* txt[5] = { row, tipt, kd_on, kd_reset, kd_lua };
    for (int i = 1; i < 4; i++) {
        while (pos[i] < n && in[pos[i]] != '\n') pos[i]++;
        if (pos[i] >= n) { free(row); free(tipt); return NULL; }
        pos[i]++;
    }
    /* the gaps must be in file order (row, tip table, OnPageOn, reset, end) */
    int order[5] = { 0, 1, 2, 3, 4 };
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (pos[order[j]] < pos[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    size_t total = n + 1;
    for (int i = 0; i < 5; i++) total += strlen(txt[i]);
    char* o = (char*)malloc(total);
    if (!o) { free(row); free(tipt); return NULL; }
    size_t w = 0, r = 0;
    for (int k = 0; k < 5; k++) {
        int i = order[k];
        memcpy(o + w, in + r, pos[i] - r); w += pos[i] - r; r = pos[i];
        size_t l = strlen(txt[i]);
        memcpy(o + w, txt[i], l); w += l;
    }
    memcpy(o + w, in + r, n - r); w += n - r;
    o[w] = 0;
    *out_n = w;
    free(row);
    free(tipt);
    return o;
}

#ifdef _WIN32
#include <windows.h>

static struct {
    CRITICAL_SECTION cs;
    int kind;             /* 0 menus, 1 campaign, 2 multiplayer */
    char name[64];
    int64_t start;
    unsigned version;
} g_s;

/* One per presence thread: the thread reads `stop` and frees the block when it ends; the controller sets `stop` once
 * and never touches the block again (a thread that outlives its stop wait frees it on its own). */
typedef struct { volatile LONG stop; uint8_t buf[KD_MAX_PAYLOAD / 4 + 8]; } kd_worker_t;

static CRITICAL_SECTION g_ctl;   /* kz3_discord_set / stop: one start or stop at a time */
static int g_inited;
static HANDLE g_thread;          /* the running presence thread, NULL when off */
static kd_worker_t* g_w;
static int64_t g_session_start;
static char g_client_id[40];
static int g_id_ok;
static DWORD g_boot_tick;
static char g_ini[1100];         /* <exe dir>\kz3.ini */
static int g_env_forced;

/* The chapter is tracked whether the presence is on or not (local state only, nothing leaves the process), so turning
 * the row on in a level shows that level's chapter at once. */
static void on_open(const char* guest_path)
{
    /* The first seconds of a run preload lumps (the RE notes s.37: the timeline ignores t < 20 s too). */
    if (GetTickCount() - g_boot_tick < 20000) return;
    int kind; char name[64];
    if (!kd_classify_path(guest_path, &kind, name, sizeof name)) return;
    EnterCriticalSection(&g_s.cs);
    if (kind != g_s.kind || strcmp(name, g_s.name)) {
        g_s.kind = kind; snprintf(g_s.name, sizeof g_s.name, "%s", name);
        g_s.start = (int64_t)time(NULL);
        g_s.version++;
    }
    LeaveCriticalSection(&g_s.cs);
}

extern void (*g_ps3_fs_open_hook)(const char*);                 /* patch 4021 */
extern int (*g_pad_kb_ext_cmd)(const char* cmd, char* out, size_t n);   /* patch 4025 (cellPad.c pad_kb_ui_cmd) */
extern const char* (*g_pad_kb_tr)(const char* key, const char* en);   /* patch 4027: the Keyboard page's text */

static int pipe_read(HANDLE h, uint8_t* b, DWORD n, DWORD timeout_ms, const kd_worker_t* w)
{
    DWORD got = 0, waited = 0;
    while (got < n) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail) {
            DWORD r = 0;
            if (!ReadFile(h, b + got, avail < n - got ? avail : n - got, &r, NULL) || !r) return -1;
            got += r;
            continue;
        }
        if (waited >= timeout_ms) return 0;
        Sleep(20); waited += 20;
        if (w->stop) return 0;
    }
    return 1;
}

/* One frame, or 0 = nothing (timeout), 1 = frame in buf, -1 = broken pipe or hostile length. */
static int read_frame(HANDLE h, uint8_t* buf, size_t cap, uint32_t* op, uint32_t* plen, DWORD timeout_ms, const kd_worker_t* w)
{
    int r = pipe_read(h, buf, 8, timeout_ms, w);
    if (r <= 0) return r;
    uint32_t o2 = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    uint32_t n2 = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (n2 > KD_MAX_PAYLOAD || n2 + 8 > cap) return -1;
    if (n2 && pipe_read(h, buf + 8, n2, 2000, w) <= 0) return -1;
    *op = o2; *plen = n2;
    return 1;
}

static int send_frame(HANDLE h, uint32_t op, const char* json, size_t len)
{
    uint8_t b[2048];
    size_t n = kd_frame_encode(op, json, len, b, sizeof b);
    DWORD w = 0;
    return n && WriteFile(h, b, (DWORD)n, &w, NULL) && w == n;
}

static HANDLE open_pipe(int* idx)
{
    char path[48];
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { *idx = i; return h; }
    }
    return INVALID_HANDLE_VALUE;
}

static void current_activity(char* json, size_t cap, unsigned nonce, unsigned* ver)
{
    kd_activity_t a;
    char name[64];
    int kind;
    EnterCriticalSection(&g_s.cs);
    kind = g_s.kind; snprintf(name, sizeof name, "%s", g_s.name);
    a.start = g_s.kind ? g_s.start : g_session_start;
    *ver = g_s.version;
    LeaveCriticalSection(&g_s.cs);
    a.details = kind ? name : "In the menus";
    a.state = kind == 1 ? "Campaign" : kind == 2 ? "Online" : NULL;
    if (kind == 2) { a.details = "Multiplayer"; a.state = NULL; }
    if (!kd_build_activity(json, cap, GetCurrentProcessId(), nonce, &a)) json[0] = 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    kd_worker_t* w = (kd_worker_t*)arg;
    uint8_t* buf = w->buf;   /* per thread: one that outlived its stop wait may still be reading */
    DWORD next_try = 0;
    int first_fail_logged = 0;
    unsigned nonce = 1;
    while (!w->stop) {
        int idx = -1;
        HANDLE h = INVALID_HANDLE_VALUE;
        if ((LONG)(GetTickCount() - next_try) >= 0) {
            h = open_pipe(&idx);
            next_try = GetTickCount() + 30000; /* at most one connect attempt per 30 s */
        }
        if (h == INVALID_HANDLE_VALUE) {
            if (!first_fail_logged) { fprintf(stderr, "[discord] presence on, Discord not running (retry every 30 s)\n"); first_fail_logged = 1; }
            for (int i = 0; i < 20 && !w->stop; i++) Sleep(100);
            continue;
        }
        char json[1024];
        size_t jl = kd_build_handshake(json, sizeof json, g_client_id);
        uint32_t op = 0, pl = 0;
        int ok = jl && send_frame(h, KD_OP_HANDSHAKE, json, jl) && read_frame(h, buf, sizeof w->buf, &op, &pl, 3000, w) == 1 && op != KD_OP_CLOSE;
        if (!ok) { CloseHandle(h); if (!w->stop) fprintf(stderr, "[discord] discord-ipc-%d did not accept the handshake\n", idx); continue; }
        fprintf(stderr, "[discord] presence on, connected to discord-ipc-%d\n", idx);
        first_fail_logged = 0;
        unsigned sent_ver = (unsigned)-1;
        DWORD last_send = 0;
        while (!w->stop) {
            unsigned ver;
            current_activity(json, sizeof json, nonce, &ver);
            /* resend on change, and every 60 s so a Discord that lost it gets it back */
            if (json[0] && (ver != sent_ver || GetTickCount() - last_send > 60000)) {
                if (!send_frame(h, KD_OP_FRAME, json, strlen(json))) break;
                nonce++; sent_ver = ver; last_send = GetTickCount();
            }
            int r = read_frame(h, buf, sizeof w->buf, &op, &pl, 200, w); /* replies are read so the pipe never fills */
            if (r < 0) break;
            if (r == 1) {
                if (op == KD_OP_CLOSE) break;
                if (op == KD_OP_PING && !send_frame(h, KD_OP_PONG, (const char*)buf + 8, pl)) break;
            }
        }
        int cleared = 0;
        if (w->stop) {   /* off or exit: SET_ACTIVITY without an activity clears it, then the pipe is closed */
            jl = kd_build_clear(json, sizeof json, GetCurrentProcessId(), nonce);
            cleared = jl && send_frame(h, KD_OP_FRAME, json, jl);
        }
        CloseHandle(h);
        fprintf(stderr, "[discord] %sdisconnected\n", cleared ? "activity cleared, " : "");
        next_try = GetTickCount() + 30000;
    }
    free(w);
    return 0;
}

static void kd_start_locked(void)
{
    if (g_thread || !g_id_ok) return;
    kd_worker_t* w = (kd_worker_t*)calloc(1, sizeof *w);
    HANDLE t = w ? CreateThread(NULL, 0, worker, w, 0, NULL) : NULL;
    if (!t) { free(w); fprintf(stderr, "[discord] off (thread failed)\n"); return; }
    g_w = w;
    g_thread = t;
}

static void kd_stop_locked(DWORD wait_ms)
{
    if (!g_thread) return;
    InterlockedExchange(&g_w->stop, 1);   /* the thread frees g_w: not touched after this */
    if (WaitForSingleObject(g_thread, wait_ms) != WAIT_OBJECT_0)
        fprintf(stderr, "[discord] presence thread still busy after %lu ms; it ends on its own\n", (unsigned long)wait_ms);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_w = NULL;
}

/* kz3.ini discord_presence = 0/1, every other line kept (tmp file, then replace). */
static void kd_save(int on)
{
    static char in[65536], out[65536 + 128];
    char tmp[1110];
    FILE* f = fopen(g_ini, "rb");
    size_t n = 0;
    if (f) { n = fread(in, 1, sizeof in - 1, f); fclose(f); }
    in[n] = 0;
    if (n == sizeof in - 1 || kd_ini_upsert(in, out, sizeof out, "discord_presence", on ? "1" : "0") < 0) {
        fprintf(stderr, "[discord] kz3.ini too large to update: discord_presence not saved\n");
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini);
    f = fopen(tmp, "wb");
    size_t len = strlen(out);
    int ok = f && fwrite(out, 1, len, f) == len;
    if (f) ok = (fclose(f) == 0) && ok;
    ok = ok && MoveFileExA(tmp, g_ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    fprintf(stderr, "[discord] kz3.ini discord_presence = %d %s%s\n", on, ok ? "saved" : "NOT saved",
            ok && g_env_forced ? " (KZ3_DISCORD_PRESENCE still wins at the next start while it is set)" : "");
}

void kz3_discord_set(int on, int save)
{
    if (!g_inited) return;
    EnterCriticalSection(&g_ctl);
    if (on) {
        kd_start_locked();
        fprintf(stderr, "[discord] on (Options): %s\n", !g_id_ok ? "NOT started, discord_client_id is not a number"
                                                       : g_thread ? "presence started" : "NOT started");
    } else {
        int was = g_thread != NULL;
        kd_stop_locked(2500);
        fprintf(stderr, "[discord] off (Options)%s\n", was ? ": presence stopped" : "");
    }
    if (save) kd_save(on);
    LeaveCriticalSection(&g_ctl);
}

int kz3_discord_running(void)
{
    if (!g_inited) return 0;
    EnterCriticalSection(&g_ctl);
    int r = g_thread != NULL;
    LeaveCriticalSection(&g_ctl);
    return r;
}

/* The row's bridge command ("kb:discord [state|on|off]", patch 4025): a set applies and saves at once. */
static int kd_ext_cmd(const char* cmd, char* out, size_t n)
{
    if (kz3_tr_ext_cmd(cmd, out, n)) return 1;   /* network pass 45: "lang <id>" from the port's pages */
    int want = 0, running = kz3_discord_running();
    int r = kd_cmd_parse(cmd, running, &want, out, n);
    if (r == 0) return 0;
    if (r == 2) {
        if (want != running) kz3_discord_set(want, 1);
        snprintf(out, n, "%s", kz3_discord_running() ? "on" : "off");
    }
    return 1;
}

void kz3_discord_start(void)
{
    if (g_inited) return;
    const char* id = getenv("KZ3_DISCORD_CLIENT_ID");
    snprintf(g_client_id, sizeof g_client_id, "%s", (id && *id) ? id : KD_DEFAULT_CLIENT_ID);
    g_id_ok = id_ok(g_client_id);
    InitializeCriticalSection(&g_s.cs);
    InitializeCriticalSection(&g_ctl);
    g_session_start = (int64_t)time(NULL);
    g_boot_tick = GetTickCount();

    char exe[1024];
    DWORD m = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (m == 0 || m >= sizeof exe) exe[0] = 0;
    char* sl = strrchr(exe, '\\');
    char* s2 = strrchr(exe, '/');
    if (s2 > sl) sl = s2;
    if (sl) *sl = 0;
    snprintf(g_ini, sizeof g_ini, "%s/kz3.ini", sl ? exe : ".");
    static char text[65536];
    FILE* f = fopen(g_ini, "rb");
    size_t n = 0;
    if (f) { n = fread(text, 1, sizeof text - 1, f); fclose(f); }
    text[n] = 0;
    const char* env = getenv("KZ3_DISCORD_PRESENCE");
    int on = kd_initial(env, f ? text : NULL, &g_env_forced);

    g_ps3_fs_open_hook = on_open;
    g_pad_kb_ext_cmd = kd_ext_cmd;
    g_pad_kb_tr = kz3_tr;                  /* network pass 45 (patch 4027) */
    g_inited = 1;
    if (g_env_forced)
        fprintf(stderr, "[discord] KZ3_DISCORD_PRESENCE=%s wins over kz3.ini discord_presence for this run\n", env);
    if (!on) { fprintf(stderr, "[discord] off (Options > Display > Discord status turns it on)\n"); return; }
    if (!g_id_ok) { fprintf(stderr, "[discord] off (discord_client_id is not a number)\n"); return; }
    EnterCriticalSection(&g_ctl);
    kd_start_locked();
    LeaveCriticalSection(&g_ctl);
    fprintf(stderr, "[discord] on (%s)\n", g_env_forced ? "KZ3_DISCORD_PRESENCE" : "kz3.ini discord_presence = 1");
}

void kz3_discord_stop(void)
{
    if (!g_inited) return;
    EnterCriticalSection(&g_ctl);
    kd_stop_locked(1500);
    LeaveCriticalSection(&g_ctl);
}
#else
void kz3_discord_start(void) { fprintf(stderr, "[discord] off\n"); }
void kz3_discord_set(int on, int save) { (void)on; (void)save; }
int kz3_discord_running(void) { return 0; }
void kz3_discord_stop(void) {}
#endif
