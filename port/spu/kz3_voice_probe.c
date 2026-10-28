/* KZ3 stream voice ring probe (SPU pass 51; RE pass 112 s.4/s.5 probes 1-4, the quit-to-menu hang): gated by
 * KZ3_PKT_PROBE=1 (kz3_pkt_probe.c's switch).
 *
 * Layouts (RE pass 112 s.4, checked against the decompile of 0xA6674C / 0xA66928 / 0xA66F8C / 0xA67450 this round):
 *   voice+0x4C ring: +0 ring state word (Update writes 2 when the voice state is 1), +8 capacity, +0xC tail (pushed),
 *              +0x10 head (retired), +0x14 flags, +0x1C halfword counter, entries of 0x20 bytes from +0x20.
 *   voice+0x50 ring bytes; voice+0x54 = ring + voice+0x50 = state block: +0 state (1 starting, 6 finishing),
 *              +4 consumed index, +8 flags (0x200), +0x14 halfword counter; voice+0x58 state bytes.
 *   voice+0x60 completion callback (descriptor), voice+0x5C event callback.
 * Hooks (1.00 only): 0xA6674C RetirePackets(voice) entry, 0xA66928 voice Update(voice) entry, 0xA66F8C
 * PacketQueue::Push(voice, entry, flags) -> 1 pushed / 0 full.
 * At the first push of a voice its ring + state block is put in a runtime SPU DMA watch window (patch 1026,
 * slots 4..7, one per voice): every SPU DMA touching it prints a `[dmaw]` line (who reads the entries, who should
 * write the consumed index). Every 5 s, when anything changed or a voice has unretired entries: one line per voice. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

extern uint8_t* vm_base;
extern int kz3_pkt_probe_on(void);
#define SPU_DMA_WATCH_SLOTS 8
extern volatile uint32_t g_spu_dma_watch_lo[SPU_DMA_WATCH_SLOTS], g_spu_dma_watch_hi[SPU_DMA_WATCH_SLOTS];
extern volatile int g_spu_dma_watch_on;

#define NV 16
static struct {
    uint32_t voice;
    long upd, ret, push, push_full;
    int watch;
    uint32_t last[4];
} g_v[NV];
static volatile long g_lock, g_changes;
static int g_next_watch = 4;
static int g_thread;
static int g_drain = -1;
static volatile long g_mark[11];

/* KZ3_VOICE_DRAIN=1 (opt-in workaround, pass 51): voices are tracked without the probe output too. */
int kz3_voice_drain_on(void)
{
    if (g_drain < 0) {
        const char* e = getenv("KZ3_VOICE_DRAIN");
        g_drain = e && *e && *e != '0';
    }
    return g_drain;
}
static int track_on(void)
{
    return kz3_pkt_probe_on() || kz3_voice_drain_on();
}

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static int guest_ok(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0x50000000u;
}
static uint32_t w(uint32_t base, uint32_t off)
{
    return guest_ok(base) && guest_ok(base + off) ? rd32(base + off) : 0xDEADDEADu;
}
static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}
static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}
static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static void voice_line(int i, const char* tag)
{
    const uint32_t v = g_v[i].voice, ring = w(v, 0x4C), st = w(v, 0x54);
    fprintf(stderr,
            "[voice-probe] %s t=%llu voice=0x%08X vt=0x%08X upd=%ld ret=%ld push=%ld full=%ld ring=0x%08X rbytes=0x%X "
            "rstate=%u cap=%u tail=%u head=%u rflags=0x%X r1C=%u | st=0x%08X sbytes=0x%X state=%u consumed=%u "
            "sflags=0x%X s14=%u | cb60=0x%08X cb5C=0x%08X v64=%u\n",
            tag, now_ms(), v, w(v, 0), g_v[i].upd, g_v[i].ret, g_v[i].push, g_v[i].push_full, ring, w(v, 0x50),
            w(ring, 0), w(ring, 8), w(ring, 0xC), w(ring, 0x10), w(ring, 0x14), w(ring, 0x1C) >> 16, st, w(v, 0x58),
            w(st, 0), w(st, 4), w(st, 8), w(st, 0x14) >> 16, w(v, 0x60), w(v, 0x5C),
            guest_ok(v) ? vm_base[v + 0x64] : 0xFFu);
}

