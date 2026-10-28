/* kz3_discord: see kz3_discord.h. Sends to the local Discord app only: the activity text below, the process id
 * and the application id. No account data, no network call of ours. */
#include "kz3_discord.h"
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

#ifdef _WIN32
#include <windows.h>

static struct {
    CRITICAL_SECTION cs;
    int kind;             /* 0 menus, 1 campaign, 2 multiplayer */
    char name[64];
    int64_t start;
    unsigned version;
} g_s;
static HANDLE g_thread;
static volatile LONG g_stop;
static volatile LONG g_on;
static int64_t g_session_start;
static char g_client_id[40];
static DWORD g_boot_tick;

static void on_open(const char* guest_path)
{
    if (!g_on) return;
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

extern void (*g_ps3_fs_open_hook)(const char*); /* patch 4021 */

static int pipe_read(HANDLE h, uint8_t* b, DWORD n, DWORD timeout_ms)
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
        if (g_stop) return 0;
    }
    return 1;
}

/* One frame, or 0 = nothing (timeout), 1 = frame in buf, -1 = broken pipe or hostile length. */
static int read_frame(HANDLE h, uint8_t* buf, size_t cap, uint32_t* op, uint32_t* plen, DWORD timeout_ms)
{
    int r = pipe_read(h, buf, 8, timeout_ms);
    if (r <= 0) return r;
    uint32_t o2 = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    uint32_t n2 = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (n2 > KD_MAX_PAYLOAD || n2 + 8 > cap) return -1;
    if (n2 && pipe_read(h, buf + 8, n2, 2000) <= 0) return -1;
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
    (void)arg;
    uint8_t buf[KD_MAX_PAYLOAD / 4 + 8];
    DWORD next_try = 0;
    int first_fail_logged = 0;
    unsigned nonce = 1;
    while (!g_stop) {
        int idx = -1;
        HANDLE h = INVALID_HANDLE_VALUE;
        if ((LONG)(GetTickCount() - next_try) >= 0) {
            h = open_pipe(&idx);
            next_try = GetTickCount() + 30000; /* at most one connect attempt per 30 s */
        }
        if (h == INVALID_HANDLE_VALUE) {
            if (!first_fail_logged) { fprintf(stderr, "[discord] presence on, not running (retry every 30 s)\n"); first_fail_logged = 1; }
            for (int i = 0; i < 20 && !g_stop; i++) Sleep(100);
            continue;
        }
        char json[1024];
        size_t jl = kd_build_handshake(json, sizeof json, g_client_id);
        uint32_t op = 0, pl = 0;
        int ok = jl && send_frame(h, KD_OP_HANDSHAKE, json, jl) && read_frame(h, buf, sizeof buf, &op, &pl, 3000) == 1 && op != KD_OP_CLOSE;
        if (!ok) { CloseHandle(h); fprintf(stderr, "[discord] discord-ipc-%d did not accept the handshake\n", idx); continue; }
        fprintf(stderr, "[discord] presence on, connected to discord-ipc-%d\n", idx);
        first_fail_logged = 0;
        unsigned sent_ver = (unsigned)-1;
        DWORD last_send = 0;
        while (!g_stop) {
            unsigned ver;
            current_activity(json, sizeof json, nonce, &ver);
            /* resend on change, and every 60 s so a Discord that lost it gets it back */
            if (json[0] && (ver != sent_ver || GetTickCount() - last_send > 60000)) {
                if (!send_frame(h, KD_OP_FRAME, json, strlen(json))) break;
                nonce++; sent_ver = ver; last_send = GetTickCount();
            }
            int r = read_frame(h, buf, sizeof buf, &op, &pl, 200); /* replies are read so the pipe never fills */
            if (r < 0) break;
            if (r == 1) {
                if (op == KD_OP_CLOSE) break;
                if (op == KD_OP_PING && !send_frame(h, KD_OP_PONG, (const char*)buf + 8, pl)) break;
            }
        }
        if (g_stop) {
            jl = kd_build_clear(json, sizeof json, GetCurrentProcessId(), nonce);
            if (jl) send_frame(h, KD_OP_FRAME, json, jl);
        }
        CloseHandle(h);
        fprintf(stderr, "[discord] disconnected\n");
        next_try = GetTickCount() + 30000;
    }
    return 0;
}

void kz3_discord_start(void)
{
    const char* on = getenv("KZ3_DISCORD_PRESENCE");
    if (!on || !*on || !strcmp(on, "0")) { fprintf(stderr, "[discord] off\n"); return; }
    const char* id = getenv("KZ3_DISCORD_CLIENT_ID");
    snprintf(g_client_id, sizeof g_client_id, "%s", (id && *id) ? id : KD_DEFAULT_CLIENT_ID);
    if (!id_ok(g_client_id)) { fprintf(stderr, "[discord] off (discord_client_id is not a number)\n"); return; }
    InitializeCriticalSection(&g_s.cs);
    g_session_start = (int64_t)time(NULL);
    g_boot_tick = GetTickCount();
    g_on = 1;
    g_ps3_fs_open_hook = on_open;
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!g_thread) { g_on = 0; g_ps3_fs_open_hook = NULL; fprintf(stderr, "[discord] off (thread failed)\n"); }
}

void kz3_discord_stop(void)
{
    if (!g_thread) return;
    g_on = 0;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, 1500);
    CloseHandle(g_thread);
    g_thread = NULL;
}
#else
void kz3_discord_start(void) { fprintf(stderr, "[discord] off\n"); }
void kz3_discord_stop(void) {}
#endif
