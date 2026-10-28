/* kz3_log: the one logger of the Killzone 3 port and its runtime. See kz3_log.h for the contract.
 * Portable C11: MinGW-w64 (Windows API) and Linux (POSIX). Notes: the RE notes section 29. */
#define KZ3_LOG_IMPLEMENTATION 1
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif
#include "kz3_log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>   /* MINIDUMP_EXCEPTION_INFORMATION: 4-byte packed in the SDK, keep the real type */
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <time.h>
#  include <unistd.h>
#endif

#define TAG_MAX     32      /* tag bytes kept per override / per thread */
#define OVR_MAX     64      /* per-tag overrides */
#define RING_W      400     /* bytes kept per ring line (the file gets the full line) */
#define ONCE_SLOTS  4096
#define PATH_MAX_   520

/* ------------------------------------------------------------------------------------------------ platform */
#ifdef _WIN32
typedef HANDLE os_fd;
#  define OS_BADFD INVALID_HANDLE_VALUE
static os_fd os_create_new(const char* p)
{
    return CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
}
static int  os_exists_err(void) { return GetLastError() == ERROR_FILE_EXISTS; }
static void os_write(os_fd fd, const char* b, size_t n)
{
    while (n) { DWORD w = 0; if (!WriteFile(fd, b, (DWORD)n, &w, NULL) || !w) return; b += w; n -= w; }
}
static void os_close(os_fd fd) { CloseHandle(fd); }
static os_fd os_stdout(void) { HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); return h ? h : OS_BADFD; }
static os_fd os_stderr(void) { HANDLE h = GetStdHandle(STD_ERROR_HANDLE); return h ? h : OS_BADFD; }
static unsigned long os_tid(void) { return (unsigned long)GetCurrentThreadId(); }
static uint64_t os_now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}
static void os_mkdir(const char* p) { CreateDirectoryA(p, NULL); }
static void os_unlink(const char* p) { DeleteFileA(p); }
static void os_stamp(char out[16])
{
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(out, 16, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
static void os_sleep_ms(unsigned ms) { Sleep(ms); }
static void os_hard_exit(int code) { TerminateProcess(GetCurrentProcess(), (UINT)code); }
/* names in dir starting with pre and ending with suf; returns count (at most cap) */
static int os_list(const char* dir, const char* pre, const char* suf, char (*names)[64], int cap)
{
    char pat[PATH_MAX_]; WIN32_FIND_DATAA d; int n = 0;
    snprintf(pat, sizeof pat, "%s/%s*%s", dir, pre, suf);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        size_t l = strlen(d.cFileName), ls = strlen(suf);
        if (n < cap && l < 64 && l >= ls && !strcmp(d.cFileName + l - ls, suf)) memcpy(names[n++], d.cFileName, l + 1);
    } while (FindNextFileA(h, &d));
    FindClose(h);
    return n;
}
#else
typedef int os_fd;
#  define OS_BADFD (-1)
static os_fd os_create_new(const char* p) { return open(p, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644); }
static int  os_exists_err(void) { return errno == EEXIST; }
static void os_write(os_fd fd, const char* b, size_t n)
{
    while (n) { ssize_t w = write(fd, b, n); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
                b += w; n -= (size_t)w; }
}
static void os_close(os_fd fd) { close(fd); }
static os_fd os_stdout(void) { return 1; }
static os_fd os_stderr(void) { return 2; }
static unsigned long os_tid(void) { return (unsigned long)syscall(SYS_gettid); }
static uint64_t os_now_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static void os_mkdir(const char* p) { mkdir(p, 0755); }
static void os_unlink(const char* p) { unlink(p); }
static void os_stamp(char out[16])
{
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    snprintf(out, 16, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec);
}
static void os_sleep_ms(unsigned ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static void os_hard_exit(int code) { _exit(code); }
static int os_list(const char* dir, const char* pre, const char* suf, char (*names)[64], int cap)
{
    DIR* d = opendir(dir); struct dirent* e; int n = 0;
    if (!d) return 0;
    size_t lp = strlen(pre), ls = strlen(suf);
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (n < cap && l < 64 && l >= lp + ls && !strncmp(e->d_name, pre, lp) && !strcmp(e->d_name + l - ls, suf))
            memcpy(names[n++], e->d_name, l + 1);
    }
    closedir(d);
    return n;
}
#endif

/* ------------------------------------------------------------------------------------------------ state */
typedef struct {
    int def, lo, hi, n;                     /* lo/hi: min/max threshold over def and all entries (fast path) */
    struct { char tag[TAG_MAX]; unsigned char len; signed char lvl; } e[OVR_MAX];
} cfg_t;

static cfg_t            g_cfg0 = { KZ3_LOG_INFO, KZ3_LOG_INFO, KZ3_LOG_INFO, 0, {{{0}, 0, 0}} };
static _Atomic(cfg_t*)  g_cfg = &g_cfg0;
static _Atomic intptr_t g_fd = (intptr_t)-1;     /* os_fd of the log file, -1 = none */
static atomic_int       g_stdout_echo;
static atomic_int       g_stderr_errors;
static atomic_int       g_inited;
static _Atomic uint64_t g_t0_us;
static char             g_dir[PATH_MAX_];        /* "<dir>/logs" */
static char             g_path[PATH_MAX_];
static _Atomic(kz3_log_fatal_hook) g_hook;
static void* _Atomic    g_hook_user;
static atomic_int       g_fatal_busy;
static char             g_crash_path[PATH_MAX_];
#ifdef _WIN32
typedef BOOL (WINAPI *mdw_fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static mdw_fn           g_mdw;             /* dbghelp!MiniDumpWriteDump, resolved at init */
#endif
static atomic_int       g_crash_seq;

typedef struct { _Atomic uint64_t seq; unsigned short len; char text[RING_W]; } ring_slot;
static ring_slot        g_ring[KZ3_LOG_RING_LINES];
static _Atomic uint64_t g_ring_head;

static _Atomic uint64_t g_once[ONCE_SLOTS];

typedef struct {
    unsigned long tid;
    char  label[24];
    char  last_tag[TAG_MAX];
    int   last_level;
    int   last_valid;
    int   skipping;                         /* inside a filtered-out printf line that has no '\n' yet */
    int   in_fatal;
    size_t plen;
    char  pend[KZ3_LOG_LINE_MAX];           /* partial printf line */
} tls_t;
static _Thread_local tls_t t_tls;

static os_fd cur_fd(void) { intptr_t v = atomic_load_explicit(&g_fd, memory_order_acquire); return (os_fd)v; }

static uint64_t now_ms(void)
{
    uint64_t t0 = atomic_load_explicit(&g_t0_us, memory_order_relaxed), n = os_now_us();
    if (!t0) { uint64_t z = 0; atomic_compare_exchange_strong(&g_t0_us, &z, n); t0 = atomic_load(&g_t0_us); }
    return n >= t0 ? (n - t0) / 1000u : 0;
}

static const char k_lvl_char[] = "EWIDT";

/* ------------------------------------------------------------------------------------------------ levels */
int kz3_log_parse_level(const char* s, size_t n)
{
    static const struct { const char* name; int lvl; } k[] = {
        {"error", 0}, {"err", 0}, {"e", 0}, {"0", 0}, {"warn", 1}, {"warning", 1}, {"w", 1}, {"1", 1},
        {"info", 2}, {"i", 2}, {"2", 2}, {"debug", 3}, {"d", 3}, {"3", 3}, {"trace", 4}, {"t", 4}, {"4", 4},
        {"off", -1}, {"none", -1}, {"-1", -1} };
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        size_t l = strlen(k[i].name);
        if (l != n) continue;
        size_t j = 0;
        for (; j < n; j++) { char c = s[j]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != k[i].name[j]) break; }
        if (j == n) return k[i].lvl;
    }
    return -2;
}

