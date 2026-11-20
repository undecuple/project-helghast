/*
 * KZ3 port, test only: passive loopback listener for the first Medius exchange: it only listens, on loopback,
 * and never replies.
 *
 * Included by kz3_net.c (like rt_capture.c) and by tools/net/test_rt_passive.c. Opt-in: KZ3_NET_PASSIVE=1, the
 * server is 127.0.0.0/8 and KZ3_NET_CAPTURE is not set (the answering capture owns the same ports). It binds only
 * 127.0.0.1 (any other address is refused), receives and never answers: no data is ever sent on any socket.
 *
 * Diagnostic output: it records, to one text file under work/ (KZ3_NET_PASSIVE_OUT, default work/net/medius-passive.log):
 *   per connection: order number, relative start and end time, port, tcp or udp, bytes each way, which side closed;
 *   per client-to-listener frame: the id byte, its encryption-flag bit, the declared 16-bit length, and whether a
 *   checksum field is present (yes/no; RT framing: u8 id with bit 7 = encrypted, u16 LE length, then for an encrypted
 *   frame a 4-byte hash field ahead of the payload).
 * It never records payload bytes, checksum values, tickets, names, hashes of those, or peer addresses. Payload and
 * checksum bytes are counted and skipped as they arrive, never stored. If the framing does not fit (a declared length
 * above RTP_MAX_LEN, which also catches HTTP text), it records "unknown-framing" with the bytes received so far and
 * stops parsing that connection (it still counts bytes).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
typedef SOCKET rtp_sock;
#  define RTP_BAD INVALID_SOCKET
#  define rtp_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
typedef int rtp_sock;
#  define RTP_BAD (-1)
#  define rtp_close close
#endif

#define RTP_MAX_LEN     0x4000u     /* ASSUMPTION: a longer declared length is not RT framing */
#define RTP_MAX_LINES   20000u
#define RTP_MAX_CONNS   64u
#define RTP_MAX_LISTEN  16u

typedef struct {
    int      stopped;     /* unknown framing seen */
    int      skipping;
    uint8_t  hdr[3];
    unsigned hn;
    uint32_t skip;
    uint64_t rx;          /* bytes seen on this connection/flow */
} RtpParse;

typedef struct {
    rtp_sock s;
    int      udp;
    int      port;
} RtpListener;

static struct {
    FILE*            f;
    int              open;
    volatile int     stop;
    uint64_t         t0_ms;
    unsigned         order, lines, live;
    unsigned         nlisten;
    RtpListener      l[RTP_MAX_LISTEN];
    void           (*log)(const char*);
#ifdef _WIN32
    CRITICAL_SECTION mu;
#else
    pthread_mutex_t  mu;
#endif
} g_rtp = {0};

static uint64_t rtp_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}
static void rtp_lock(void)   {
#ifdef _WIN32
    EnterCriticalSection(&g_rtp.mu);
#else
    pthread_mutex_lock(&g_rtp.mu);
#endif
}
static void rtp_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_rtp.mu);
#else
    pthread_mutex_unlock(&g_rtp.mu);
#endif
}
static void rtp_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L }; nanosleep(&ts, NULL);
#endif
}

/* One line to the file (caller holds no lock). The format strings below carry numbers and fixed words only. */
static void rtp_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void rtp_line(const char* fmt, ...)
{
    rtp_lock();
    if (g_rtp.open) {
        if (g_rtp.lines < RTP_MAX_LINES) {
            va_list ap; va_start(ap, fmt); vfprintf(g_rtp.f, fmt, ap); va_end(ap);
            fputc('\n', g_rtp.f); fflush(g_rtp.f);
        } else if (g_rtp.lines == RTP_MAX_LINES) { fputs("log-full: further lines dropped\n", g_rtp.f); fflush(g_rtp.f); }
        g_rtp.lines++;
    }
    rtp_unlock();
}
static unsigned rtp_next_order(void) { rtp_lock(); unsigned n = ++g_rtp.order; rtp_unlock(); return n; }
static unsigned long rtp_rel(void) { return (unsigned long)(rtp_now_ms() - g_rtp.t0_ms); }

