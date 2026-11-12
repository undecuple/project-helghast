/*
 * Host test for KZ3 patch 2162 (RSX pass 149): the SSE2 HALF kernel (CMP32 has none: measured slower) of rsx_vertex_fetch_many against
 * the scalar loops (RSX_VTX_SIMD off) and against rsx_vertex_fetch_one, byte for byte, over random and exhaustive
 * inputs; then a micro-benchmark (informative). Built by tools/smoke/test_vtx_simd.sh; also built with
 * -DRSX_VTX_SIMD_TEST_FLAW (the kernels then carry a deliberate bug) as the negative control: that build must FAIL.
 */
#define _POSIX_C_SOURCE 200809L
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out) { (void)rsx; (void)index; memset(out, 0, sizeof(*out)); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4]) { (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); g_fail = 1; } printf("\n"); } while (0)

static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)(g_rng >> 11); }

#define SRC_BYTES (1u << 20)
static u8 g_src[SRC_BYTES];
static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
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
}

#define MAXN 20000
static rsx_vertex_ref g_refs[MAXN];
static u8 g_a[MAXN * 48 + 64], g_b[MAXN * 48 + 64], g_c[MAXN * 48 + 64];

/* simd on, simd off and fetch_one must agree on the return code and (when it succeeded) every output byte */
static int agree(const rsx_vertex_fetch_plan* plan, u32 count)
{
    const u32 stride = plan->layout.stride;
    memset(g_a, 0xA5, (size_t)count * stride + 16); memset(g_b, 0xA5, (size_t)count * stride + 16);
    memset(g_c, 0xA5, (size_t)count * stride + 16);
    int ok1 = 1;
    for (u32 i = 0; i < count && ok1; i++) ok1 = rsx_vertex_fetch_one(plan, &g_refs[i], g_a + (u64)i * stride);
    rsx_vtx_cmp32_batch = 1; rsx_vtx_f16_fast = 1;
    rsx_vtx_simd = 1; const int ok2 = rsx_vertex_fetch_many(plan, g_refs, count, g_b);
    rsx_vtx_simd = 0; const int ok3 = rsx_vertex_fetch_many(plan, g_refs, count, g_c);
    if (ok1 != ok2 || ok1 != ok3) return 0;
    if (!ok1) return 1;
    return !memcmp(g_a, g_b, (size_t)count * stride + 16) && !memcmp(g_b, g_c, (size_t)count * stride + 16);
}

static double bench(const rsx_vertex_fetch_plan* plan, u32 count, int simd)
{
    rsx_vtx_simd = simd; rsx_vtx_cmp32_batch = 1; rsx_vtx_f16_fast = 1;
    struct timespec t0, t1;
    rsx_vertex_fetch_many(plan, g_refs, count, g_b);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 300; r++) rsx_vertex_fetch_many(plan, g_refs, count, g_b);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (300.0 * count);
}

