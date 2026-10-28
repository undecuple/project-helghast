/* KZ3 stream packet balance probe (SPU pass 50; RE pass 109 s.1/s.2 probe 1, the quit-to-menu hang): KZ3_PKT_PROBE=1.
 *
 * Hooks (1.00 only; rows in port/spu/ppu_overrides.txt, glue in kz3_ppu_overrides.cpp):
 *   0xD9A908 MakePacket(out, slot, arg): takes pool->arr[used++] (pool = 0x15E27E8, the word in the TOC cell -0x65F8:
 *            +4 packet pointer array, +8 capacity, +0xC used; disassembly 0xD9A968..0xD9A9A4 this round), refcount 1,
 *            pkt+0xC = slot, pkt+8 = arg; the packet is *out after the call.
 *   0xD9A050 Packet::Release(pkt) (vtable 0x12D1EB0 slot +8, last reference): if pkt+0xC (slot) and the TOC word
 *            -0x65B4 are non-zero and pkt+0x14 (n) != 0 -> slot->vtbl[+0xC](slot, n) (AdvanceReadPos); then
 *            pool->arr[--used] = pkt.
 * Kept: per-slot made/released counts and released bytes; a table of live packets (made, not released) with their make
 * time; a summary every KZ3_PKT_PROBE_S seconds (default 5) when something changed or a live packet is older than 2 s:
 * totals, pool used/cap, per-slot rows, the oldest live packets with their words +0..+0x1C. Read-only on guest memory. */
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

#define POOL 0x15E27E8u
#define NSLOT 32
#define NLIVE 512

static int g_on = -1;
static volatile long g_lock;
static long g_made, g_rel, g_rel_noslot, g_make_fail;
static struct {
    uint32_t slot;
    long made, rel;
    unsigned long long bytes;
    unsigned long long t_make, t_rel;
} g_slot[NSLOT];
static struct {
    uint32_t pkt, slot;
    unsigned long long t;
} g_live[NLIVE];
static long g_changes;

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
/* `out` of MakePacket is usually on a PPU thread stack (0xD0000000 region): pass 50's first T7 run counted every make
 * as a failure with the heap-only check above. */
static int guest_ok_any(uint32_t ea)
{
    return ea >= 0x10000u && ea < 0xF0000000u;
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

static int slot_index(uint32_t slot)
{
    for (int i = 0; i < NSLOT; i++) {
        if (g_slot[i].slot == slot) return i;
        if (!g_slot[i].slot) {
            g_slot[i].slot = slot;
            return i;
        }
    }
    return -1;
}

static void summary(void)
{
    const unsigned long long t = now_ms();
    fprintf(stderr,
            "[pkt-probe] t=%llu made=%ld released=%ld (no slot %ld) make-fail=%ld pool used=%u cap=%u tocw=0x%08X\n", t,
            g_made, g_rel, g_rel_noslot, g_make_fail, rd32(POOL + 0xC), rd32(POOL + 8), rd32(0x132C960u - 0x65B4u));
    for (int i = 0; i < NSLOT && g_slot[i].slot; i++)
        fprintf(stderr,
                "[pkt-probe]   slot 0x%08X vt=0x%08X made=%ld rel=%ld bytes=%llu last make -%llu ms rel -%llu ms "
                "+70=%u +74=%u +78=%u +28=%u\n",
                g_slot[i].slot, guest_ok(g_slot[i].slot) ? rd32(g_slot[i].slot) : 0, g_slot[i].made, g_slot[i].rel,
                g_slot[i].bytes, g_slot[i].t_make ? t - g_slot[i].t_make : 0, g_slot[i].t_rel ? t - g_slot[i].t_rel : 0,
                guest_ok(g_slot[i].slot) ? rd32(g_slot[i].slot + 0x70) : 0,
                guest_ok(g_slot[i].slot) ? rd32(g_slot[i].slot + 0x74) : 0,
                guest_ok(g_slot[i].slot) ? rd32(g_slot[i].slot + 0x78) : 0,
                guest_ok(g_slot[i].slot) ? rd32(g_slot[i].slot + 0x28) : 0);
    int shown = 0, live = 0;
    for (int i = 0; i < NLIVE; i++) {
        if (!g_live[i].pkt) continue;
        live++;
        if (t - g_live[i].t < 2000 || shown >= 12) continue;
        shown++;
        const uint32_t p = g_live[i].pkt;
        fprintf(stderr,
                "[pkt-probe]   live pkt 0x%08X slot 0x%08X age %llu ms: %08X %08X %08X %08X %08X %08X %08X %08X\n", p,
                g_live[i].slot, t - g_live[i].t, rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 0xC), rd32(p + 0x10),
                rd32(p + 0x14), rd32(p + 0x18), rd32(p + 0x1C));
    }
    fprintf(stderr, "[pkt-probe]   live=%d (older than 2 s: %d shown)\n", live, shown);
    { /* the pool's first 8 packet pointers and their refcounts (MakePacket takes arr[used], Release puts back at --used) */
        const uint32_t arr = rd32(POOL + 4);
        char line[400];
        int k = snprintf(line, sizeof line, "[pkt-probe]   pool arr 0x%08X:", arr);
        for (unsigned i = 0; i < 8 && guest_ok(arr) && k < (int)sizeof line - 24; i++) {
            const uint32_t p = rd32(arr + 4 * i);
            k += snprintf(line + k, sizeof line - (size_t)k, " %08X(r%u)", p, guest_ok(p) ? rd32(p + 4) : 0);
        }
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);
}

