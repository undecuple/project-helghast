// Patch 1091 host test: the lifted code's inline guest loads (ppu_lifter.py preamble block, extracted by
// test_vm_inline.sh into VMI_BLOCK). Self-contained: a 4 GB MAP_NORESERVE guest space, out-of-line vm_read* stubs that
// count per-thread calls and do the plain big-endian load (what the runtime's vm_read* return for a plain address with
// no diagnostic armed: patch 1080's equivalence, the RE notes Pass 62 s.3).
//   1. load equivalence: random EAs over mapped windows, every width and alignment, inline vs vm_read*: 0 differences,
//      and the inline path taken (no slow call) while the gate is open;
//   2. special addresses go to vm_read*: null page, raw SPU window and above, the GCM ref word's page, gate closed;
//   3. poll liveness: a translated-style loop `while (vm_read32(flag) != 1) {}` sees another thread's store;
//      negative control: the same loop with a plain load must hang (the compiler hoists the load at -O2).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern "C" { uint8_t* vm_base; unsigned char g_ppu_vm_page[65536]; }
static const uint32_t INJECT_BASE = 0x20000000u;   // libs/video/cellGcmSys.c ppu_hle_inject_base
static thread_local unsigned long t_slow;
template <typename T> static T be_load(uint64_t a)
{
    uint8_t b[8]; memcpy(b, vm_base + (uint32_t)a, sizeof(T));
    T v = 0; for (unsigned i = 0; i < sizeof(T); i++) v = (T)((v << 8) | b[i]);   // big-endian, byte by byte
    return v;
}
extern "C" {
uint8_t  vm_read8 (uint64_t a) { t_slow++; return be_load<uint8_t>(a); }
uint16_t vm_read16(uint64_t a) { t_slow++; return be_load<uint16_t>(a); }
uint32_t vm_read32(uint64_t a) { t_slow++; return be_load<uint32_t>(a); }
uint64_t vm_read64(uint64_t a) { t_slow++; return be_load<uint64_t>(a); }
}
#include VMI_BLOCK
#ifndef vm_read32
#error "patch 1091 block did not define the inline loads"
#endif

// gate open/closed exactly as ppu_loader.cpp's vm_inline_open sets it (RD on pages 1..0xDFFF but the ref word's)
static void gate(int open)
{
    memset(g_ppu_vm_page, 0, sizeof g_ppu_vm_page);
    if (open) for (uint32_t pg = 1; pg < 0xE000u; pg++) if (pg != ((INJECT_BASE + 0x2008u) >> 16)) g_ppu_vm_page[pg] |= 1;
}
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 10) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static int load_eq(uint32_t ea, int expect_inline, unsigned long* diff)
{
    unsigned long s0 = t_slow, slow_expected = 0;
    if (vm_read8(ea)  != be_load<uint8_t>(ea))  (*diff)++;
    if (vm_read16(ea) != be_load<uint16_t>(ea)) (*diff)++;
    if (vm_read32(ea) != be_load<uint32_t>(ea)) (*diff)++;
    if (vm_read64(ea) != be_load<uint64_t>(ea)) (*diff)++;
    slow_expected = expect_inline ? 0 : 4;
    return t_slow - s0 == slow_expected;
}

static volatile int g_done_inline, g_done_plain;
static uint32_t g_flag_ea;
__attribute__((noinline)) static void poll_inline(uint32_t ea) { while (vm_read32(ea) != 1) {} }
// Negative control: the same poll with a plain load, bounded so a hoisted load cannot hang the test forever. A
// side-effect-free unbounded loop is deleted outright by GCC at -O2 (C++ forward progress); an asm statement in the
// loop acts as a compiler barrier and keeps the reload. Hoisted, the loop either computes i = PLAIN_MAX at once or
// spins to it; it never sees the store.
static const unsigned long PLAIN_MAX = 1ul << 40;
static volatile unsigned long g_plain_i;
__attribute__((noinline)) static unsigned long poll_plain(uint32_t ea)
{
    const uint32_t* p = (const uint32_t*)(vm_base + ea);
    unsigned long i = 0;
    while (__builtin_bswap32(*p) != 1 && i < PLAIN_MAX) i++;
    return i;
}
static void* th_inline(void*) { poll_inline(g_flag_ea); g_done_inline = 1; return 0; }
static void* th_plain(void*)  { g_plain_i = poll_plain(g_flag_ea + 0x40); g_done_plain = 1; return 0; }
static void msleep(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, 0); }