/* Feed received bytes (client to listener). Only the 3 header bytes of a frame are ever held. */
static void rtp_feed(RtpParse* p, unsigned order, const uint8_t* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p->rx++;
        if (p->stopped) continue;
        if (p->skipping) {                       /* payload and checksum bytes: counted, skipped, never stored */
            size_t k = n - i; if (k > p->skip) k = p->skip;
            p->skip -= (uint32_t)k; p->rx += k - 1; i += k - 1;
            if (!p->skip) p->skipping = 0;
            continue;
        }
        p->hdr[p->hn++] = b[i];
        if (p->hn < 3) continue;
        p->hn = 0;
        unsigned len = p->hdr[1] | (unsigned)p->hdr[2] << 8;
        int enc = (p->hdr[0] & 0x80) != 0;
        if (len > RTP_MAX_LEN) {
            p->stopped = 1;
            rtp_line("unknown-framing conn=%u t_ms=%lu bytes_so_far=%llu", order, rtp_rel(), (unsigned long long)p->rx);
            continue;
        }
        rtp_line("frame conn=%u t_ms=%lu id=0x%02X enc=%d len=%u checksum=%s", order, rtp_now_ms() >= g_rtp.t0_ms ? rtp_rel() : 0ul,
                 p->hdr[0] & 0x7Fu, enc, len, enc ? "yes" : "no");
#ifdef RTP_MUTATE_LEAK   /* test mutation only: the host test must fail when a payload byte reaches the output */
        rtp_line("leak %02x", (unsigned)(i + 1 < n ? b[i + 1] : 0));
#endif
        p->skip = len + (enc ? 4u : 0u);
        p->skipping = p->skip != 0;
    }
}

/* Strict loopback rule: exactly the dotted string 127.0.0.1. */
static int rtp_is_loopback(const char* ip) { return ip && !strcmp(ip, "127.0.0.1"); }

/* The output must be under work/: a relative or absolute path with a "work" directory component and no ".." */
static int rtp_path_ok(const char* path)
{
    if (!path || !*path || strlen(path) > 400) return 0;
    if (strstr(path, "..") || strchr(path, '\\')) return 0;
    if (!strncmp(path, "work/", 5)) return 1;
    return strstr(path, "/work/") != NULL;
}

static int rtp_mkparents(const char* path)
{
#ifndef _WIN32
    char t[512]; snprintf(t, sizeof t, "%s", path);
    for (char* c = t + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(t, 0755); *c = '/'; }
#else
    (void)path;
#endif
    return 0;
}

int kz3_net_passive_open_log(const char* path, void (*log)(const char*))
{
    if (g_rtp.open) return -1;
    if (!rtp_path_ok(path)) { if (log) log("passive capture: output path refused (it must be under work/)"); return -1; }
#ifdef _WIN32
    static int inited; if (!inited) { InitializeCriticalSection(&g_rtp.mu); inited = 1; }
#else
    static int inited; if (!inited) { pthread_mutex_init(&g_rtp.mu, NULL); inited = 1; }
#endif
    rtp_mkparents(path);
    g_rtp.f = fopen(path, "w");
    if (!g_rtp.f) { if (log) log("passive capture: cannot open the output file"); return -1; }
    g_rtp.log = log; g_rtp.stop = 0; g_rtp.order = 0; g_rtp.lines = 0; g_rtp.live = 0; g_rtp.nlisten = 0;
    g_rtp.t0_ms = rtp_now_ms(); g_rtp.open = 1;
    fputs("framing assumption: checksum field follows when the encryption bit is set\n", g_rtp.f);
    fprintf(g_rtp.f, "assumption: declared length above %u is unknown framing (parsing of that connection stops)\n", RTP_MAX_LEN);
    fputs("# passive loopback listener: connections and frame headers only; nothing is ever sent\n", g_rtp.f);
    return 0;
}