static void cfg_bounds(cfg_t* c)
{
    c->lo = c->hi = c->def;
    for (int i = 0; i < c->n; i++) { if (c->e[i].lvl < c->lo) c->lo = c->e[i].lvl; if (c->e[i].lvl > c->hi) c->hi = c->e[i].lvl; }
}

static int cfg_lookup(const cfg_t* c, const char* tag, size_t len)
{
    for (int i = 0; i < c->n; i++)
        if (c->e[i].len == len && !memcmp(c->e[i].tag, tag, len)) return c->e[i].lvl;
    return c->def;
}

int kz3_log_threshold(const char* tag, size_t len)
{
    const cfg_t* c = atomic_load_explicit(&g_cfg, memory_order_acquire);
    return cfg_lookup(c, tag ? tag : "", tag ? len : 0);
}

static int enabled_n(int level, const char* tag, size_t len)
{
    const cfg_t* c = atomic_load_explicit(&g_cfg, memory_order_acquire);
    if (level <= c->lo) return 1;
    if (level > c->hi) return 0;
    return level <= cfg_lookup(c, tag, len);
}

int kz3_log_enabled(int level, const char* tag) { return enabled_n(level, tag ? tag : "", tag ? strlen(tag) : 0); }

static void cfg_publish(cfg_t* n) { cfg_bounds(n); atomic_store_explicit(&g_cfg, n, memory_order_release); }
/* The old table is not freed: a reader may still hold it. Reconfiguring is rare (init, tests, a debug console). */

int kz3_log_configure(const char* spec)
{
    cfg_t* n = (cfg_t*)malloc(sizeof *n);
    if (!n) return -1;
    *n = *atomic_load(&g_cfg);
    int bad = 0;
    const char* p = spec ? spec : "";
    while (*p) {
        const char* e = strchr(p, ','); if (!e) e = p + strlen(p);
        const char* colon = NULL;
        for (const char* q = p; q < e; q++) if (*q == ':') colon = q;   /* last ':' (tags may contain spaces) */
        if (colon) {
            const char* t = p; size_t tl = (size_t)(colon - p);
            while (tl && (*t == ' ')) { t++; tl--; }
            while (tl && t[tl - 1] == ' ') tl--;
            if (tl && t[0] == '[' && t[tl - 1] == ']') { t++; tl -= 2; }
            int lvl = kz3_log_parse_level(colon + 1, (size_t)(e - colon - 1));
            if (lvl == -2 || tl >= TAG_MAX) bad++;
            else if (tl == 1 && t[0] == '*') n->def = lvl;
            else {
                int i = 0;
                for (; i < n->n; i++) if (n->e[i].len == tl && !memcmp(n->e[i].tag, t, tl)) break;
                if (i == n->n) { if (n->n == OVR_MAX) { bad++; goto next; } n->n++; }
                memcpy(n->e[i].tag, t, tl); n->e[i].tag[tl] = 0; n->e[i].len = (unsigned char)tl;
                n->e[i].lvl = (signed char)lvl;
            }
        } else if (e > p) {                 /* bare level = default */
            int lvl = kz3_log_parse_level(p, (size_t)(e - p));
            if (lvl == -2) bad++; else n->def = lvl;
        }
    next:
        p = *e ? e + 1 : e;
    }
    cfg_publish(n);
    return bad ? -1 : 0;
}

void kz3_log_set_default_level(int level)
{
    cfg_t* n = (cfg_t*)malloc(sizeof *n);
    if (!n) return;
    *n = *atomic_load(&g_cfg);
    n->def = level;
    cfg_publish(n);
}

/* ------------------------------------------------------------------------------------------------ router table */
/* Message level of the runtime's existing "[tag] ..." diagnostics. Built from two real logs (pass 56):
 * kz3-check boot (60 s, 25.8 MB) and the rsx r28a level run (600 s, 367 MB, 7.9 M lines); counts and bytes per
 * 10 minutes of level play in the comments. Entries with a word match "[tag] word..." and must come before the
 * tag's plain entry. A tag ending in '*' matches any tag with that prefix. Unlisted tags are info. Hottest first. */
