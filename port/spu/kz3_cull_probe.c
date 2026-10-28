/* KZ3 culling probe 3 (SPU pass 35; RE pass 73, the RE notes section 11.2-11.3): KZ3_CULL_PROBE=1.
 *
 * The level's visibility results live in the per-view descriptor X+0xC670 (four {count, EA, cap} triplets at
 * +0x00/+0x0C/+0x18/+0x24) whose sync word +0x30 the PPU sets to 1 at the rebuild (0xB37FBC) and spins on until
 * an SPU chain clears it. Candidate 3 of 11.3 ("early sync clear": the PPU reads partial lists) shows as a count
 * that still grows after the clear. So: the pre-hook of 0xB37FBC (next frame) hands the descriptor EA here once;
 * from then on the DMA put hook and the PUTLLC hook (patch 1051) watch for the write that lands on +0x30 and read
 * the four counts at that moment (s0), a helper thread reads them 2 ms later (s1), and the next rebuild's pre-hook
 * reads them a third time (s2: what the PPU finally consumed, before it is overwritten). Any difference between
 * s0/s1/s2 is printed ([cull-probe] frame lines, 40 at most); a 10 s summary gives frames, how many clears the
 * two hooks saw (put vs atomic; 0 of both = the clear comes another way), the diff counts and the last counts.
 * Static camera: the counts should also repeat frame to frame (candidate 5 "not culling" = counts stable while
 * the prop blinks). 1.00 addresses in the override row; 1.14's 0xB37FBC counterpart is not known yet (re-data). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size); /* runtime/spu/spu_channels.c */
extern void (*g_kz3_putllc_hook)(uint32_t ea);             /* patch 1051 */

static int g_on = -1;
static volatile uint32_t g_desc;
static void (*g_put_next)(uint32_t, uint32_t);
static uint32_t g_s0[4], g_s1[4];
static volatile int g_s0_valid, g_s1_valid;
static unsigned g_frames, g_clears_put, g_clears_putllc, g_d01, g_d12, g_d02, g_lines, g_repeat, g_last[4];
#ifdef _WIN32
static HANDLE g_ev;
#endif

static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static void read4(uint32_t* o)
{
    uint32_t d = g_desc;
    o[0] = rd32(d);
    o[1] = rd32(d + 0xC);
    o[2] = rd32(d + 0x18);
    o[3] = rd32(d + 0x24);
}

static void on_clear(int atomic)
{
    if (rd32(g_desc + 0x30) != 0) return; /* a write on the line that did not clear the word */
    read4(g_s0);
    g_s0_valid = 1;
    if (atomic)
        g_clears_putllc++;
    else
        g_clears_put++;
#ifdef _WIN32
    if (g_ev) SetEvent(g_ev);
#endif
}
static void put_hook(uint32_t ea, uint32_t size)
{
    uint32_t w = g_desc + 0x30;
    if (g_desc && ea <= w && w < ea + size) on_clear(0);
    if (g_put_next) g_put_next(ea, size);
}
static void putllc_hook(uint32_t ea)
{
    if (g_desc && (ea & ~127u) == ((g_desc + 0x30) & ~127u)) on_clear(1);
}
#ifdef _WIN32
static DWORD WINAPI delayed_read(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(g_ev, INFINITE);
        Sleep(2);
        if (g_desc) {
            read4(g_s1);
            g_s1_valid = 1;
        }
    }
}
#endif

/* Pre-hook of 0xB37FBC(desc, ...): desc = X+0xC670 + view*0x7300. The first descriptor seen is the one watched. */
void kz3_cull_probe_desc(uint32_t desc)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_CULL_PROBE");
        g_on = (e && atoi(e) > 0) ? 1 : 0;
    }
    if (!g_on || !vm_base || !desc) return;
    if (!g_desc) {
        g_desc = desc;
#ifdef _WIN32
        g_ev = CreateEvent(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 64 * 1024, delayed_read, NULL, 0, NULL);
#endif
        g_put_next = g_spu_put_hook;
        g_spu_put_hook = put_hook;
        g_kz3_putllc_hook = putllc_hook;
        fprintf(stderr,
                "[cull-probe] desc=0x%08X (X+0xC670): watching the sync word +0x30 = 0x%08X (now %u), counts at "
                "+0x00/+0x0C/+0x18/+0x24 (KZ3_CULL_PROBE)\n",
                desc, desc + 0x30, rd32(desc + 0x30));
        return;
    }
    if (desc != g_desc) return;
    uint32_t s2[4];
    read4(s2);
    g_frames++;
    int d01 = g_s0_valid && g_s1_valid && memcmp(g_s0, g_s1, sizeof g_s0) != 0;
    int d12 = g_s1_valid && memcmp(g_s1, s2, sizeof s2) != 0;
    int d02 = g_s0_valid && memcmp(g_s0, s2, sizeof s2) != 0;
    g_d01 += d01;
    g_d12 += d12;
    g_d02 += d02;
    if (g_frames > 1 && memcmp(g_last, s2, sizeof s2) == 0) g_repeat++;
    if ((d01 || d12 || d02) && g_lines < 40) {
        g_lines++;
        fprintf(
            stderr,
            "[cull-probe] frame %u: counts differ: at clear %u/%u/%u/%u (%s) +2ms %u/%u/%u/%u at rebuild %u/%u/%u/%u\n",
            g_frames, g_s0[0], g_s0[1], g_s0[2], g_s0[3], g_s0_valid ? "seen" : "no clear seen", g_s1[0], g_s1[1],
            g_s1[2], g_s1[3], s2[0], s2[1], s2[2], s2[3]);
    }
    if (g_frames % 100 == 0)
        fprintf(
            stderr,
            "[cull-probe] frames=%u clears put=%u putllc=%u diff clear-vs-2ms=%u 2ms-vs-rebuild=%u clear-vs-rebuild=%u "
            "same-as-previous=%u last counts %u/%u/%u/%u sync=%u\n",
            g_frames, g_clears_put, g_clears_putllc, g_d01, g_d12, g_d02, g_repeat, s2[0], s2[1], s2[2], s2[3],
            rd32(g_desc + 0x30));
    memcpy(g_last, s2, sizeof s2);
    g_s0_valid = g_s1_valid = 0;
}
