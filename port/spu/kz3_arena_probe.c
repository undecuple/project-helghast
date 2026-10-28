/* KZ3 frame-arena probe (SPU pass 35; RE pass 74, the RE notes 11.5): KZ3_ARENA_PROBE=1.
 *
 * Candidate 1 of 11.3: a frame-arena chunk holding frame N's occluder list / geometry is reused by frame N+1 while
 * frame N's SPU block chain (raster 44/45) still reads it. On the PS3 a chunk becomes takeable only when the RSX has
 * executed BeginFrame(N+1)'s label (epoch N+1 in the label word at *[arena+0x18]); on the port the RSX backend
 * completes that label on its own clock while the lifted jobs run 10-40x slower, so the window grows. Two hooks:
 *  - 0x5B9F0(arena, stamp, wait) [1.14 0x5C2D0], the chunk take: the pre-hook snapshots the stamp table, the
 *    post-hook finds the taken chunk (r3 = chunk EA, 0 = none) and logs its OLD table word (whose epoch says which
 *    frame used it last), the label value, the arena epoch word, the free counter [arena+0x20], the frame counter
 *    [[0x133163C]+0x10+0x5A8] and the time spent (the usleep-30 spin when no chunk is free). Only the frame arena
 *    G+0x50 (G = [0x133163C]) is logged in detail; other arenas are counted.
 *  - 0x1D8B08(block, obj, tmp, view, X+0xC3E0, X+0xC670), the view's job builder [1.14 unknown]: it starts with the
 *    spin on the chain counter block+0x8A0 (0x1D8B64), so the counter's value at entry says whether the previous
 *    frame's chain was still running when this frame started (nonzero = the candidate-1 window is open) and the
 *    time in the function is the spin.
 * Summary every 10 s: takes of the frame arena, takes whose old word is the previous epoch (frame N's chunk reused
 * at N+1: the suspicious case), older, spins; builder calls, entries with a running chain, max/mean spin ms.
 * 1.00 addresses in the rows; 1.14 twins: 0x1E4278 / 0x5C2D0 and G = 0x13A56BC (the RE notes s.35, s.36.7). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;

static uint32_t g_arena_g = 0x133163Cu; /* Renderer singleton pointer (1.00; 11.5.1; 1.14 0x13A56BC set by the cpp); R = G+0x10, frame arena = G+0x50 */
void kz3_arena_probe_set_g(uint32_t g) { g_arena_g = g; }
#define KZ3_ARENA_G g_arena_g
#define KZ3_ARENA_MAX_CHUNKS 256

static int g_on = -1;
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
static int64_t now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (int64_t)(c.QuadPart * 1000000 / f.QuadPart);
#else
    return 0;
#endif
}
static int on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_ARENA_PROBE");
        g_on = (e && atoi(e) > 0) ? 1 : 0;
    }
    return g_on && vm_base;
}

/* ---- 0x5B9F0 ---- */
static __thread uint32_t t_snap[KZ3_ARENA_MAX_CHUNKS];
static __thread uint32_t t_count;
static __thread int64_t t_t0;
static unsigned g_takes, g_takes_other, g_prev_epoch, g_older, g_spins, g_lines, g_none;
static int64_t g_last_sum_us;
static unsigned g_max_spin_us;

void kz3_arena_probe_pre(uint32_t arena)
{
    if (!on() || !guest_ok(arena)) return;
    uint32_t count = rd32(arena), table = rd32(arena + 8);
    if (count > KZ3_ARENA_MAX_CHUNKS) count = KZ3_ARENA_MAX_CHUNKS;
    t_count = 0;
    if (guest_ok(table)) {
        for (uint32_t i = 0; i < count; i++) t_snap[i] = rd32(table + 4 * i);
        t_count = count;
    }
    t_t0 = now_us();
}

