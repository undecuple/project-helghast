/* Host test for KZ3 patch 2075 (rsx_tex_lru.h): the O(1) LRU list picks the same victim as the old
 * min-last_use_serial scan, and a cache shaped like the live-draw texture cache recycles the victim's resource for
 * KZ3's moving ring of 8x8 blocks while the textures bound every frame stay cached. No D3D12, no game data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_tex_lru.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* 1. random hits and misses: list victim == serial scan victim, every time */
static void test_equivalence(uint32_t cap, uint32_t ops)
{
    rsx_tex_lru l; rsx_tex_lru_init(&l);
    rsx_tex_lru_link* k = calloc(cap, sizeof *k);
    uint64_t* serial = calloc(cap, sizeof *serial);
    uint64_t now = 0; uint32_t n = 0, mismatches = 0, evictions = 0;
    for (uint32_t op = 0; op < ops; op++) {
        if (n > 0 && (rnd() % 4u) != 0u) {   /* hit on a random live entry, skewed to low indices */
            const uint32_t i = (rnd() % 3u) ? rnd() % (n < 16u ? n : 16u) : rnd() % n;
            serial[i] = ++now; rsx_tex_lru_touch(&l, k, i);
            continue;
        }
        uint32_t idx;
        if (n < cap) { idx = n++; rsx_tex_lru_push_front(&l, k, idx); }
        else {
            uint32_t scan = 0;
            for (uint32_t i = 1; i < n; i++) if (serial[i] < serial[scan]) scan = i;
            idx = rsx_tex_lru_victim(&l);
            if (idx != scan) mismatches++;
            evictions++;
            if ((rnd() % 8u) == 0u) continue;   /* failed decode: the victim keeps its key and its place */
            rsx_tex_lru_touch(&l, k, idx);
        }
        serial[idx] = ++now;
    }
    CHECK(mismatches == 0, "equivalence cap=%u: %u of %u victims differ from the serial scan", cap, mismatches,
          evictions);
    CHECK(evictions > ops / 10u, "equivalence cap=%u: only %u evictions, the test did not exercise the full cache",
          cap, evictions);
    /* walk the list: every entry once, links consistent */
    uint32_t seen = 0, prev = RSX_TEX_LRU_NIL;
    for (uint32_t i = l.head; i != RSX_TEX_LRU_NIL && seen <= cap; i = k[i].next, seen++) {
        CHECK(k[i].prev == prev, "list cap=%u: entry %u prev %u, expected %u", cap, i, k[i].prev, prev);
        prev = i;
    }
    CHECK(seen == n && l.tail == prev, "list cap=%u: %u linked of %u entries", cap, seen, n);
    printf("[test_tex_lru] equivalence cap=%u ops=%u evictions=%u mismatches=%u\n", cap, ops, evictions, mismatches);
    free(k); free(serial);
}

/* 2. the K4 pattern: S static textures bound every frame + B ring blocks per frame at advancing offsets in a ring
 * of R slots (R > cap). A resource is (w, h); recycling reuses the victim's resource when the shape matches, as
 * create_texture_mipped does. */
typedef struct { uint32_t key, w, h; int res; } ent_t;
static void test_ring(int recycle)
{
    enum { CAP = 256, S = 100, B = 40, R = 1000, FRAMES = 200 };
    static ent_t e[CAP];
    rsx_tex_lru l; rsx_tex_lru_init(&l);
    static rsx_tex_lru_link k[CAP];
    uint32_t n = 0, creates = 0, recycled = 0, static_misses = 0, ring_pos = 0, late_creates = 0;
    int next_res = 1;
    for (uint32_t f = 0; f < FRAMES; f++) {
        for (uint32_t d = 0; d < S + B; d++) {
            const int is_static = d < S;
            const uint32_t key = is_static ? 0x80000000u + d : (ring_pos++ % R);
            const uint32_t w = is_static ? 256u : 8u, h = is_static ? 256u : 8u;
            uint32_t i;
            for (i = 0; i < n; i++) if (e[i].key == key) break;
            if (i < n) { rsx_tex_lru_touch(&l, k, i); continue; }
            if (is_static) static_misses++;
            int victim_res = 0; uint32_t vw = 0, vh = 0;
            if (n < CAP) { i = n++; rsx_tex_lru_push_front(&l, k, i); }
            else { i = rsx_tex_lru_victim(&l); victim_res = e[i].res; vw = e[i].w; vh = e[i].h;
                   rsx_tex_lru_touch(&l, k, i); }
            int res;
            if (recycle && victim_res && vw == w && vh == h) { res = victim_res; recycled++; }
            else { res = next_res++; creates++; if (f >= 50u) late_creates++; }
            e[i].key = key; e[i].w = w; e[i].h = h; e[i].res = res;
        }
    }
    printf("[test_tex_lru] ring recycle=%d: creates=%u (after frame 50: %u) recycled=%u static-misses=%u\n", recycle,
           creates, late_creates, recycled, static_misses);
    CHECK(static_misses == S, "ring recycle=%d: textures bound every frame missed %u times (want %u)", recycle,
          static_misses, (unsigned)S);
    if (recycle) {
        CHECK(late_creates == 0, "ring: %u resources created in steady state (want 0)", late_creates);
        CHECK(creates == CAP, "ring: %u resources created (want %u, one per cache entry)", creates, (unsigned)CAP);
    } else {
        CHECK(late_creates == 150u * B, "ring (old path): %u late creates, want one per ring miss (%u)", late_creates,
              150u * B);
    }
}

int main(void)
{
    test_equivalence(8, 20000);
    test_equivalence(64, 200000);
    test_equivalence(8192, 400000);
    test_ring(0);
    test_ring(1);
    printf(fails ? "[test_tex_lru] FAIL (%d)\n" : "[test_tex_lru] OK\n", fails);
    return fails ? 1 : 0;
}
