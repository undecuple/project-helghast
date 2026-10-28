/*
 * Host test for KZ3 patch 2040 (RSX pass 40): rsx_vertex_fetch_many, the
 * attribute-major vertex fetch, against rsx_vertex_fetch_one byte for byte.
 * No D3D12, no game data: the fetch plan is filled by hand over a random
 * source buffer. Built by tools/smoke/test_vtx_batch.sh (Linux cc).
 *
 * Cases: every element type (FLOAT/HALF/UNORM8/UINT8/SNORM16/SINT16/CMP32)
 * with 1-4 components, disabled attributes (defaults), a frequency-divided
 * attribute, vertices outside the contiguous mapping (guest_ptr fallback),
 * no mapping at all, INLINE_ARRAY plans, counts around the block size, and
 * an unreadable ATTR0 (both must return 0). Then a timing line (informative).
 * Patches 2073 (CMP32 block loop) and 2074 (branch-free HALF, RSX pass 69):
 * the cases run with both on and off, plus every half value and a CMP32
 * sweep, and a second timing line (on vs off).
 */
#define _POSIX_C_SOURCE 200809L
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The fetch-plan init path of rsx_vertex_compact.c reads the dispatch state; this test fills plans by hand and
 * never calls it, so the symbols are stubs (host tests stay self-contained). */
u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out) { (void)rsx; (void)index; memset(out, 0, sizeof(*out)); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4]) { (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); g_fail = 1; } printf("\n"); } while (0)

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)(g_rng >> 11); }

#define SRC_BYTES (1u << 20)
static u8 g_src[SRC_BYTES];
static u32 g_guest_calls;

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    g_guest_calls++;
    if (location != 0 || (u64)offset + min_bytes > SRC_BYTES) return NULL;
    return g_src + offset;
}

static void set_attr(rsx_vertex_fetch_plan* plan, u32 attr, u32 type, u32 size, u32 stride, u32 offset, u32 freq)
{
    rsx_vertex_fetch_attr* f = &plan->attr[attr];
    f->desc.type = type; f->desc.size = size; f->desc.stride = stride; f->desc.offset = offset;
    f->desc.frequency = freq; f->desc.location = 0;
    f->elem_size = type ? rsx_vertex_attrib_size(type, size) : 0;
    f->stride = stride ? stride : f->elem_size;
    for (int c = 0; c < 4; c++) f->default_value[c] = (float)(rnd() % 1000) / 7.0f;
}

static void plan_init(rsx_vertex_fetch_plan* plan, u32 mask)
{
    memset(plan, 0, sizeof(*plan));
    rsx_vertex_layout_plan_init(&plan->layout, mask);
    plan->guest_ptr = guest_ptr;
    plan->base_offset = 0;
}

/* Fetch count refs both ways; return 1 when the results (and return codes) agree. */
static int compare(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count, u8* a, u8* b,
                   int* rc_one, int* rc_many)
{
    const u32 stride = plan->layout.stride;
    memset(a, 0xA5, (size_t)count * stride + 16);
    memset(b, 0xA5, (size_t)count * stride + 16);
    int ok1 = 1;
    for (u32 i = 0; i < count && ok1; i++)
        ok1 = rsx_vertex_fetch_one(plan, &refs[i], a + (u64)i * stride);
    const int ok2 = rsx_vertex_fetch_many(plan, refs, count, b);   /* v0.18.0: upstream's signature, out at layout.stride */
    *rc_one = ok1; *rc_many = ok2;
    if (ok1 != ok2) return 0;
    if (!ok1) return 1;   /* both failed: the partial output is unspecified */
    return memcmp(a, b, (size_t)count * stride + 16) == 0;
}

#ifdef RSX_VTX_PATCH_2073
/* Patches 2073 (CMP32 block loop) and 2074 (branch-free HALF) on or off together. */
static void set_fast(int on)
{
    rsx_vtx_cmp32_batch = on;
#ifdef RSX_VTX_PATCH_2074
    rsx_vtx_f16_fast = on;
#endif
}
#endif