typedef struct { const char* tag; unsigned char tlen; const char* word; unsigned char wlen; signed char lvl; } route_t;
#define R(t, l)      { t, (unsigned char)(sizeof(t) - 1), "", 0, l }
#define RW(t, w, l)  { t, (unsigned char)(sizeof(t) - 1), w, (unsigned char)(sizeof(w) - 1), l }
#define E KZ3_LOG_ERROR
#define W KZ3_LOG_WARN
#define I KZ3_LOG_INFO
#define D KZ3_LOG_DEBUG
#define T KZ3_LOG_TRACE
static const route_t k_routes[] = {
    R("mfc-list", T),                       /* 3.1 M lines, 134 MB + its hex-dump continuation lines (~150 MB) */
    R("spurs-pm", T),                       /* 1.6 M, 73 MB */
    R("WAIT", T), R("WAKE", T),             /* 106 k, 7.1 MB */
    R("spu_workload", D),                   /* 31 k, 2.8 MB */
    RW("RSX null", "set_viewport", T), RW("RSX null", "set_render_target", T), RW("RSX null", "set_", T),
    R("RSX null", I),                       /* 55 k, 2.5 MB, nearly all set_* */
    RW("cellSpurs", "EventFlag", T), RW("cellSpurs", "CreateTask", D), RW("cellSpurs", "Wakeup", D),
    RW("cellSpurs", "SendSignal", D), R("cellSpurs", I),                    /* 15 k, 1.5 MB */
    RW("evt", "port_send", T), RW("evt", "queue_receive", T), RW("evt", "receive", T), R("evt", D), /* 26 k, 1.3 MB */
    R("taskset", D),                        /* 16 k, 1.2 MB */
    RW("cellGcmSys", "SetUserHandler", T), RW("cellGcmSys", "SetFlip", D), RW("cellGcmSys", "SetWaitFlip", D),
    R("cellGcmSys", I),                     /* 8 k, 385 kB */
    R("live-draw", D), R("dmaw", T), R("spu-ovl-map", D), R("tex-refresh", D), R("label", D),
    R("spurs-curcont", T), R("wws-res", T), R("wws-batch", T), R("wws-code", T), R("wws-jobentry", T),
    R("spu-int", D), R("LWM-BLOCK", T), R("LWM-GOT", T), R("spu-evt", T), R("spurs-kern", D), R("mfc", T),
    R("park-late", D), R("frame-mark", D), R("flipword", D), R("zetatrack", D), R("surfsz", D), R("task2", D),
    R("unnorm", D), R("GET_REPORT", D), R("report-readback", D), R("cond-render", D), R("tex3d", D),
    R("zalias", D), R("usercmd", D), R("NV0039", D), R("texture-cache", D), R("shader-disk-cache", D),
    /* fiber breaker (port/spu/kz3_ppu_overrides.cpp): "[fiber] S=... waiting N s" is runprofile_kz3.py's
     * spu-job-chain-stall signature; its "  slot" continuation lines and the watchdog "sample" lines belong with it.
     * The per-job "S=... wait/resume/start/finish slot" lines are behind KZ3_FIBER_LOG (default: the first 24 events), so info is
     * cheap. deadlock/watchdog/fatal are anomalies the breaker reports once. The plain tag stays debug (pass 72). */
    RW("fiber", "fatal", E), RW("fiber", "deadlock", W), RW("fiber", "watchdog", W),
    RW("fiber", "S=", I), RW("fiber", "  slot", I), RW("fiber", "sample", I),
    R("fiber", D), R("THREAD*", D), R("sem", D), R("spu-smc", D), R("spu-coh", D), R("host-aa", D),
    R("main-surface-writeback", D), R("surface-writeback", D), R("spu", D), R("guest-validate", D),
    R("present-qpc", D), R("sc-caller", D), R("cellAudio", D),
    RW("cellPad", "script", I), RW("cellPad", "PAD_SCRIPT", I),   /* scripted input must show in run logs (gameplay tests) */
    R("cellPad", D), R("cellKb", D),
    /* errors and warnings */
    R("CRASH", E), R("null-call", E), R("fatal", E), R("GUARDVIOLATION", E), R("HardExit", E),
    R("null-write", W), R("null-read", W), R("hle", W), R("TUNERFIX", W),
    R("link-miss", W), R("crashdump", D),
    /* info on purpose (kz3_metrics.py parses them): fs, sys_fs, flip, fps, spu-ovl, psarc, boot, SYS, RSX */
};
#undef R
#undef RW
#undef E
#undef W
#undef I
#undef D
#undef T

/* tag in [p, p+n) -> message level; rest = text after "] " */
static int route_level(const char* tag, size_t tl, const char* rest, size_t rl)
{
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++) {
        const route_t* r = &k_routes[i];
        if (r->tag[r->tlen - 1] == '*') {
            if (tl < (size_t)r->tlen - 1u || memcmp(r->tag, tag, r->tlen - 1u)) continue;
        } else if (r->tlen != tl || memcmp(r->tag, tag, tl)) continue;
        if (r->wlen && (rl < r->wlen || memcmp(rest, r->word, r->wlen))) continue;
        return r->lvl;
    }
    return KZ3_LOG_INFO;
}

/* "[tag] rest": returns tag length (0 = no tag); *tag and *rest point into s. stop_pct: give up on a '%' in the tag
 * (format strings). */
static size_t parse_tag(const char* s, size_t n, const char** tag, const char** rest, int stop_pct)
{
    if (n < 3 || s[0] != '[') return 0;
    size_t i = 1;
    for (; i < n && i <= TAG_MAX; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ']') break;
        if (c < 0x20 || c >= 0x7f || c == '[' || (stop_pct && c == '%')) return 0;
    }
    if (i >= n || s[i] != ']' || i == 1 || i > TAG_MAX - 1) return 0;
    *tag = s + 1;
    size_t r = i + 1;
    if (r < n && s[r] == ' ') r++;
    *rest = s + r;
    return i - 1;
}