typedef struct { rtp_sock s; int port, udp; } RtpArg;

static int rtp_wait_readable(rtp_sock s)   /* 1 readable, 0 timeout or stop */
{
    fd_set fs; struct timeval tv = { 0, 100000 };
    FD_ZERO(&fs); FD_SET(s, &fs);
    return select((int)s + 1, &fs, NULL, NULL, &tv) > 0;
}

static void rtp_live(int d) { rtp_lock(); g_rtp.live += (unsigned)d; rtp_unlock(); }

#ifdef _WIN32
static DWORD WINAPI rtp_conn_thread(LPVOID v)
#else
static void* rtp_conn_thread(void* v)
#endif
{
    RtpArg a = *(RtpArg*)v; free(v);
    unsigned order = rtp_next_order();
    unsigned long t_open = rtp_rel();
    RtpParse p; memset(&p, 0, sizeof p);
    const char* who = "listener";
    uint8_t buf[2048];
    rtp_line("open conn=%u t_ms=%lu port=%d proto=tcp", order, t_open, a.port);
    while (!g_rtp.stop) {
        if (!rtp_wait_readable(a.s)) continue;
        int n = (int)recv(a.s, (char*)buf, sizeof buf, 0);
        if (n == 0) { who = "client"; break; }
        if (n < 0) { who = "error"; break; }
        rtp_feed(&p, order, buf, (size_t)n);
    }
    rtp_line("close conn=%u t_ms=%lu port=%d proto=tcp rx=%llu tx=0 closed_by=%s", order, rtp_rel(), a.port,
             (unsigned long long)p.rx, who);
    rtp_close(a.s);
    rtp_live(-1);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI rtp_tcp_thread(LPVOID v)
#else
static void* rtp_tcp_thread(void* v)
#endif
{
    RtpArg l = *(RtpArg*)v; free(v);
    while (!g_rtp.stop) {
        if (!rtp_wait_readable(l.s)) continue;
        rtp_sock c = accept(l.s, NULL, NULL);
        if (c == RTP_BAD) continue;
        rtp_lock(); int full = g_rtp.live >= RTP_MAX_CONNS + RTP_MAX_LISTEN; if (!full) g_rtp.live++; rtp_unlock();
        RtpArg* a = (RtpArg*)malloc(sizeof *a);
        if (full || !a) { free(a); if (!full) rtp_live(-1); rtp_close(c); continue; }
        a->s = c; a->port = l.port; a->udp = 0;
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, rtp_conn_thread, a, 0, NULL);
        if (h) CloseHandle(h); else { free(a); rtp_close(c); rtp_live(-1); }
#else
        pthread_t t;
        if (pthread_create(&t, NULL, rtp_conn_thread, a) == 0) pthread_detach(t); else { free(a); rtp_close(c); rtp_live(-1); }
#endif
    }
    rtp_close(l.s);
    rtp_live(-1);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI rtp_udp_thread(LPVOID v)
#else
static void* rtp_udp_thread(void* v)
#endif
{
    RtpArg l = *(RtpArg*)v; free(v);
    unsigned order = 0; unsigned long t_open = 0;
    RtpParse flow; memset(&flow, 0, sizeof flow);
    uint8_t buf[2048];
    while (!g_rtp.stop) {
        if (!rtp_wait_readable(l.s)) continue;
        int n = (int)recvfrom(l.s, (char*)buf, sizeof buf, 0, NULL, NULL);   /* the sender's address is not read */
        if (n < 0) break;
        if (!order) { order = rtp_next_order(); t_open = rtp_rel(); rtp_line("open conn=%u t_ms=%lu port=%d proto=udp", order, t_open, l.port); }
        RtpParse d = flow; d.hn = 0; d.skipping = 0; d.skip = 0;   /* each datagram starts a frame */
        rtp_feed(&d, order, buf, (size_t)n);
        flow.stopped = d.stopped; flow.rx = d.rx;
    }
    if (order) rtp_line("close conn=%u t_ms=%lu port=%d proto=udp rx=%llu tx=0 closed_by=listener", order, rtp_rel(), l.port,
                        (unsigned long long)flow.rx);
    rtp_close(l.s);
    rtp_live(-1);
    return 0;
}

/* Start one listener on bind_ip:port (port 0 = any; the bound port is returned). Refuses any bind_ip but 127.0.0.1.
 * Returns the bound port, or -1. */
int kz3_net_passive_listen(const char* bind_ip, int port, int udp)
{
    if (!g_rtp.open || g_rtp.nlisten >= RTP_MAX_LISTEN) return -1;
    if (!rtp_is_loopback(bind_ip)) {
        if (g_rtp.log) g_rtp.log("passive capture: bind address refused (loopback 127.0.0.1 only)");
        return -1;
    }
    if (port < 0 || port > 65535) return -1;
    rtp_sock s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == RTP_BAD) return -1;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(0x7F000001u);
    socklen_t sl = sizeof sa;
    if (bind(s, (struct sockaddr*)&sa, sizeof sa) != 0 || (!udp && listen(s, 8) != 0) ||
        getsockname(s, (struct sockaddr*)&sa, &sl) != 0) { rtp_close(s); return -1; }
    int bound = ntohs(sa.sin_port);
    RtpArg* a = (RtpArg*)malloc(sizeof *a);
    if (!a) { rtp_close(s); return -1; }
    a->s = s; a->port = bound; a->udp = udp;
    rtp_live(1);
    g_rtp.l[g_rtp.nlisten++] = (RtpListener){ s, udp, bound };
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, udp ? rtp_udp_thread : rtp_tcp_thread, a, 0, NULL);
    if (!h) { free(a); rtp_close(s); rtp_live(-1); return -1; }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, udp ? rtp_udp_thread : rtp_tcp_thread, a) != 0) { free(a); rtp_close(s); rtp_live(-1); return -1; }
    pthread_detach(t);