int main()
{
    void* m = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { puts("SKIP: cannot reserve a 4 GB guest space"); return 0; }
    vm_base = (uint8_t*)m;
    const uint32_t ref = INJECT_BASE + 0x2008u, refpg = ref & ~0xFFFFu;

    // 1. equivalence. Windows: 96 random 16 KB windows + both ends of the inline range, random bytes.
    uint32_t win[100]; int nw = 0;
    win[nw++] = 0x10000u; win[nw++] = 0xE0000000u - 0x4000u; win[nw++] = 0x1FFFC000u; win[nw++] = 0xC0000000u;
    while (nw < 100) win[nw++] = (uint32_t)(0x10000u + rnd() % (0xE0000000u - 0x20000u)) & ~0x3FFFu;
    for (int w = 0; w < nw; w++) for (uint32_t i = 0; i < 0x4000u + 8; i++) vm_base[(uint64_t)win[w] + i] = (uint8_t)rnd();
    gate(1);
    unsigned long diff = 0, n = 0, wrong_route = 0;
    for (int k = 0; k < 2000000; k++) {
        uint32_t ea = win[rnd() % nw] + (uint32_t)(rnd() % 0x4000u);
        if ((ea & ~0xFFFFu) == refpg || ea < 0x10000u || ea >= 0xE0000000u) continue;
        if (!load_eq(ea, 1, &diff)) wrong_route++;
        n++;
    }
    printf("equivalence: %lu EAs x 4 widths (gate open): %lu differ, %lu took vm_read*\n", n, diff, wrong_route);
    CHECK(diff == 0 && wrong_route == 0, "inline loads differ from vm_read* or left the inline path");
    // the same with the gate closed: values equal, every load on vm_read*
    gate(0); diff = 0; wrong_route = 0;
    for (int k = 0; k < 100000; k++) { uint32_t ea = win[rnd() % nw] + (uint32_t)(rnd() % 0x4000u);
                                       if (!load_eq(ea, 0, &diff)) wrong_route++; }
    printf("gate closed: 100000 EAs x 4 widths: %lu differ, %lu bypassed vm_read*\n", diff, wrong_route);
    CHECK(diff == 0 && wrong_route == 0, "gate closed but a load bypassed vm_read*");

    // 2. special addresses with the gate open
    gate(1); diff = 0; unsigned long sp = 0, bad = 0;
    for (uint32_t ea = 0; ea < 0x10000u; ea += 0x7u) { sp++; if (!load_eq(ea, 0, &diff)) bad++; }
    uint32_t hi[] = {0xE0000000u, 0xE0040000u, 0xE0044004u, 0xE0144004u, 0xE0540000u, 0xF0000000u, 0xFFFFFFF0u};
    for (uint32_t ea : hi) { sp++; if (!load_eq(ea, 0, &diff)) bad++; }
    for (int k = 0; k < 10000; k++) { uint32_t ea = 0xE0000000u + (uint32_t)(rnd() % 0x1FFFFFF0u); sp++;
                                      if (!load_eq(ea, 0, &diff)) bad++; }
    sp++; if (!load_eq(ref, 0, &diff)) bad++;
    // the whole ref page is slow; the pages around it inline
    for (uint32_t ea = refpg; ea < refpg + 0x10000u; ea += 0x3Du) { sp++; if (!load_eq(ea, 0, &diff)) bad++; }
    sp++; if (!load_eq(refpg - 8, 1, &diff)) bad++;
    sp++; if (!load_eq(refpg + 0x10000u, 1, &diff)) bad++;
    printf("special addresses: %lu checked, %lu routed wrong, %lu differ\n", sp, bad, diff);
    CHECK(bad == 0 && diff == 0, "a special address took the inline path (or a neighbour left it)");

    // 3. poll liveness + negative control
    g_flag_ea = 0x00200000u;
    memset(vm_base + g_flag_ea, 0, 0x80);
    pthread_t a, b;
    pthread_create(&a, 0, th_inline, 0); pthread_create(&b, 0, th_plain, 0);
    msleep(50);
    const int early = g_done_inline;   // the inline poll may not finish before the store
    __atomic_store_n((uint32_t*)(vm_base + g_flag_ea), __builtin_bswap32(1u), __ATOMIC_RELEASE);
    __atomic_store_n((uint32_t*)(vm_base + g_flag_ea + 0x40), __builtin_bswap32(1u), __ATOMIC_RELEASE);
    int waited = 0; while (!g_done_inline && waited < 5000) { msleep(10); waited += 10; }
    msleep(500);
    printf("poll liveness: inline poll %s (%d ms); plain-load control %s\n",
           g_done_inline ? "saw the store" : "HUNG", waited, !g_done_plain ? "still spinning after the store (hoisted)" :
           g_plain_i >= PLAIN_MAX ? "ran out without the store (hoisted)" : "saw the store (not hoisted)");
    CHECK(!early, "a poll loop finished before the flag was set");
    CHECK(g_done_inline, "inline poll never saw the other thread's store");
    CHECK(!g_done_plain || g_plain_i >= PLAIN_MAX, "negative control finished: the compiler did not hoist the plain load, test has no power");
    pthread_detach(a);
    fflush(stdout);
    _exit(fails ? 1 : 0);   // the control thread spins forever
}