int kz3_log_route(const char* line, size_t len, char* tag_out, size_t cap)
{
    const char *tag, *rest;
    size_t tl = parse_tag(line, len, &tag, &rest, 0);
    if (tag_out && cap) {
        size_t c = tl < cap - 1 ? tl : cap - 1;
        memcpy(tag_out, tag ? tag : "", c); tag_out[c] = 0;
    }
    if (!tl) return KZ3_LOG_INFO;
    return route_level(tag, tl, rest, len - (size_t)(rest - line));
}

/* ------------------------------------------------------------------------------------------------ emission */
/* The ring is a seqlock: a reader may copy a slot while a writer fills it and drops the copy when the sequence
 * moved. That copy is a benign race by design, so ThreadSanitizer is told to skip these two functions. */
#if defined(__SANITIZE_THREAD__)
#  define KZ3_NO_TSAN __attribute__((no_sanitize("thread")))
#else
#  define KZ3_NO_TSAN
#endif
/* Byte copy through volatile: never turned into a memcpy call (which TSan would intercept). Lines are short. */
KZ3_NO_TSAN static void ring_copy(volatile char* d, const volatile char* s, size_t n)
{
    for (size_t k = 0; k < n; k++) d[k] = s[k];
}

/* A writer claims its slot by CAS on the sequence: a slot still being written (odd) or already holding a newer line
 * (two writers 200 lines apart) is left alone and this line skips the ring (it still goes to the file). */
KZ3_NO_TSAN static void ring_put(const char* s, size_t n)
{
    uint64_t i = atomic_fetch_add_explicit(&g_ring_head, 1, memory_order_relaxed);
    ring_slot* r = &g_ring[i % KZ3_LOG_RING_LINES];
    if (n > RING_W) n = RING_W;
    uint64_t cur = atomic_load_explicit(&r->seq, memory_order_relaxed);
    if ((cur & 1) || cur > 2 * i + 1) return;
    if (!atomic_compare_exchange_strong_explicit(&r->seq, &cur, 2 * i + 1, memory_order_acquire, memory_order_relaxed))
        return;
    atomic_thread_fence(memory_order_release);
    ring_copy(r->text, s, n);
    r->len = (unsigned short)n;
    atomic_store_explicit(&r->seq, 2 * i + 2, memory_order_release);
}

/* Calls fn for each intact ring line, oldest first. Lines being rewritten while we read are skipped. */
KZ3_NO_TSAN static void ring_foreach(void (*fn)(const char*, size_t, void*), void* u)
{
    uint64_t h = atomic_load_explicit(&g_ring_head, memory_order_acquire);
    uint64_t i = h > KZ3_LOG_RING_LINES ? h - KZ3_LOG_RING_LINES : 0;
    char tmp[RING_W];
    for (; i < h; i++) {
        ring_slot* r = &g_ring[i % KZ3_LOG_RING_LINES];
        uint64_t q1 = atomic_load_explicit(&r->seq, memory_order_acquire);
        if (q1 != 2 * i + 2) continue;
        size_t n = r->len; if (n > RING_W) n = RING_W;
        ring_copy(tmp, r->text, n);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&r->seq, memory_order_relaxed) != q1) continue;
        fn(tmp, n, u);
    }
}

typedef struct { char* buf; size_t cap, len; } snap_t;
static void snap_cb(const char* s, size_t n, void* u)
{
    snap_t* b = (snap_t*)u;
    if (b->len + n + 2 > b->cap) return;
    memcpy(b->buf + b->len, s, n); b->len += n; b->buf[b->len++] = '\n'; b->buf[b->len] = 0;
}

size_t kz3_log_ring_snapshot(char* buf, size_t cap)
{
    if (!buf || !cap) return 0;
    snap_t b = { buf, cap, 0 };
    buf[0] = 0;
    ring_foreach(snap_cb, &b);
    return b.len;
}

static tls_t* tls(void)
{
    tls_t* t = &t_tls;
    if (!t->tid) t->tid = os_tid();
    return t;
}

void kz3_log_set_thread_label(const char* label)
{
    tls_t* t = tls();
    if (!label) { t->label[0] = 0; return; }
    snprintf(t->label, sizeof t->label, "%s", label);
}

/* body = "[tag] text" (or an untagged line), without the trailing newline */
static void emit_body(int level, const char* body, size_t n)
{
    tls_t* t = tls();
    char line[KZ3_LOG_LINE_MAX + 96];
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    int p = snprintf(line, 96, "%9llu %5lu%s%s %c ", (unsigned long long)now_ms(), t->tid, t->label[0] ? " " : "",
                     t->label, k_lvl_char[level]);
    if (p < 0) p = 0;
    if (p > 95) p = 95;
    while (n && (body[n - 1] == '\n' || body[n - 1] == '\r')) n--;
    size_t room = KZ3_LOG_LINE_MAX - 1;
    size_t b = n;
    if (b > room) b = room;
    memcpy(line + p, body, b);
    size_t len = (size_t)p + b;
    if (n > room) { memcpy(line + len - 3, "...", 3); }
    ring_put(line, len);
    line[len] = '\n';
    os_fd fd = cur_fd();
    if (fd != OS_BADFD) os_write(fd, line, len + 1);
    if (atomic_load_explicit(&g_stdout_echo, memory_order_relaxed)) {
        os_fd o = os_stdout(); if (o != OS_BADFD) os_write(o, line + p, len + 1 - (size_t)p);
    } else if (level == KZ3_LOG_ERROR && atomic_load_explicit(&g_stderr_errors, memory_order_relaxed)) {
        os_fd o = os_stderr(); if (o != OS_BADFD) os_write(o, line + p, len + 1 - (size_t)p);
    }
}

