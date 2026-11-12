/*
 * Host test for KZ3 patch 2161 (RSX pass 149): rsx_smp_index.h against the linear memcmp scan sampler_slot used
 * before. Random key streams (hits, misses, adds up to the 128-key cap, colliding hashes), equivalence of every lookup,
 * a negative control (an index that misses one add must be caught), and a micro-benchmark (informative).
 * Built by tools/smoke/test_smp_index.sh.
 */
#define _POSIX_C_SOURCE 200809L
#include "rsx_smp_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CAP 128u
static uint32_t g_keys[CAP][4];
static uint16_t g_idx[RSX_SMP_IDX_SIZE];
static uint32_t g_n;
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }

static int scan(const uint32_t key[4])
{
    for (uint32_t i = 0; i < g_n; i++) if (!memcmp(g_keys[i], key, 16)) return (int)i;
    return -1;
}
static void reset(void) { g_n = 0; memset(g_idx, 0, sizeof g_idx); }
static void make_key(uint32_t k[4], uint32_t pool)   /* keys shaped like sampler_key: few distinct bit patterns */
{
    const uint32_t r = rnd() % pool;
    k[0] = (r & 7u) << 8 | ((r >> 3) & 3u) << 24; k[1] = ((r >> 5) & 3u) | (((r >> 7) & 3u) << 8); k[2] = (r >> 9) << 4; k[3] = 0;
    if (pool > 64u && (r & 1u)) k[3] = r;
}

int main(void)
{
    int fail = 0;
    /* 1. equivalence: lookup (add on miss, as sampler_slot does) over many random streams, several pool sizes */
    uint64_t looks = 0, bad = 0;
    for (uint32_t pool = 4; pool <= 4096; pool *= 4)
        for (int stream = 0; stream < 200; stream++) {
            reset();
            for (int step = 0; step < 600; step++) {
                uint32_t key[4]; make_key(key, pool);
                if (rnd() % 11 == 0) key[rnd() & 3u] ^= 1u << (rnd() & 31u);   /* near-miss keys */
                const int a = scan(key), b = rsx_smp_index_find(g_idx, (const uint32_t (*)[4])g_keys, key);
                looks++; if (a != b) bad++;
                if (a < 0 && g_n < CAP) { memcpy(g_keys[g_n], key, 16); rsx_smp_index_add(g_idx, (const uint32_t (*)[4])g_keys, g_n); g_n++; }
            }
        }
    printf("[%s] %llu lookups over random streams (pools 4..4096, up to %u keys): index == linear scan, %llu mismatches\n",
           bad ? "FAIL" : "PASS", (unsigned long long)looks, CAP, (unsigned long long)bad);
    fail |= bad != 0;
    /* 2. full table: 128 keys, every key found at its own index, absent keys not found, load <= 25 % */
    reset();
    for (uint32_t i = 0; i < CAP; i++) { g_keys[i][0] = i * 0x1000193u; g_keys[i][1] = i; g_keys[i][2] = i << 4; g_keys[i][3] = 0;
                                          rsx_smp_index_add(g_idx, (const uint32_t (*)[4])g_keys, i); }
    g_n = CAP;
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < CAP; i++) if (rsx_smp_index_find(g_idx, (const uint32_t (*)[4])g_keys, g_keys[i]) != (int)i) wrong++;
    for (uint32_t i = 0; i < 4096; i++) { uint32_t k[4] = { rnd(), rnd(), rnd(), rnd() }; if (rsx_smp_index_find(g_idx, (const uint32_t (*)[4])g_keys, k) != -1) wrong++; }
    printf("[%s] full 128-key table: every key found at its index, 4096 absent keys not found\n", wrong ? "FAIL" : "PASS");
    fail |= wrong != 0;
    /* 3. negative control: an index that lost one add must disagree with the scan */
    reset();
    for (uint32_t i = 0; i < 40; i++) { for (int c = 0; c < 4; c++) g_keys[i][c] = rnd(); if (i != 17) rsx_smp_index_add(g_idx, (const uint32_t (*)[4])g_keys, i); }
    g_n = 40;
    const int sa = scan(g_keys[17]), sb = rsx_smp_index_find(g_idx, (const uint32_t (*)[4])g_keys, g_keys[17]);
    printf("[%s] negative control: index missing one add is caught (scan %d, index %d)\n", sa != sb ? "PASS" : "FAIL", sa, sb);
    fail |= sa == sb;
    /* 4. micro-benchmark: lookups of present keys, ns per lookup, scan vs index, for n cached keys */
    for (uint32_t n = 16; n <= 128; n *= 2) {
        reset();
        for (uint32_t i = 0; i < n; i++) { for (int c = 0; c < 4; c++) g_keys[i][c] = rnd(); rsx_smp_index_add(g_idx, (const uint32_t (*)[4])g_keys, i); }
        g_n = n;
        enum { M = 4096 };
        static uint32_t req[M][4];
        for (int i = 0; i < M; i++) memcpy(req[i], g_keys[rnd() % n], 16);
        volatile int sink = 0; double ns[2];
        for (int mode = 0; mode < 2; mode++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int r = 0; r < 2000; r++)
                for (int i = 0; i < M; i++)
                    sink += mode ? rsx_smp_index_find(g_idx, (const uint32_t (*)[4])g_keys, req[i]) : scan(req[i]);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ns[mode] = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (2000.0 * M);
        }
        if (sink == 12345678) puts("");
        printf("[time] %3u cached keys, hits at random positions: scan %.1f ns/lookup, index %.1f ns/lookup (x%.1f)\n", n, ns[0], ns[1], ns[0] / ns[1]);
    }
    printf(fail ? "[test_smp_index] FAIL\n" : "[test_smp_index] PASS\n");
    return fail;
}