#ifdef _WIN32
static DWORD WINAPI probe_thread(LPVOID p)
#else
static void* probe_thread(void* p)
#endif
{
    (void)p;
    const char* s = getenv("KZ3_PKT_PROBE_S");
    unsigned period = s ? (unsigned)atoi(s) : 5;
    if (period < 1) period = 1;
    long last = -1;
    for (;;) {
        sleep_ms(period * 1000u);
        lock();
        int old = 0;
        const unsigned long long t = now_ms();
        for (int i = 0; i < NLIVE; i++)
            if (g_live[i].pkt && t - g_live[i].t >= 2000) {
                old = 1;
                break;
            }
        if (g_changes != last || old) summary();
        last = g_changes;
        unlock();
    }
    return 0;
}

int kz3_pkt_probe_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_PKT_PROBE");
        g_on = e && *e && *e != '0';
        if (g_on) {
#ifdef _WIN32
            CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
#else
            pthread_t t;
            pthread_create(&t, NULL, probe_thread, NULL);
#endif
        }
    }
    return g_on;
}

/* After MakePacket(out, slot, arg) returned. */
void kz3_pkt_probe_make(uint32_t out, uint32_t slot)
{
    const uint32_t pkt = guest_ok_any(out) ? rd32(out) : 0;
    const unsigned long long t = now_ms();
    lock();
    g_changes++;
    if (!pkt) {
        g_make_fail++;
        unlock();
        return;
    }
    g_made++;
    if (g_made <= 16) /* the first packets' EAs (for PPU_CASWATCH=<line> in a later run: who drops the reference) */
        fprintf(stderr, "[pkt-probe] make #%ld pkt=0x%08X slot=0x%08X arg=0x%08X refs=%u pool used=%u\n", g_made, pkt,
                slot, rd32(pkt + 8), rd32(pkt + 4), rd32(POOL + 0xC));
    const int s = slot_index(slot);
    if (s >= 0) {
        g_slot[s].made++;
        g_slot[s].t_make = t;
    }
    for (int i = 0; i < NLIVE; i++)
        if (!g_live[i].pkt || g_live[i].pkt == pkt) {
            g_live[i].pkt = pkt;
            g_live[i].slot = slot;
            g_live[i].t = t;
            break;
        }
    unlock();
}

/* Before Packet::Release(pkt) runs (it clears nothing we read, but the pool reuses the packet afterwards). */
void kz3_pkt_probe_release(uint32_t pkt)
{
    const uint32_t slot = guest_ok(pkt) ? rd32(pkt + 0xC) : 0;
    const uint32_t n = guest_ok(pkt) ? rd32(pkt + 0x14) : 0;
    const unsigned long long t = now_ms();
    lock();
    g_changes++;
    g_rel++;
    if (!slot) g_rel_noslot++;
    const int s = slot ? slot_index(slot) : -1;
    if (s >= 0) {
        g_slot[s].rel++;
        g_slot[s].bytes += n;
        g_slot[s].t_rel = t;
    }
    for (int i = 0; i < NLIVE; i++)
        if (g_live[i].pkt == pkt) {
            g_live[i].pkt = 0;
            break;
        }
    unlock();
}
