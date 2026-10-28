/* KZ3 Asset Streaming idle-wait probe (SPU pass 48; RE pass 106, the RE notes "Quit to the main menu"):
 * KZ3_STREAM_PROBE=1.
 *
 * After Quit/Yes the main thread drains the Asset Streaming queue (0x316804) while the Asset Streaming Thread sits in
 * 0x300250 (re-data: WaitUntilIdle/shutdown): under the lwmutex M+8 (M = *0x1585F68) it sums, over the entries
 * i < [M+0x24] at [M+0x20] + i*0x48, the length of the circular intrusive list whose head pointer is at entry+0x24
 * (node -> next at node+4, stop when next == head) plus 1 when the word at 0xE1D428 (entry+0x10 != 0) or 0xE1F318
 * (entry+0x10 == 0) is non-zero; non-zero -> usleep(1 ms), again. (Disassembly 0x300330..0x300410, this round.)
 * The override on 0x300250 (kz3_ppu_overrides.cpp) marks entry/exit; this thread dumps M, every entry (18 words), its
 * list nodes (8 words each) and the two flag words every KZ3_STREAM_PROBE_S seconds (default 10) while the wait lasts
 * longer than that, so the request that never completes is visible. Read-only on guest memory. 1.00 addresses only
 * 1.14 (SPU pass 53, RE pass 116 s.36.12; loads re-read with rizin on EBOOT-114.elf 0x30BD28..0x30BD9C): the twin
 * 0x30BC20 reads M = *0x15FB668 and the flag words 0xE97128 (1.00 0xE1D428) / 0xE99018 (1.00 0xE1F318); the device
 * globals are taken as M's neighbours +8/+0x10 as on 1.00 (ASSUMPTION: same layout, not read). The cpp hook passes the
 * version on entry. */
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

static int g_on = -1;
static volatile int g_inside;
static volatile uint32_t g_self;
static volatile unsigned long long g_t_enter;
static unsigned g_dumps;

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

static void words(const char* tag, uint32_t ea, unsigned n)
{
    char line[400];
    int k = snprintf(line, sizeof line, "[stream-probe]   %s 0x%08X:", tag, ea);
    for (unsigned i = 0; i < n && k < (int)sizeof line - 12; i++)
        k +=
            snprintf(line + k, sizeof line - (size_t)k, " %08X", guest_ok(ea + 4 * i) ? rd32(ea + 4 * i) : 0xBADBAD00u);
    fprintf(stderr, "%s\n", line);
}

/* Version-dependent globals (1.00 defaults; kz3_stream_idle_enter(.., 1) switches to 1.14). */
static uint32_t g_m_ea = 0x1585F68u, g_fw_set = 0xE1D428u, g_fw_clr = 0xE1F318u, g_fn = 0x300250u;

