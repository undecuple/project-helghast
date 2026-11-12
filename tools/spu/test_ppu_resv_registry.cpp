/* Source: Ratchet & Clank port tests/test_ppu_resv_registry.cpp (MIT; NOTICE credit: Ratchet & Clank port team), unchanged.
 * Host reproduction of reservation-registration capacity loss after repeated
 * reuse of one PPU thread context. No game, generated game code or guest image.
 *
 * The test runner extracts the actual patched runtime registration/invalidation
 * functions into ppu_resv_runtime.inc and uses the actual ppu_context layout.
 * Success requires a writer to invalidate a distinct victim after repeated
 * registration of another context; observing a bounded count alone is not enough.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "runtime/ppu/ppu_context.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "runtime/platform/win32_compat.h"
#endif

extern "C" {
PPU_THREAD_LOCAL ppu_context* g_active_ctx = nullptr;
}
#include "ppu_resv_runtime.inc"

static ppu_context writer, reused, victim, unrelated;
static ppu_context concurrent_victims[8];
static ppu_context capacity_contexts[PPU_RESV_MAX + 1];
static constexpr uint32_t line = 0x1800;
static constexpr uint32_t reserved_word = line + 4;

static void reset_registry()
{
    std::memset(g_resv_ctxs, 0, sizeof g_resv_ctxs);
    g_resv_ctx_n = 0;
    g_active_ctx = nullptr;
    std::memset(&writer, 0, sizeof writer);
    std::memset(&reused, 0, sizeof reused);
    std::memset(&victim, 0, sizeof victim);
    std::memset(&unrelated, 0, sizeof unrelated);
}

static bool run_case(const char* operation, bool churn)
{
    reset_registry();
    ppu_resv_register(&writer);
    ppu_resv_register(&unrelated);
    const unsigned registrations = churn ? PPU_RESV_MAX * 2u : 1u;
    for (unsigned n = 0; n < registrations; n++) ppu_resv_register(&reused);
    ppu_resv_register(&victim);
    writer.reserve_addr = reserved_word;
    victim.reserve_addr = reserved_word;
    reused.reserve_addr = PPU_RESV_INVALID;
    unrelated.reserve_addr = line + 128;
    g_active_ctx = &writer;

    if (!std::strcmp(operation, "line")) ppu_resv_break_line(line);
    else if (!std::strcmp(operation, "store")) ppu_resv_break_store(reserved_word);
    else ppu_resv_break(reserved_word);

    const bool invalidated = victim.reserve_addr == PPU_RESV_INVALID;
    const bool unrelated_preserved = unrelated.reserve_addr == line + 128;
    const bool writer_preserved = std::strcmp(operation, "line") || writer.reserve_addr == reserved_word;
    const bool ok = invalidated && unrelated_preserved && writer_preserved;
    std::printf("%s: %s invalidation after %u reused-context registrations; "
                "victim=0x%llX, registry count=%ld%s\n",
                ok ? "PASS" : "FAIL", operation, registrations,
                (unsigned long long)victim.reserve_addr, (long)g_resv_ctx_n,
                invalidated ? "" : " (distinct victim is still reserved)");
    return ok;
}

static bool concurrent_registration()
{
    reset_registry();
    ppu_resv_register(&writer);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; i++) {
        concurrent_victims[i].reserve_addr = reserved_word;
        workers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned n = 0; n < 256; n++) ppu_resv_register(&reused);
            ppu_resv_register(&concurrent_victims[i]);
        });
    }
    while (ready.load(std::memory_order_acquire) != 8) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    g_active_ctx = &writer;
    ppu_resv_break_line(line);
    bool ok = (long)g_resv_ctx_n == 10;  /* writer, one reused slot, eight distinct victims */
    for (const auto& context : concurrent_victims) ok = ok && context.reserve_addr == PPU_RESV_INVALID;
    std::printf("%s: concurrent duplicate registration preserves all eight victims' invalidation; count=%ld\n",
                ok ? "PASS" : "FAIL", (long)g_resv_ctx_n);
    return ok;
}

static bool full_capacity()
{
    reset_registry();
    for (unsigned i = 0; i < PPU_RESV_MAX; i++) {
        capacity_contexts[i].reserve_addr = reserved_word;
        ppu_resv_register(&capacity_contexts[i]);
    }
    /* The same 128 contexts still fit, even after repeated registration. */
    for (unsigned n = 0; n < 2; n++)
        for (unsigned i = 0; i < PPU_RESV_MAX; i++) ppu_resv_register(&capacity_contexts[i]);
    g_active_ctx = &writer;
    ppu_resv_break_line(line);
    bool ok = (long)g_resv_ctx_n == PPU_RESV_MAX;
    for (unsigned i = 0; i < PPU_RESV_MAX; i++)
        ok = ok && capacity_contexts[i].reserve_addr == PPU_RESV_INVALID;
    std::printf("%s: all 128 distinct contexts fit and invalidate after duplicate registration; count=%ld\n",
                ok ? "PASS" : "FAIL", (long)g_resv_ctx_n);
    return ok;
}

static int overflow_child()
{
    reset_registry();
    for (unsigned i = 0; i <= PPU_RESV_MAX; i++) ppu_resv_register(&capacity_contexts[i]);
    std::fprintf(stderr, "FAIL: registry silently accepted an untracked distinct context beyond capacity\n");
    return 1;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "all";
    if (!std::strcmp(mode, "overflow-child")) return overflow_child();
    if (std::strcmp(mode, "all") && std::strcmp(mode, "baseline") && std::strcmp(mode, "churn")
        && std::strcmp(mode, "concurrent") && std::strcmp(mode, "capacity")) {
        std::fprintf(stderr, "usage: test_ppu_resv_registry [all|baseline|churn|concurrent|capacity]\n");
        return 2;
    }
    int failed = 0;
    const char* operations[] = {"line", "store", "word"};
    if (!std::strcmp(mode, "all") || !std::strcmp(mode, "baseline") || !std::strcmp(mode, "churn")) {
        for (const char* op : operations) {
            if (std::strcmp(mode, "churn")) failed += !run_case(op, false);
            if (std::strcmp(mode, "baseline")) failed += !run_case(op, true);
        }
    }
    if (!std::strcmp(mode, "all") || !std::strcmp(mode, "concurrent")) failed += !concurrent_registration();
    if (!std::strcmp(mode, "all") || !std::strcmp(mode, "capacity")) failed += !full_capacity();
    return failed ? 1 : 0;
}