void kz3_logv(int level, const char* tag, const char* fmt, va_list ap)
{
    if (!tag) tag = "";
    size_t tl = strlen(tag);
    if (!enabled_n(level, tag, tl)) return;
    char body[KZ3_LOG_LINE_MAX + 8];
    int p = tl ? snprintf(body, TAG_MAX + 4, "[%.*s] ", (int)(tl < TAG_MAX ? tl : TAG_MAX), tag) : 0;
    if (p < 0) p = 0;
    int m = vsnprintf(body + p, sizeof body - (size_t)p, fmt, ap);
    size_t n = (size_t)p + (m < 0 ? 0 : (size_t)m);
    if (n >= sizeof body) { n = sizeof body - 1; memcpy(body + n - 3, "...", 3); }
    /* multi-line messages: one log line each, same level/tag */
    size_t s = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || body[i] == '\n') {
            if (i > s || s == 0) emit_body(level, body + s, i - s);
            s = i + 1;
        }
    }
}

void kz3_log(int level, const char* tag, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); kz3_logv(level, tag, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ router */
static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* An untagged line that continues the previous one of the same thread (hex dumps, indented detail lines). */
static int is_continuation(const char* s, size_t n)
{
    if (!n) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c == ' ' || c == '\t' || (c >= '0' && c <= '9')) return 1;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) && c != '[') return 1;   /* punctuation */
    size_t k = 0;
    while (k < n && is_hex(s[k])) k++;
    return k >= 4 && k < n && (s[k] == ' ' || s[k] == ':');
}

static void route_one(const char* s, size_t n)
{
    tls_t* t = tls();
    while (n && (s[n - 1] == '\r')) n--;
    if (!n) return;                                    /* blank lines carry nothing */
    const char *tag, *rest;
    size_t tl = parse_tag(s, n, &tag, &rest, 0);
    int lvl;
    if (tl) {
        lvl = route_level(tag, tl, rest, n - (size_t)(rest - s));
        size_t c = tl < TAG_MAX - 1 ? tl : TAG_MAX - 1;
        memcpy(t->last_tag, tag, c); t->last_tag[c] = 0;
        t->last_level = lvl; t->last_valid = 1;
    } else if (t->last_valid && is_continuation(s, n)) {
        tag = t->last_tag; tl = strlen(t->last_tag); lvl = t->last_level;
    } else {
        tag = ""; tl = 0; lvl = KZ3_LOG_INFO;
    }
    if (enabled_n(lvl, tag, tl)) emit_body(lvl, s, n);
}

void kz3_log_line(const char* text)
{
    if (!text) return;
    const char* s = text;
    for (const char* p = text;; p++) {
        if (*p == '\n' || !*p) {
            route_one(s, (size_t)(p - s));
            if (!*p) break;
            s = p + 1;
        }
    }
}

/* Append formatted text to the thread's partial line and route every completed line. */
static void pend_feed(tls_t* t, const char* s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') { route_one(t->pend, t->plen); t->plen = 0; continue; }
        if (t->plen == sizeof t->pend - 1) { route_one(t->pend, t->plen); t->plen = 0; }
        t->pend[t->plen++] = c;
    }
}

int kz3_log_vprintf(const char* fmt, va_list ap)
{
    if (!fmt) return 0;
    tls_t* t = tls();
    if (t->skipping) {                     /* rest of a filtered-out line */
        if (strchr(fmt, '\n')) t->skipping = 0;
        return 0;
    }
    if (t->plen == 0 && fmt[0] == '[') {   /* fast path: decide from the format's own tag, before formatting */
        const char *tag, *rest;
        size_t fl = strlen(fmt);
        size_t tl = parse_tag(fmt, fl, &tag, &rest, 1);
        if (tl) {
            int lvl = route_level(tag, tl, rest, fl - (size_t)(rest - fmt));
            if (!enabled_n(lvl, tag, tl)) {
                size_t c = tl < TAG_MAX - 1 ? tl : TAG_MAX - 1;
                memcpy(t->last_tag, tag, c); t->last_tag[c] = 0;
                t->last_level = lvl; t->last_valid = 1;
                const char* nl = strchr(fmt, '\n');
                if (!nl) t->skipping = 1;
                return 0;
            }
        }
    }
    char buf[KZ3_LOG_LINE_MAX * 2];
    va_list cp; va_copy(cp, ap);
    int m = vsnprintf(buf, sizeof buf, fmt, cp);
    va_end(cp);
    if (m < 0) return m;
    size_t n = (size_t)m < sizeof buf - 1 ? (size_t)m : sizeof buf - 1;
    pend_feed(t, buf, n);
    return m;
}

int kz3_log_printf(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = kz3_log_vprintf(fmt, ap); va_end(ap); return r;
}

int kz3_log_vfprintf(FILE* f, const char* fmt, va_list ap)
{
    if (f == stdout || f == stderr) return kz3_log_vprintf(fmt, ap);
    return vfprintf(f, fmt, ap);
}

int kz3_log_fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt); int r = kz3_log_vfprintf(f, fmt, ap); va_end(ap); return r;
}

int kz3_log_fputs(const char* s, FILE* f)
{
    if (f != stdout && f != stderr) return fputs(s, f);
    tls_t* t = tls();
    if (t->skipping) { if (strchr(s, '\n')) t->skipping = 0; return 0; }
    pend_feed(t, s, strlen(s));
    return 0;
}

int kz3_log_puts(const char* s)
{
    tls_t* t = tls();
    if (t->skipping) { t->skipping = 0; return 0; }
    pend_feed(t, s, strlen(s));
    pend_feed(t, "\n", 1);
    return 0;
}

void kz3_log_flush(void)
{
    tls_t* t = tls();
    if (t->plen) { route_one(t->pend, t->plen); t->plen = 0; }
}

int kz3_log_fflush(FILE* f)
{
    /* A redirected caller's partial line stays pending. The CRT stream is flushed all the same: files without the
     * redirect (the SPU probes, kz3_stall_diag.c, kz3_cpu_check.c) still write it, and ps3_stdio_init() makes it
     * fully buffered (64 KB), so ps3_stdio's flush thread and its exit flush, which arrive here, are their only flush
     * (SPU pass 115: UCRT builds lost those lines). */
    return fflush(f);
}