int main(void)
{
    for (u32 i = 0; i < SRC_BYTES; i++) g_src[i] = (u8)rnd();
    for (u32 h = 0; h < 65536; h++) { g_src[0x80000 + h * 2] = (u8)(h >> 8); g_src[0x80000 + h * 2 + 1] = (u8)h; }   /* every half */
    /* 1. random: type x size x stride x count x refs (some outside the mapping) */
    {
        u32 bad = 0, cases = 0;
        static const u32 strides[] = { 0, 4, 8, 12, 16, 20, 32 };   /* 0 = elem size */
        for (u32 type = RSX_VTX_TYPE_HALF; type <= RSX_VTX_TYPE_CMP32; type++) {
            if (type != RSX_VTX_TYPE_HALF && type != RSX_VTX_TYPE_CMP32) continue;
            for (u32 size = 1; size <= 4; size++)
                for (u32 si = 0; si < 7; si++)
                    for (u32 count = 0; count < 70; count += (count < 20 ? 1 : 7)) {
                        const u32 st = strides[si];
                        if (st && st < rsx_vertex_attrib_size(type, size)) continue;
                        rsx_vertex_fetch_plan plan;
                        plan_init(&plan, (1u << 0) | (1u << 3));
                        set_attr(&plan, 0, type, size, st, 0x1000, 0);
                        set_attr(&plan, 3, RSX_VTX_TYPE_FLOAT, 4, 16, 0x40000, 0);
                        for (u32 i = 0; i < count; i++) { g_refs[i].vertex_id = rnd() % 900; g_refs[i].base_index = (i & 1) ? 0 : 3; }
                        rsx_vertex_fetch_plan_prepare(&plan, g_refs, count / 2 + 1);   /* later refs may fall outside the mapping */
                        for (u32 i = count / 2 + 1; i < count; i++) if (rnd() & 1) g_refs[i].vertex_id += 5000;
                        cases++;
                        if (!agree(&plan, count)) bad++;
                    }
        }
        CHECK(bad == 0, "%u random plans (HALF/CMP32 x 1-4 comps x strides x counts, partial mappings): simd == scalar == fetch_one", cases);
    }
    /* 2. every half value (2 and 4 components) and 65536 random CMP32 words, contiguous refs, odd count */
    {
        u32 bad = 0;
        for (u32 size = 2; size <= 4; size += 2) {
            rsx_vertex_fetch_plan plan;
            plan_init(&plan, 1u << 0);
            set_attr(&plan, 0, RSX_VTX_TYPE_HALF, size, size * 2, 0x80000, 0);
            const u32 cnt = 65536 / (size / 2) - 1;
            for (u32 i = 0; i < cnt; i++) { g_refs[i % MAXN].vertex_id = i; g_refs[i % MAXN].base_index = 0; }
            const u32 n = cnt < MAXN ? cnt : MAXN;
            rsx_vertex_fetch_plan_prepare(&plan, g_refs, n);
            if (!agree(&plan, n)) bad++;
            for (u32 i = 0; i < n; i++) g_refs[i].vertex_id = i + (size == 2 ? 20000u : 10000u);   /* second window of the half table */
            rsx_vertex_fetch_plan_prepare(&plan, g_refs, n);
            if (!agree(&plan, n)) bad++;
        }
        for (u32 pass = 0; pass < 8; pass++) {
            rsx_vertex_fetch_plan plan;
            plan_init(&plan, 1u << 0);
            set_attr(&plan, 0, RSX_VTX_TYPE_CMP32, 1, 4, 0x20000 + pass * 0x8000, 0);
            for (u32 i = 0; i < 8191 + pass; i++) { g_refs[i].vertex_id = i; g_refs[i].base_index = 0; }
            for (u32 i = 0; i < 8192; i++) { u32 w = rnd() ^ (rnd() << 21); u8* p = g_src + 0x20000 + pass * 0x8000 + i * 4;
                                              p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w; }
            rsx_vertex_fetch_plan_prepare(&plan, g_refs, 8191 + pass);
            if (!agree(&plan, 8191 + pass)) bad++;
        }
        CHECK(bad == 0, "every half value, divided-free CMP32 sweeps (odd counts): simd == scalar == fetch_one");
    }
    /* 3. a divided (modulo) attribute and a failing ATTR0 */
    {
        rsx_vertex_fetch_plan plan;
        plan_init(&plan, (1u << 0) | (1u << 1));
        plan.divider_mask = 1u << 1;
        set_attr(&plan, 0, RSX_VTX_TYPE_CMP32, 1, 4, 0x1000, 0);
        set_attr(&plan, 1, RSX_VTX_TYPE_HALF, 4, 8, 0x2000, 3);
        for (u32 i = 0; i < 301; i++) { g_refs[i].vertex_id = rnd() % 200; g_refs[i].base_index = 0; }
        rsx_vertex_fetch_plan_prepare(&plan, g_refs, 301);
        CHECK(agree(&plan, 301), "frequency-divided HALF next to CMP32: simd == scalar");
        rsx_vertex_fetch_plan p2;
        plan_init(&p2, 1u << 0);
        set_attr(&p2, 0, RSX_VTX_TYPE_HALF, 4, 8, 0xFFFF0, 0);   /* runs off the source: attr 0 must fail in all three paths */
        for (u32 i = 0; i < 64; i++) { g_refs[i].vertex_id = i * 100; g_refs[i].base_index = 0; }
        rsx_vertex_fetch_plan_prepare(&p2, g_refs, 64);
        CHECK(agree(&p2, 64), "unreadable ATTR0: same return code in simd, scalar and fetch_one");
    }
    /* 4. micro-benchmark (informative): ns per vertex, contiguous refs (post-remap shape) and shuffled refs */
    {
        for (int shuffled = 0; shuffled < 2; shuffled++) {
            for (u32 i = 0; i < 16384; i++) { g_refs[i].vertex_id = i; g_refs[i].base_index = 0; }
            if (shuffled) for (u32 i = 16383; i > 0; i--) { u32 j = rnd() % (i + 1); u32 t = g_refs[i].vertex_id; g_refs[i].vertex_id = g_refs[j].vertex_id; g_refs[j].vertex_id = t; }
            static const struct { const char* name; u32 type, size, stride; } cfg[] = {
                { "CMP32      ", RSX_VTX_TYPE_CMP32, 1, 4 }, { "HALF x4    ", RSX_VTX_TYPE_HALF, 4, 8 },
                { "HALF x2    ", RSX_VTX_TYPE_HALF, 2, 4 } };
            for (int k = 0; k < 3; k++) {
                rsx_vertex_fetch_plan plan;
                plan_init(&plan, 1u << 0);
                set_attr(&plan, 0, cfg[k].type, cfg[k].size, cfg[k].stride, 0x20000, 0);
                rsx_vertex_fetch_plan_prepare(&plan, g_refs, 16384);
                double off = bench(&plan, 16384, 0), on = bench(&plan, 16384, 1);
                off = bench(&plan, 16384, 0); on = bench(&plan, 16384, 1);   /* second round: warm */
                printf("[time] %s %s 16384 vertices: scalar %.2f ns/vertex, SSE2 %.2f ns/vertex (x%.2f)\n", cfg[k].name,
                       shuffled ? "shuffled " : "ascending", off, on, off / on);
            }
        }
    }
    printf(g_fail ? "[test_vtx_simd] FAIL\n" : "[test_vtx_simd] PASS\n");
    return g_fail;
}