#ifdef _WIN32
static DWORD WINAPI probe_thread(LPVOID p)
#else
static void* probe_thread(void* p)
#endif
{
    (void)p;
    long last = -1;
    for (;;) {
        sleep_ms(5000);
        lock();
        int pending = 0;
        for (int i = 0; i < NV && g_v[i].voice; i++) {
            const uint32_t ring = w(g_v[i].voice, 0x4C);
            if (w(ring, 0xC) != w(ring, 0x10)) pending = 1;
        }
        if (g_changes != last || pending) {
            for (int i = 0; i < NV && g_v[i].voice; i++) voice_line(i, "sum");
            fflush(stderr);
        }
        last = g_changes;
        unlock();
    }
    return 0;
}

/* Slot of `voice` (created on first sight); caller holds the lock. */
static int slot(uint32_t voice)
{
    for (int i = 0; i < NV; i++) {
        if (g_v[i].voice == voice) return i;
        if (!g_v[i].voice) {
            g_v[i].voice = voice;
            g_v[i].watch = -1;
            if (!g_thread) {
                g_thread = 1;
#ifdef _WIN32
                CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
#else
                pthread_t t;
                pthread_create(&t, NULL, probe_thread, NULL);
#endif
            }
            return i;
        }
    }
    return -1;
}

/* Prints the voice when head/tail/consumed/state changed since its last print (first 60 such lines per voice). */
static void maybe_line(int i, const char* tag)
{
    const uint32_t v = g_v[i].voice, ring = w(v, 0x4C), st = w(v, 0x54);
    const uint32_t cur[4] = {w(ring, 0xC), w(ring, 0x10), w(st, 4), w(st, 0)};
    if (!memcmp(cur, g_v[i].last, sizeof cur)) return;
    memcpy(g_v[i].last, cur, sizeof cur);
    static long lines[NV];
    if (++lines[i] > 60) return;
    voice_line(i, tag);
    fflush(stderr);
}

/* v114: the hook is the 1.14 twin 0xA7A25C; Update's range 0xA66928..0xA66A40 shifts by +0x13B10 (RE pass 116 s.36.12). */
void kz3_voice_probe_retire(uint32_t voice, uint32_t lr, int v114)
{
    const uint32_t upd = v114 ? 0xA7A438u : 0xA66928u;
    if (!track_on()) return;
    static volatile long n_ret;
    const long nr = __atomic_add_fetch(&n_ret, 1, __ATOMIC_RELAXED);
    if (kz3_pkt_probe_on() && (nr <= 20 || lr < upd || lr >= upd + 0x118u))
        fprintf(stderr, "[voice-probe] RetirePackets t=%llu voice=0x%08X lr=0x%08X\n", now_ms(), voice, lr);
    lock();
    const int i = slot(voice);
    if (i >= 0) {
        g_v[i].ret++;
        maybe_line(i, "retire");
    }
    g_changes++;
    unlock();
}

void kz3_voice_probe_update(uint32_t voice)
{
    if (!track_on()) return;
    lock();
    const int i = slot(voice);
    if (i >= 0) {
        g_v[i].upd++;
        maybe_line(i, "update");
    }
    unlock();
}