void kz3_arena_probe_post(uint32_t arena, uint32_t stamp, uint32_t wait, uint32_t chunk)
{
    if (!on() || !guest_ok(arena)) return;
    int64_t t1 = now_us();
    unsigned spent = (unsigned)(t1 - t_t0);
    uint32_t G = rd32(KZ3_ARENA_G);
    int frame_arena = guest_ok(G) && arena == G + 0x50;
    if (!frame_arena) {
        g_takes_other++;
        return;
    }
    uint32_t base = rd32(arena + 0xC), size = rd32(arena + 4), label_ea = rd32(arena + 0x18);
    uint32_t L = guest_ok(label_ea) ? rd32(label_ea) : 0xFFFFFFFFu;   /* label EA printed since pass 35b (unbuilt) */
    uint32_t epoch = rd32(arena + 0x1C), free_ea = rd32(arena + 0x20);
    uint32_t freec = guest_ok(free_ea) ? rd32(free_ea) : 0xFFFFFFFFu;
    uint32_t frame = rd32(G + 0x10 + 0x5A8);
    uint32_t idx = 0xFFFFFFFFu, old = 0;
    if (chunk && size && chunk >= base) {
        idx = (chunk - base) / size;
        old = idx < t_count ? t_snap[idx] : 0;
    }
    if (spent > 1000) {
        g_spins++;
        if (spent > g_max_spin_us) g_max_spin_us = spent;
    }
    if (!chunk)
        g_none++;
    else {
        g_takes++;
        unsigned oe = old >> 28, ne = epoch >> 28;
        if (oe == ((ne + 15) & 15))
            g_prev_epoch++;
        else
            g_older++;
    }
    if (g_lines < 60 || (g_takes % 200) == 0) {
        g_lines++;
        fprintf(stderr,
                "[arena-probe] frame=%u take chunk=%u ea=0x%08X old=0x%08X (epoch %u) label=0x%08X (epoch %u) "
                "now epoch %u stamp=0x%08X free=%u wait=%u spent=%u us label_ea=0x%08X\n",
                frame, idx, chunk, old, old >> 28, L, L >> 28, epoch >> 28, stamp, freec, wait, spent, label_ea);
    }
    if (t1 - g_last_sum_us >= 10000000) {
        g_last_sum_us = t1;
        fprintf(stderr,
                "[arena-probe] 10s: frame=%u takes=%u (prev-epoch chunk=%u older=%u none=%u) spins>1ms=%u max=%u us "
                "other arenas=%u label=0x%08X free=%u\n",
                frame, g_takes, g_prev_epoch, g_older, g_none, g_spins, g_max_spin_us, g_takes_other, L, freec);
        g_max_spin_us = 0;
    }
}

/* ---- 0x1D8B08 ---- */
static __thread int64_t t_b0;
static __thread uint32_t t_b_counter;
static unsigned g_b_calls, g_b_running, g_b_lines, g_b_max_us;
static double g_b_sum_us;
static int64_t g_b_last_sum_us;

void kz3_arena_probe_builder_pre(uint32_t block)
{
    if (!on() || !guest_ok(block)) return;
    t_b_counter = rd32(block + 0x8A0);
    t_b0 = now_us();
}

void kz3_arena_probe_builder_post(uint32_t block)
{
    if (!on() || !guest_ok(block)) return;
    int64_t t1 = now_us();
    unsigned spent = (unsigned)(t1 - t_b0);
    g_b_calls++;
    g_b_sum_us += spent;
    if (spent > g_b_max_us) g_b_max_us = spent;
    if (t_b_counter) {
        g_b_running++;
        if (g_b_lines < 40) {
            g_b_lines++;
            fprintf(stderr,
                    "[arena-probe] builder: chain counter %u at entry (previous frame's chain still running), "
                    "waited %u us, block=0x%08X\n",
                    t_b_counter, spent, block);
        }
    }
    if (t1 - g_b_last_sum_us >= 10000000) {
        g_b_last_sum_us = t1;
        fprintf(stderr, "[arena-probe] builder 10s: calls=%u chain-running-at-entry=%u mean=%.0f us max=%u us\n",
                g_b_calls, g_b_running, g_b_calls ? g_b_sum_us / g_b_calls : 0.0, g_b_max_us);
        g_b_max_us = 0;
    }
}