static void fill_refs(rsx_vertex_ref* refs, u32 count, u32 max_id, u32 base_index)
{
    for (u32 i = 0; i < count; i++) { refs[i].vertex_id = rnd() % max_id; refs[i].base_index = base_index; }
}

int main(void)
{
    for (u32 i = 0; i < SRC_BYTES; i++) g_src[i] = (u8)rnd();
    /* A few exact half/float specials so the conversions see them. */
    for (u32 i = 0; i < 4096; i += 64) { g_src[i] = 0x7C; g_src[i + 1] = 0x00; g_src[i + 2] = 0x80; g_src[i + 3] = 0x00; }

    enum { MAXN = 2048 };
    static rsx_vertex_ref refs[MAXN];
    static u8 outa[MAXN * 16 * 16 + 64], outb[MAXN * 16 * 16 + 64];
    int r1, r2;

#ifdef RSX_VTX_PATCH_2073
    /* Patches 2073/2074: sections 1-3 run twice, the CMP32 block loop and branch-free HALF decode on, then off. */
    for (int mode = 0; mode < 2; mode++) {
    set_fast(!mode);
    printf("[mode] patch 2073/2074 paths %s\n", mode ? "off" : "on");
#endif
    /* 1. Every type and size in one interleaved layout (stride 64), plus a disabled attribute and a divided one. */
    {
        rsx_vertex_fetch_plan plan;
        const u32 mask = 0x7FFFu;   /* attrs 0-14 (15 stays disabled by mask) */
        plan_init(&plan, mask);
        const u32 types[7] = { RSX_VTX_TYPE_FLOAT, RSX_VTX_TYPE_HALF, RSX_VTX_TYPE_UNORM8, RSX_VTX_TYPE_UINT8,
                               RSX_VTX_TYPE_SNORM16, RSX_VTX_TYPE_SINT16, RSX_VTX_TYPE_CMP32 };
        u32 off = 0;
        for (u32 a = 0; a < 14; a++) {
            const u32 type = types[a % 7], size = (a / 7) ? 4 - (a % 4) : 1 + (a % 4);
            set_attr(&plan, a, type, size, 64, off, (a == 13) ? 3 : 0);
            off += rsx_vertex_attrib_size(type, size);
        }
        set_attr(&plan, 14, 0, 0, 0, 0, 0);   /* enabled by the mask, disabled descriptor: default value */
        plan.divider_mask = 1u << 13;          /* attr 13: modulo 3 */
        const u32 counts[7] = { 0, 1, 127, 128, 129, 1000, 2048 };
        for (u32 k = 0; k < 7; k++) {
            fill_refs(refs, counts[k], 4000, (k & 1) ? 17 : 0);
            rsx_vertex_fetch_plan_prepare(&plan, refs, counts[k]);
            const int same = compare(&plan, refs, counts[k], outa, outb, &r1, &r2);
            CHECK(same && r1 == 1, "interleaved all-types, count %u: identical (rc %d/%d)", counts[k], r1, r2);
        }
        /* Narrow the mapping so half of the vertices fall back to guest_ptr. */
        fill_refs(refs, 1000, 4000, 0);
        rsx_vertex_fetch_plan_prepare(&plan, refs, 1000);
        for (u32 a = 0; a < 14; a++) { plan.attr[a].stable_first = 1000; plan.attr[a].stable_last = 2999; }
        g_guest_calls = 0;
        CHECK(compare(&plan, refs, 1000, outa, outb, &r1, &r2) && r1 == 1 && g_guest_calls > 0,
              "narrowed mapping (guest_ptr fallbacks %u): identical", g_guest_calls);
        /* No mapping at all: every attribute through guest_ptr. */
        for (u32 a = 0; a < 14; a++) plan.attr[a].stable_base = NULL;
        CHECK(compare(&plan, refs, 1000, outa, outb, &r1, &r2) && r1 == 1, "no mapping: identical");
        /* An unreadable ATTR0 (ids past the buffer): both fail. */
        for (u32 i = 0; i < 1000; i++) refs[i].vertex_id = 0xFFFF0u;
        CHECK(compare(&plan, refs, 1000, outa, outb, &r1, &r2) && r1 == 0, "unreadable ATTR0: both return 0");
    }

    /* 2. The level's common shape: FLOAT3 position, SNORM16x4, UNORM8x4, HALF2 texcoord, tight strides, mapping
     *    prepared; and a FLOAT4 attribute with stride == elem_size in another region. */
    {
        rsx_vertex_fetch_plan plan;
        plan_init(&plan, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 8) | (1u << 9));
        set_attr(&plan, 0, RSX_VTX_TYPE_FLOAT, 3, 32, 0, 0);
        set_attr(&plan, 2, RSX_VTX_TYPE_SNORM16, 4, 32, 12, 0);
        set_attr(&plan, 3, RSX_VTX_TYPE_UNORM8, 4, 32, 20, 0);
        set_attr(&plan, 8, RSX_VTX_TYPE_HALF, 2, 32, 24, 1);
        set_attr(&plan, 9, RSX_VTX_TYPE_FLOAT, 4, 0, 0x80000, 0);
        fill_refs(refs, 2048, 4000, 5);
        rsx_vertex_fetch_plan_prepare(&plan, refs, 2048);
        CHECK(plan.attr[0].stable_base && plan.attr[9].stable_base, "level-shaped plan prepared (contiguous mappings)");
        CHECK(compare(&plan, refs, 2048, outa, outb, &r1, &r2) && r1 == 1, "level-shaped layout: identical");
        /* ids in ascending dense order (the remap's dense range) */
        for (u32 i = 0; i < 2048; i++) { refs[i].vertex_id = i; refs[i].base_index = 100; }
        rsx_vertex_fetch_plan_prepare(&plan, refs, 2048);
        CHECK(compare(&plan, refs, 2048, outa, outb, &r1, &r2) && r1 == 1, "dense ascending ids: identical");
    }

    /* 3. INLINE_ARRAY plan: the stream supplies attrs 0 and 1; attr 2 is enabled but not in the stream. */
    {
        rsx_vertex_fetch_plan plan;
        plan_init(&plan, 0x7u);
        set_attr(&plan, 0, RSX_VTX_TYPE_FLOAT, 3, 0, 0, 0);
        set_attr(&plan, 1, RSX_VTX_TYPE_UNORM8, 4, 0, 0, 0);
        set_attr(&plan, 2, RSX_VTX_TYPE_HALF, 2, 0, 0, 0);
        plan.inline_data = g_src + 0x10000; plan.inline_bytes = 16 * 300; plan.inline_stride = 16;
        plan.inline_off[0] = 0; plan.inline_off[1] = 12; plan.inline_off[2] = 16; /* off+size > stride: unmapped */
        plan.guest_ptr = guest_ptr;
        for (u32 i = 0; i < 400; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }   /* 300..399 past the stream */
        rsx_vertex_fetch_plan_prepare(&plan, refs, 400);
        CHECK(compare(&plan, refs, 300, outa, outb, &r1, &r2) && r1 == 1, "inline stream, in range: identical");
        CHECK(compare(&plan, refs, 400, outa, outb, &r1, &r2) && r1 == 0, "inline stream, ATTR0 past the stream: both return 0");
    }
#ifdef RSX_VTX_PATCH_2073
    }

    /* 3b. Patches 2073/2074: all 65,536 half values (HALF4, 16,384 vertices) and 32,768 CMP32 words (bits 6-20 swept,
     *     the rest random) through the fast paths, against the per-vertex decoder; then a timing line on vs off. */
    {
        static u8 big_a[16384 * 32 + 64], big_b[16384 * 32 + 64];
        static rsx_vertex_ref big_refs[16384];
        set_fast(1);
        for (u32 h = 0; h < 65536; h++) { g_src[0xA0000 + h * 2] = (u8)(h >> 8); g_src[0xA0000 + h * 2 + 1] = (u8)h; }
        u32 bad = 0;
        for (u32 pass = 0; pass < 2; pass++) {
            rsx_vertex_fetch_plan plan;
            plan_init(&plan, (1u << 0) | (1u << 1));
            set_attr(&plan, 0, RSX_VTX_TYPE_HALF, 4, 8, 0xA0000, 0);
            set_attr(&plan, 1, RSX_VTX_TYPE_CMP32, 1, 4, 0x20000 + pass * 0x10000, 0);
            for (u32 i = 0; i < 16384; i++) {
                big_refs[i].vertex_id = i; big_refs[i].base_index = 0;
                const u32 w = (pass * 16384 + i) * 64u + (rnd() & 63u) + (rnd() << 20);   /* 2 x 16384 x 64 = 2^21 */
                u8* p = g_src + 0x20000 + pass * 0x10000 + i * 4;
                p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
            }
            rsx_vertex_fetch_plan_prepare(&plan, big_refs, 16384);
            if (!compare(&plan, big_refs, 16384, big_a, big_b, &r1, &r2) || r1 != 1) bad++;
        }
        CHECK(bad == 0, "all half values and 2 x 16384 CMP32 words: fast paths identical to the per-vertex decoder");
        rsx_vertex_fetch_plan plan;   /* timing (informative): HALF4 + CMP32, 16384 vertices x 100 */
        plan_init(&plan, (1u << 0) | (1u << 1));
        set_attr(&plan, 0, RSX_VTX_TYPE_HALF, 4, 8, 0xA0000, 0);
        set_attr(&plan, 1, RSX_VTX_TYPE_CMP32, 1, 4, 0x20000, 0);
        rsx_vertex_fetch_plan_prepare(&plan, big_refs, 16384);
        double ns[2];
        for (int mode = 0; mode < 2; mode++) {
            set_fast(!mode);
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int r = 0; r < 100; r++) rsx_vertex_fetch_many(&plan, big_refs, 16384, big_b, plan.layout.stride);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ns[mode] = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (100.0 * 16384);
        }
        printf("[time] HALF4+CMP32 fetch_many: fast paths on %.2f ns/vertex, off %.2f ns/vertex\n", ns[0], ns[1]);
        set_fast(1);
    }