/* After Push(voice, entry, flags) returned ret. */
void kz3_voice_probe_push(uint32_t voice, uint32_t entry, uint32_t ret)
{
    if (!track_on()) return;
    lock();
    g_changes++;
    const int i = slot(voice);
    if (i >= 0) {
        g_v[i].push++;
        if (!(ret & 0xFF)) g_v[i].push_full++;
        if (kz3_pkt_probe_on() && g_v[i].watch < 0 && g_next_watch < SPU_DMA_WATCH_SLOTS) {
            const uint32_t ring = w(voice, 0x4C);
            const uint32_t end = ring + w(voice, 0x50) + w(voice, 0x58);
            if (guest_ok(ring) && end > ring && end - ring < 0x10000u) {
                g_v[i].watch = g_next_watch++;
                g_spu_dma_watch_lo[g_v[i].watch] = ring;
                g_spu_dma_watch_hi[g_v[i].watch] = end;
                g_spu_dma_watch_on = 1;
                fprintf(stderr, "[voice-probe] watch w%d = 0x%08X-0x%08X (voice 0x%08X ring + state)\n", g_v[i].watch,
                        ring, end, voice);
            }
        }
        if (g_v[i].push <= 30 || !(ret & 0xFF))
            fprintf(stderr, "[voice-probe] push #%ld t=%llu voice=0x%08X entry=0x%08X pkt=0x%08X ret=%u\n", g_v[i].push,
                    now_ms(), voice, entry, w(entry, 0), ret & 0xFF);
        maybe_line(i, "push");
        fflush(stderr);
    }
    unlock();
}

/* Counts + caller of the other voice paths (kind 0 Reset 0xA671A0, 1 dtor 0xA66D54, 2 dtor 0xA66B00, 3 voice-list
 * update 0xA69110, 4 voice-list update 0xA698A0): the first 10 of each and every 1000th. */
void kz3_voice_probe_mark(int kind, uint32_t self, uint32_t lr)
{
    if (kind < 0 || kind > 10 || !kz3_pkt_probe_on()) return;
    static const char* const nm[11] = {
        "Reset-A671A0", "dtor-A66D54", "dtor-A66B00", "list-A69110",   "list-A698A0",      "?",
        "list-A6F500",  "list-A456A8", "list-A69EE4", "attach-A6A498", "voice-vf50-A65794"};
    const long n = __atomic_add_fetch(&g_mark[kind], 1, __ATOMIC_RELAXED);
    if (n > 10 && n % 1000 != 0) return;
    fprintf(stderr, "[voice-probe] %s #%ld t=%llu this=0x%08X lr=0x%08X\n", nm[kind], n, now_ms(), self, lr);
}

/* KZ3_VOICE_DRAIN=1: called on the main thread after Asset Streaming's shutdown wait 0x300250 cancelled the requests
 * (its second 0xA57B64 call). For every tracked voice whose ring holds entries the voice never retired (pushed >
 * retired, never updated: RE pass 112 s.4, SPU pass 51), it sets the consumed index to the ring tail, as the voice
 * destructors 0xA66B00 / 0xA66D54 and 0xA671A0 do, and returns the voice in out[] so the caller runs RetirePackets on
 * it (completion callback voice+0x60 -> Packet::Release -> AdvanceReadPos). Returns the count. */
int kz3_voice_drain_list(uint32_t* out, int max)
{
    int n = 0;
    lock();
    for (int i = 0; i < NV && g_v[i].voice && n < max; i++) {
        const uint32_t v = g_v[i].voice, ring = w(v, 0x4C), st = w(v, 0x54);
        if (!guest_ok(ring) || !guest_ok(st)) continue;
        const uint32_t tail = rd32(ring + 0xC), head = rd32(ring + 0x10);
        if (tail == head) continue;
        const uint32_t be = __builtin_bswap32(tail);
        memcpy(vm_base + st + 4, &be, 4);
        fprintf(stderr, "[voice-drain] t=%llu voice=0x%08X head=%u tail=%u upd=%ld: consumed := tail, RetirePackets\n",
                now_ms(), v, head, tail, g_v[i].upd);
        out[n++] = v;
    }
    unlock();
    return n;
}