/* ------------------------------------------------------------------------------------------------ once */
int kz3_log_once_flag(int* flag)
{
    return atomic_exchange_explicit((_Atomic int*)flag, 1, memory_order_relaxed) == 0;
}

int kz3_log_once_key(const char* tag, uint64_t key)
{
    uint64_t h = 1469598103934665603ull;
    for (const char* p = tag ? tag : ""; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    h ^= key + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    uint64_t v = h | 1;
    for (unsigned i = 0; i < 64; i++) {
        _Atomic uint64_t* s = &g_once[(h + i) & (ONCE_SLOTS - 1)];
        uint64_t cur = atomic_load_explicit(s, memory_order_relaxed);
        if (cur == v) return 0;
        if (cur == 0) {
            uint64_t z = 0;
            if (atomic_compare_exchange_strong(s, &z, v)) return 1;
            if (z == v) return 0;
        }
    }
    return 0;                                   /* table full around this key: stay quiet */
}

/* ------------------------------------------------------------------------------------------------ files */
static int cmp_names(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

/* Delete the oldest pre*suf files in g_dir so that at most keep remain. Names carry a sortable timestamp. */
static void prune(const char* pre, const char* suf, int keep)
{
    static char names[256][64];     /* only called from init and the (serialized) crash path */
    int n = os_list(g_dir, pre, suf, names, 256), m = 0;
    /* Only <pre><digit>...: "Killzone3-PC-" also starts the launcher's and setup's own logs (Killzone3-PC-setup-*),
     * which are not ours to prune (pass 38, ui-review row 27). */
    for (int i = 0; i < n; i++)
        if (names[i][strlen(pre)] >= '0' && names[i][strlen(pre)] <= '9') { if (m != i) memcpy(names[m], names[i], 64); m++; }
    n = m;
    if (n <= keep) return;
    qsort(names, (size_t)n, 64, cmp_names);
    for (int i = 0; i < n - keep; i++) {
        char p[PATH_MAX_];
        snprintf(p, sizeof p, "%s/%s", g_dir, names[i]);
        os_unlink(p);
    }
}

/* Create <g_dir>/<pre><stamp>[-k]<suf>; path to out. */
static os_fd create_stamped(const char* pre, const char* suf, char* out, size_t cap)
{
    char st[16]; os_stamp(st);
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(out, cap, "%s/%s%s%s", g_dir, pre, st, suf);
        else snprintf(out, cap, "%s/%s%s-%02d%s", g_dir, pre, st, k, suf);
        os_fd fd = os_create_new(out);
        if (fd != OS_BADFD) return fd;
        if (!os_exists_err()) break;
    }
    out[0] = 0;
    return OS_BADFD;
}

static int env_is1(const char* name)
{
    const char* e = getenv(name);
    return e && e[0] == '1' && !e[1];
}

int kz3_log_init(const char* dir, unsigned flags)
{
    int z = 0;
    if (!atomic_compare_exchange_strong(&g_inited, &z, 1)) return -1;
    now_ms();
    int to_stdout = (flags & KZ3_LOG_F_STDOUT) != 0;
    if (!(flags & KZ3_LOG_F_NO_ENV)) {
        const char* e = getenv("KZ3_LOG_LEVEL");
        if (e && *e) { int l = kz3_log_parse_level(e, strlen(e)); if (l != -2) kz3_log_set_default_level(l); }
        e = getenv("KZ3_LOG");
        if (e && *e) kz3_log_configure(e);
        e = getenv("KZ3_LOG_STDOUT");
        if (e && *e) to_stdout = e[0] == '1';
    }
    atomic_store(&g_stdout_echo, to_stdout);
    atomic_store(&g_stderr_errors, (flags & KZ3_LOG_F_STDERR_ERRORS) != 0);
    int rc = 0;
    g_path[0] = 0; g_dir[0] = 0;
    if (dir && *dir) {
        snprintf(g_dir, sizeof g_dir, "%s/logs", dir);
        os_mkdir(g_dir);
        prune(KZ3_LOG_NAME_PREFIX, ".log", KZ3_LOG_KEEP_FILES - 1);
        os_fd fd = create_stamped(KZ3_LOG_NAME_PREFIX, ".log", g_path, sizeof g_path);
        if (fd == OS_BADFD) rc = 1;
        else atomic_store_explicit(&g_fd, (intptr_t)fd, memory_order_release);
    }
    const cfg_t* c = atomic_load(&g_cfg);
    char st[16]; os_stamp(st);
    kz3_log(KZ3_LOG_INFO, "log", "started %s, file '%s', default level %c, %d tag override(s), stdout %s",
            st, g_path, k_lvl_char[c->def < 0 ? 0 : c->def], c->n, to_stdout ? "on" : "off");
#ifdef _WIN32
    /* dbghelp now, not inside the crash filter (the loader lock may be held by the faulting thread). */
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    g_mdw = dh ? (mdw_fn)(void*)GetProcAddress(dh, "MiniDumpWriteDump") : NULL;
    if (!g_mdw) kz3_log(KZ3_LOG_WARN, "log", "dbghelp!MiniDumpWriteDump unavailable (%lu): no minidumps", (unsigned long)GetLastError());
#endif
    return rc;
}

void kz3_log_shutdown(void)
{
    kz3_log_flush();
    kz3_log(KZ3_LOG_INFO, "log", "closed");
    intptr_t v = atomic_exchange(&g_fd, (intptr_t)-1);
    if (v != (intptr_t)-1) os_close((os_fd)v);
    atomic_store(&g_stdout_echo, 0);
    atomic_store(&g_inited, 0);       /* allows a later init (tests) */
}

const char* kz3_log_file_path(void) { return g_path; }
const char* kz3_log_dir(void) { return g_dir; }

/* ------------------------------------------------------------------------------------------------ fatal */
static void crash_ring_cb(const char* s, size_t n, void* u)
{
    os_fd fd = *(os_fd*)u;
    os_write(fd, s, n); os_write(fd, "\n", 1);
}

static void wr(os_fd fd, const char* s) { os_write(fd, s, strlen(s)); }

const char* kz3_log_write_crash(const char* reason, const char* details)
{
    static char path[PATH_MAX_];
    int busy = 0;
    if (!g_dir[0]) return "";
    if (!atomic_compare_exchange_strong(&g_crash_seq, &busy, 1)) return "";   /* another crash file in progress */
    prune(KZ3_LOG_NAME_PREFIX "crash-", ".txt", KZ3_LOG_KEEP_FILES - 1);
    os_fd fd = create_stamped(KZ3_LOG_NAME_PREFIX "crash-", ".txt", path, sizeof path);
    if (fd == OS_BADFD) { atomic_store(&g_crash_seq, 0); return ""; }
    char head[256];
    snprintf(head, sizeof head, "Killzone 3 port crash report\ntime: %llu ms after start, thread %lu\n",
             (unsigned long long)now_ms(), tls()->tid);
    wr(fd, head);
    wr(fd, "log file: "); wr(fd, g_path); wr(fd, "\nreason: "); wr(fd, reason ? reason : "");
    wr(fd, "\n\n"); wr(fd, details ? details : ""); wr(fd, "\n\nlast log lines (oldest first):\n");
    ring_foreach(crash_ring_cb, &fd);
    os_close(fd);
    memcpy(g_crash_path, path, sizeof path);
    atomic_store(&g_crash_seq, 0);
    return path;
}

#ifdef _WIN32
struct dump_job { EXCEPTION_POINTERS* ep; DWORD tid; const char* path; BOOL ok; DWORD err; };
static DWORD WINAPI dump_thread(LPVOID p)
{
    struct dump_job* j = (struct dump_job*)p;
    HANDLE f = CreateFileA(j->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { j->err = GetLastError(); return 0; }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = j->tid; mei.ExceptionPointers = j->ep; mei.ClientPointers = FALSE;
    /* MiniDumpNormal: thread stacks and the module list; the 4 GB guest reservation stays out. */
    j->ok = g_mdw(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, NULL, NULL);
    j->err = j->ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!j->ok) DeleteFileA(j->path);
    return 0;
}
#endif

int kz3_log_write_minidump(const char* crash_path, void* exception_pointers, unsigned long faulting_tid)
{
#ifdef _WIN32
    static char dmp[PATH_MAX_];           /* one dump per process: the caller is the (serialized) crash path */
    if (!crash_path || !*crash_path) return 0;
    if (!g_mdw) { kz3_log(KZ3_LOG_ERROR, "CRASH", "minidump NOT written (dbghelp unavailable)"); return 0; }
    snprintf(dmp, sizeof dmp, "%s", crash_path);
    char* dot = strrchr(dmp, '.');
    if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) strcpy(dot, ".dmp");
    else snprintf(dmp + strlen(dmp), sizeof dmp - strlen(dmp), ".dmp");
    static struct dump_job j;             /* static: a stuck dump thread must not write into a returned frame */
    j.ep = (EXCEPTION_POINTERS*)exception_pointers; j.tid = (DWORD)faulting_tid; j.path = dmp; j.ok = FALSE; j.err = 0;
    HANDLE t = CreateThread(NULL, 256 * 1024, dump_thread, &j, 0, NULL);
    if (t) {
        if (WaitForSingleObject(t, 60000) != WAIT_OBJECT_0) {
            kz3_log(KZ3_LOG_ERROR, "CRASH", "minidump thread did not finish in 60 s");
            return 0;
        }
        CloseHandle(t);
    } else {
        kz3_log(KZ3_LOG_WARN, "CRASH", "CreateThread failed (%lu), dumping on the faulting thread", (unsigned long)GetLastError());
        dump_thread(&j);
    }
    if (j.ok) kz3_log(KZ3_LOG_ERROR, "CRASH", "minidump '%s'", dmp);
    else      kz3_log(KZ3_LOG_ERROR, "CRASH", "minidump NOT written (error %lu)", (unsigned long)j.err);
    return j.ok ? 1 : 0;
#else
    (void)crash_path; (void)exception_pointers; (void)faulting_tid;
    return 0;
#endif
}