static void dump(void)
{
    const uint32_t M = rd32(g_m_ea);
    const unsigned long long dt = now_ms() - g_t_enter;
    fprintf(stderr, "[stream-probe] dump #%u: 0x%X this=0x%08X for %llu ms; M=0x%08X [0x%X]=0x%08X [0x%X]=0x%08X\n",
            ++g_dumps, g_fn, g_self, dt, M, g_fw_set, rd32(g_fw_set), g_fw_clr, rd32(g_fw_clr));
    words("this", g_self, 16);
    words("globals M", g_m_ea, 8);
    if (!guest_ok(M)) return;
    words("M", M, 16);
    const uint32_t base = rd32(M + 0x20);
    const int n = (int)rd32(M + 0x24);
    fprintf(stderr, "[stream-probe]   entries: base=0x%08X count=%d\n", base, n);
    if (!guest_ok(base) || n < 0 || n > 256) return;
    unsigned busy_total = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t e = base + (uint32_t)i * 0x48u;
        const uint32_t flag = rd32(e + 0x10), head = rd32(e + 0x24);
        /* the game's count: nodes until next == head */
        unsigned len = 0;
        uint32_t node = head;
        while (guest_ok(node) && len < 64) {
            const uint32_t next = rd32(node + 4);
            if (next == head) break;
            len++;
            node = next;
        }
        const uint32_t fw = rd32(flag ? g_fw_set : g_fw_clr);
        const unsigned busy = len + (fw != 0);
        busy_total += busy;
        fprintf(stderr, "[stream-probe]   entry %d @0x%08X: +0x10=0x%08X head=0x%08X len=%u flagword=0x%08X busy=%u\n",
                i, e, flag, head, len, fw, busy);
        if (!busy) continue;
        words("  entry", e, 18);
        node = head;
        for (unsigned k = 0; k < 8 && guest_ok(node); k++) {
            char tag[32];
            snprintf(tag, sizeof tag, "  node %u", k);
            words(tag, node, 8);
            const uint32_t next = rd32(node + 4);
            if (next == head) break;
            node = next;
        }
    }
    /* pass 48 q2: entry 1's one node pointed at the object *0x1585F78 (+8) and a heap object (+0x1C); dump the two
     * device objects behind the globals and the first busy node in full so their classes/vtables can be read. */
    words("device0 *(M+8)", rd32(g_m_ea + 8), 24);
    words("device1 *(M+0x10)", rd32(g_m_ea + 0x10), 24);
    for (int i = 0; i < n; i++) {
        const uint32_t e = base + (uint32_t)i * 0x48u, head = rd32(e + 0x24);
        if (!guest_ok(head) || head == e + 0x20) continue;
        char tag[40];
        snprintf(tag, sizeof tag, "  entry %d node", i);
        words(tag, head, 32);
        for (unsigned k = 2; k < 8; k++) { /* every heap pointer in the node's first 8 words: 16 words of it */
            const uint32_t v = rd32(head + 4 * k);
            if (v >= 0x40000000u && v < 0x50000000u && guest_ok(v)) {
                snprintf(tag, sizeof tag, "    node+0x%X ->", 4 * k);
                words(tag, v, 16);
            }
        }
        break;
    }
    fprintf(stderr, "[stream-probe]   busy sum=%u\n", busy_total);
    fflush(stderr);
}

#ifdef _WIN32
static DWORD WINAPI probe_thread(LPVOID p)
#else
static void* probe_thread(void* p)
#endif
{
    (void)p;
    const char* s = getenv("KZ3_STREAM_PROBE_S");
    unsigned period = s ? (unsigned)atoi(s) : 10;
    if (period < 1) period = 1;
    for (;;) {
        sleep_ms(1000);
        if (!g_inside) continue;
        if (now_ms() - g_t_enter < (unsigned long long)period * 1000ull) continue;
        dump();
        sleep_ms(period * 1000 - 1000);
    }
    return 0;
}

static int on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_STREAM_PROBE");
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

void kz3_stream_idle_enter(uint32_t self, int v114)
{
    if (!on()) return;
    if (v114) {
        g_m_ea = 0x15FB668u;
        g_fw_set = 0xE97128u;
        g_fw_clr = 0xE99018u;
        g_fn = 0x30BC20u;
    }
    g_self = self;
    g_t_enter = now_ms();
    g_inside = 1;
    fprintf(stderr, "[stream-probe] 0x%X enter this=0x%08X +8=%u +10=%u M=0x%08X\n", g_fn, self,
            guest_ok(self) ? vm_base[self + 8] : 0, guest_ok(self) ? vm_base[self + 10] : 0, rd32(g_m_ea));
}

void kz3_stream_idle_exit(uint32_t self, uint32_t ret)
{
    if (!on()) return;
    g_inside = 0;
    fprintf(stderr, "[stream-probe] 0x%X exit this=0x%08X ret=%u after %llu ms\n", g_fn, self, ret,
            now_ms() - g_t_enter);
}
