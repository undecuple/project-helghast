// Host test for KZ3 patch 2082 (see test_ppu_store_note.sh). The three .inc files are the patch's own blocks,
// copied out of the patched ppu_loader.cpp and ppu_lifter.py; everything else here is a stand-in: a 4 GB-indexed
// guest page table, a small vm_base, the renderer hook as a 64 KB page bitmap like rsx_live_draw.c's ld_td_mark.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" { unsigned char g_ppu_vm_page[65536]; }
static uint8_t* vm_base;
static const uint32_t GUEST_LO = 0xBFF00000u, GUEST_SPAN = 0x00300000u;   // 1 MB below + 2 MB of the local window
static uint8_t g_mem[GUEST_SPAN + 256];

#include "gate.inc"
#include "hook.inc"
#include "lifter.inc"

// The renderer side (cellGcmSys.c gcm_ppu_note_pages_ea -> ld_td_mark): one bit per 64 KB page of location 0.
static const uint32_t LOCAL = 0xC0000000u, LOCAL_SIZE = 0x10000000u;
static volatile uint32_t g_td[4096 / 32];
static unsigned long long g_hook_calls;
static void hook(uint32_t ea, uint32_t size)
{
    g_hook_calls++;
    const uint32_t off = ea - LOCAL;
    if (off >= LOCAL_SIZE) return;
    for (uint32_t p = off >> 16; p <= (off + size - 1u) >> 16 && p < 4096u; p++) {
        const uint32_t bit = 1u << (p & 31u);
        if (!(g_td[p >> 5] & bit)) __atomic_or_fetch(&g_td[p >> 5], bit, __ATOMIC_RELAXED);
    }
}
static int td_test(uint32_t page) { return (g_td[page >> 5] >> (page & 31u)) & 1u; }
static void td_clear(void) { for (auto& w : g_td) w = 0; }

// vm_write64's tail as patched (VM_WRITE_COH without the SPU reservation branch, then the gate), and a plain one.
static __attribute__((noinline)) void st64(uint64_t a, uint64_t v)
{
    v = __builtin_bswap64(v); memcpy(vm_base + (uint32_t)a, &v, 8); VM_STORE_NOTE(a, 8);
}
static __attribute__((noinline)) void st64_plain(uint64_t a, uint64_t v)
{
    v = __builtin_bswap64(v); memcpy(vm_base + (uint32_t)a, &v, 8);
}
static __attribute__((noinline)) void stvx(uint64_t ea, const uint8_t* vr)   // the lifter's stvx, as emitted
{
    ea &= ~0xFULL; memcpy(vm_base + (uint32_t)ea, vr, 16); PPU_ST_NOTE(ea, 16);
}

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "[test_ppu_store_note] FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

template <class F> static double ns_per_store(F f, uint32_t base, uint32_t n)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t r = 0; r < 64; r++)
        for (uint32_t i = 0; i < n; i++) f(base + ((i * 8u) & 0xFFFF8u), (uint64_t)i * r);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / (64.0 * n);
}

int main()
{
    vm_base = (uint8_t*)((uintptr_t)g_mem - GUEST_LO);   // guest GUEST_LO.. maps to g_mem (4 GB reservation stand-in)
    ppu_set_store_note_hook(hook);
    ppu_store_note_pages(LOCAL, LOCAL_SIZE, 1);
    CHECK(g_ppu_vm_page[LOCAL >> 16] == PPU_VM_PAGE_NOTE && g_ppu_vm_page[0xCFFF] == PPU_VM_PAGE_NOTE &&
          g_ppu_vm_page[0xBFFF] == 0 && g_ppu_vm_page[0xD000] == 0, "NOTE bits outside/inside the window");

    // Main memory: no note. Local window: the page of the store, nothing else.
    st64(0xBFF10000u, 1); CHECK(g_hook_calls == 0, "main-memory store called the hook");
    st64(LOCAL + 0x10008u, 0x1122334455667788ull);
    CHECK(g_hook_calls == 1 && td_test(1) && !td_test(0) && !td_test(2), "local store page 1");
    CHECK(vm_base[LOCAL + 0x10008u] == 0x11, "stored bytes (big-endian) before the note");
    uint8_t vr[16]; for (int i = 0; i < 16; i++) vr[i] = (uint8_t)i;
    td_clear(); stvx(LOCAL + 0x1FFF7u, vr);
    CHECK(td_test(1) && !td_test(2) && g_hook_calls == 2, "stvx aligns down and notes page 1");
    // A store crossing into the next page notes both (the hook gets the size).
    td_clear(); st64(LOCAL + 0x1FFFCu, 2); CHECK(td_test(1) && td_test(2), "page-crossing store notes both pages");
    // Other page bits (1091's RD) are left alone; switching the window off clears NOTE only.
    __atomic_or_fetch(&g_ppu_vm_page[LOCAL >> 16], 1u, __ATOMIC_RELAXED);
    ppu_store_note_pages(LOCAL, LOCAL_SIZE, 0);
    CHECK(g_ppu_vm_page[LOCAL >> 16] == 1u, "off keeps RD");
    const unsigned long long c0 = g_hook_calls; st64(LOCAL + 0x20000u, 3);
    CHECK(g_hook_calls == c0, "store noted after the window was switched off");
    g_ppu_vm_page[LOCAL >> 16] = 0;

    // Cost: 8-byte stores over 1 MB, 64 passes. Main memory with and without the gate; local window with notes.
    const uint32_t n = 1u << 17;
    const double plain = ns_per_store(st64_plain, 0xBFF00000u, n);
    const double gated = ns_per_store(st64, 0xBFF00000u, n);
    ppu_store_note_pages(LOCAL, LOCAL_SIZE, 1);
    const double noted = ns_per_store(st64, LOCAL, n);
    printf("[test_ppu_store_note] ns/store: plain %.2f, gated main memory %.2f (+%.2f), local window noted %.2f "
           "(+%.2f); hook calls %llu\n", plain, gated, gated - plain, noted, noted - plain, g_hook_calls);
    if (g_fail) { printf("[test_ppu_store_note] FAIL (%d)\n", g_fail); return 1; }
    printf("[test_ppu_store_note] PASS\n");
    return 0;
}