void kz3_log_set_fatal_hook(kz3_log_fatal_hook fn, void* user)
{
    atomic_store(&g_hook_user, user);
    atomic_store(&g_hook, fn);
}

void kz3_log_fatal(const char* tag, const char* fmt, ...)
{
    tls_t* t = tls();
    char msg[KZ3_LOG_LINE_MAX];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    if (!tag || !*tag) tag = "fatal";
    if (t->in_fatal) {                          /* fatal raised by the hook itself */
        kz3_log(KZ3_LOG_ERROR, tag, "FATAL inside the fatal handler: %s", msg);
        os_hard_exit(KZ3_LOG_FATAL_EXIT);
        return;
    }
    int z = 0;
    while (!atomic_compare_exchange_strong(&g_fatal_busy, &z, 1)) {   /* another thread is reporting */
        z = 0;
        os_sleep_ms(5);
    }
    t->in_fatal = 1;
    kz3_log_flush();
    kz3_log(KZ3_LOG_ERROR, tag, "FATAL: %s", msg);
    const char* cp = kz3_log_write_crash("fatal error", msg);
    kz3_log(KZ3_LOG_ERROR, tag, "crash report: '%s'", cp);
    int cont;
    if (env_is1("KZ3_CONTINUE_ON_FATAL")) {
        kz3_log(KZ3_LOG_WARN, tag, "KZ3_CONTINUE_ON_FATAL=1: continuing");
        cont = 1;
    } else {
        kz3_log_fatal_hook h = atomic_load(&g_hook);
        cont = h ? h(msg, cp, atomic_load(&g_hook_user)) : 0;
    }
    t->in_fatal = 0;
    if (!cont) {
        kz3_log(KZ3_LOG_ERROR, tag, "stopping (exit code %d)", KZ3_LOG_FATAL_EXIT);
        os_hard_exit(KZ3_LOG_FATAL_EXIT);
    }
    atomic_store(&g_fatal_busy, 0);
}