#endif

    /* 4. Timing (informative): 2048 refs x 500 rounds of the level-shaped layout, ns per vertex each way. */
    {
        rsx_vertex_fetch_plan plan;
        plan_init(&plan, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 8) | (1u << 9));
        set_attr(&plan, 0, RSX_VTX_TYPE_FLOAT, 3, 32, 0, 0);
        set_attr(&plan, 2, RSX_VTX_TYPE_SNORM16, 4, 32, 12, 0);
        set_attr(&plan, 3, RSX_VTX_TYPE_UNORM8, 4, 32, 20, 0);
        set_attr(&plan, 8, RSX_VTX_TYPE_HALF, 2, 32, 24, 0);
        set_attr(&plan, 9, RSX_VTX_TYPE_FLOAT, 4, 0, 0x80000, 0);
        for (u32 i = 0; i < 2048; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }
        rsx_vertex_fetch_plan_prepare(&plan, refs, 2048);
        const u32 stride = plan.layout.stride;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int r = 0; r < 500; r++)
            for (u32 i = 0; i < 2048; i++) rsx_vertex_fetch_one(&plan, &refs[i], outa + (u64)i * stride);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double one = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (500.0 * 2048);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int r = 0; r < 500; r++) rsx_vertex_fetch_many(&plan, refs, 2048, outb, stride);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double many = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (500.0 * 2048);
        printf("[time] fetch_one %.1f ns/vertex, fetch_many %.1f ns/vertex (5 attributes, stride %u)\n", one, many, stride);
        CHECK(memcmp(outa, outb, (size_t)2048 * stride) == 0, "timing pass output identical");
    }

    printf("[test_vtx_batch] %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