#endif
    return bound;
}

/* Stop every thread (open connections are closed by the listener), then close the file. */
void kz3_net_passive_stop(void)
{
    if (!g_rtp.open) return;
    g_rtp.stop = 1;
    for (int i = 0; i < 300; i++) {
        rtp_lock(); unsigned live = g_rtp.live; rtp_unlock();
        if (!live) break;
        rtp_sleep_ms(10);
    }
    rtp_lock(); g_rtp.open = 0; fclose(g_rtp.f); g_rtp.f = NULL; rtp_unlock();
}

/* Diagnostic, from kz3_net_apply: KZ3_NET_PASSIVE=1 with a loopback server. KZ3_NET_PASSIVE_PORTS = comma list, "u" prefix = UDP
 * (default 10070,10071,10075,10078,10079 tcp and u10070,u10079). */
void kz3_net_passive_from_env(void (*log)(const char*))
{
    const char* out = getenv("KZ3_NET_PASSIVE_OUT");
    if (kz3_net_passive_open_log(out && *out ? out : "work/net/medius-passive.log", log) != 0) return;
    const char* pl = getenv("KZ3_NET_PASSIVE_PORTS");
    if (!pl || !*pl) pl = "10070,10071,10075,10078,10079,u10070,u10079";
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", pl);
    int started = 0;
    for (char* tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        int udp = (*tok == 'u'); long v = strtol(tok + udp, NULL, 10);
        if (v < 1 || v > 65535) continue;
        if (kz3_net_passive_listen("127.0.0.1", (int)v, udp) > 0) started++;
    }
    char m[96]; snprintf(m, sizeof m, "passive capture: %d loopback listener(s), nothing is sent", started);
    if (log) log(m);
}