/* ------------------------------------------------------------------------------------------------ game hooks */
/* RE pass 64 (the RE notes section 34): HardExit text, SetCrashDumpField fields, [link-miss]. */
typedef struct { uint32_t addr; uint32_t frame; uint32_t buf_off; const char* game; } hardexit_site_t;
static const hardexit_site_t k_hardexit[] = {
    /* @0x0008AB70 v1.00: stdu r1,-0x1080; vsnprintf(r1+0x70, 0x1000, r3, va); stw 0x0DEADCA7 -> [0] */
    { KZ3_ADDR_HARDEXIT_100, 0x1080u, 0x70u, "1.00" },
    /* @0x0008C360 1.14: same frame and buffer, plus strlen + SetCrashDumpField(3, buf, len) before the store */
    { KZ3_ADDR_HARDEXIT_114, 0x1080u, 0x70u, "1.14" },
};
static const char* const k_crashdump_keys[14] = {      /* 1.14 0x8CF08's key table, strings at 0xE7D4C8.. */
    "BuildNumber", "Level", "Section", "HardExitMessage", "HeapFreeBytes", "Player0Camera", "Player1Camera",
    "Player2Camera", "Player3Camera", "HintedSection", "StreamingLump", "StreamingState", "3DMode", "SplitScreen"
};
static char g_lump[96];                                 /* field 10: the lump the loader is processing */

/* Copies up to max printable bytes (NUL stops; control bytes become '?'), strips trailing newlines. */
static size_t guest_text(const uint8_t* p, uint32_t max, char* out, size_t cap)
{
    size_t n = 0;
    for (; n < max && n + 1 < cap; n++) {
        uint8_t c = p[n];
        if (!c) break;
        out[n] = (c == '\n' || c >= 0x20) ? (char)c : '?';
    }
    while (n > 0 && out[n - 1] == '\n') n--;
    out[n] = 0;
    return n;
}
static uint64_t fnv64(const char* s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

int kz3_log_hardexit(uint32_t fn, uint32_t sp, uint32_t lr, const uint8_t* vm)
{
    const hardexit_site_t* site = NULL;
    for (size_t i = 0; i < sizeof k_hardexit / sizeof k_hardexit[0]; i++)
        if (k_hardexit[i].addr == fn) site = &k_hardexit[i];
    if (!site || !vm) return 0;
    uint32_t buf = sp - site->frame + site->buf_off;
    if (buf < 0x10000u || buf >= 0xF0000000u) return 0;
    char text[KZ3_HOOK_TEXT_MAX + 1];
    if (!guest_text(vm + buf, KZ3_HOOK_TEXT_MAX, text, sizeof text)) return 0;
    if (!kz3_log_once_key("HardExit", (uint64_t)lr ^ (fnv64(text) << 8))) return 0;
    const char* mode = getenv("KZ3_HARDEXIT");
    if (mode && !strcmp(mode, "fatal"))
        kz3_log_fatal("HardExit", "%s (caller lr=0x%08X, HardExit @0x%08X, game %s)", text, lr, fn, site->game);
    else
        kz3_log(KZ3_LOG_ERROR, "HardExit", "%s (caller lr=0x%08X, HardExit @0x%08X, game %s)", text, lr, fn, site->game);
    return 1;
}

void kz3_log_crashdump_field(uint32_t id, const uint8_t* text, uint32_t len)
{
    if (!text) return;
    char buf[KZ3_HOOK_TEXT_MAX + 1];
    guest_text(text, len < KZ3_HOOK_TEXT_MAX ? len : KZ3_HOOK_TEXT_MAX, buf, sizeof buf);
    if (id == 10) { size_t n = strlen(buf); if (n >= sizeof g_lump) n = sizeof g_lump - 1; memcpy(g_lump, buf, n); g_lump[n] = 0; }
    if (id == 3) return;                                /* the HardExit hook logs it with its caller */
    if (!kz3_log_enabled(KZ3_LOG_DEBUG, "crashdump")) return;
    if (id < 14) kz3_log(KZ3_LOG_DEBUG, "crashdump", "%s='%s'", k_crashdump_keys[id], buf);
    else         kz3_log(KZ3_LOG_DEBUG, "crashdump", "field%u='%s'", (unsigned)id, buf);
}
const char* kz3_log_streaming_lump(void) { return g_lump; }

int kz3_log_link_miss(uint32_t status, const uint8_t key[16], uint32_t holder, uint32_t lr)
{
    static _Atomic int s_lines;
    if (status != 8u || !key) return 0;
    if (!kz3_log_enabled(KZ3_LOG_WARN, "link-miss")) return 0;
    uint32_t w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    if (!kz3_log_once_key("link-miss", ((uint64_t)w[0] << 32 | w[1]) ^ ((uint64_t)w[2] << 32 | w[3]) * 0x9E3779B97F4A7C15ull))
        return 0;
    int n = atomic_fetch_add_explicit(&s_lines, 1, memory_order_relaxed);
    if (n >= KZ3_LINK_MISS_MAX) {
        if (n == KZ3_LINK_MISS_MAX)
            kz3_log(KZ3_LOG_WARN, "link-miss", "further [link-miss] lines suppressed after %d", KZ3_LINK_MISS_MAX);
        return 0;
    }
    kz3_log(KZ3_LOG_WARN, "link-miss", "lump='%s' target=%08X%08X%08X%08X holder=0x%08X lr=0x%08X",
            g_lump[0] ? g_lump : "?", w[0], w[1], w[2], w[3], holder, lr);
    return 1;
}
