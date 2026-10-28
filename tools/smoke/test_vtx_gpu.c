/*
 * Host test for KZ3 patch 2076 (RSX pass 72): GPU-side vertex fetch (LD_VTX_GPU). The shader's decode is the
 * RSX_VG_SHARED text of rsx_vtx_gpu.h; here the same text is compiled as C (RSX_VG_C_REFERENCE) and run against
 * the CPU fetch. No D3D12, no game data. Built by tools/smoke/test_vtx_gpu.sh.
 *
 * 1. Every value of each format through vg_fetch vs rsx_vertex_decode_element, bit for bit, at all four byte
 *    alignments and sizes 1-4: HALF/SNORM16/SINT16 all 65,536, UNORM8/UINT8 all 256, CMP32 every field value
 *    plus 2^20 random words, FLOAT the specials plus 2^20 random words.
 * 2. Random draws (interleaved and separate buffers, odd strides and offsets, disabled attributes, base_index,
 *    one INLINE_ARRAY plan) through rsx_vertex_fetch_plan_prepare -> rsx_vg_plan -> rsx_vg_write_block -> vg_fetch
 *    per vertex, against rsx_vertex_fetch_many's float4 slots, bit for bit; every load stays inside the block.
 * 3. The fallbacks: a divided attribute (DIVIDED), an unmapped span (UNMAPPED), no inputs, the block cap (BIG).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_block[4u << 20];
static unsigned g_block_len, g_oob;
static float g_lut[68864];
static unsigned vg_raw_load(unsigned a)
{
    if (a % 4u || a + 4u > g_block_len) { g_oob++; return 0; }
    return (unsigned)g_block[a] | ((unsigned)g_block[a + 1] << 8) | ((unsigned)g_block[a + 2] << 16) |
           ((unsigned)g_block[a + 3] << 24);
}
#define VG_RAW(a) vg_raw_load(a)
#define VG_LUT(i) g_lut[i]
#define RSX_VG_C_REFERENCE
#include "rsx_vtx_gpu.h"
#include "rsx_vertex_formats.h"

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out) { (void)rsx; (void)index; memset(out, 0, sizeof(*out)); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4]) { (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); g_fail = 1; } printf("\n"); } while (0)
static u64 g_rng = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (u32)(g_rng >> 11); }

#define SRC_BYTES (1u << 20)
static u8 g_src[SRC_BYTES];
static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    if (location != 0 || (u64)offset + min_bytes > SRC_BYTES) return NULL;
    return g_src + offset;
}

/* Part 1: one element of `bytes` at block address 512 + align, read as attribute row (base = lim = address). */
static unsigned long long g_cmp, g_bad;
static void one_element(u32 type, u32 size, const u8* bytes, u32 nbytes)
{
    float want[4];
    rsx_vertex_decode_element(type, size, bytes, want);
    for (u32 al = 0; al < 4; al++) {
        const u32 a = 512u + al;
        memset(g_block + 512, 0xCD, 32);
        memcpy(g_block + a, bytes, nbytes);
        g_block_len = 512u + 32u;
        for (u32 c = 0; c < 4; c++) {
            const float got = vg_fetch(a, 0u, type | ((size < 4u ? size : 4u) << 4), a, -7.0f, 0u, c);
            g_cmp++;
            if (memcmp(&got, &want[c], 4)) {
                u32 gb, wb; memcpy(&gb, &got, 4); memcpy(&wb, &want[c], 4);
                if (g_bad++ < 8) printf("  mismatch type %u size %u align %u comp %u: got %08X want %08X\n", type, size,
                                        al, c, gb, wb);
            }
        }
    }
}

static void part1(void)
{
    u8 b[16];
    for (u32 size = 1; size <= 4; size++) {
        for (u32 v = 0; v < 65536u; v++) {   /* HALF, SNORM16, SINT16: the value in every component position */
            for (u32 c = 0; c < 8; c++) b[c] = (u8)rnd();
            const u32 pos = v % size;
            b[pos * 2] = (u8)(v >> 8); b[pos * 2 + 1] = (u8)v;
            one_element(RSX_VTX_TYPE_HALF, size, b, size * 2);
            one_element(RSX_VTX_TYPE_SNORM16, size, b, size * 2);
            one_element(RSX_VTX_TYPE_SINT16, size, b, size * 2);
        }
        for (u32 v = 0; v < 256u; v++)
            for (u32 pos = 0; pos < size; pos++) {
                for (u32 c = 0; c < 4; c++) b[c] = (u8)rnd();
                b[pos] = (u8)v;
                one_element(RSX_VTX_TYPE_UNORM8, size, b, size);
                one_element(RSX_VTX_TYPE_UINT8, size, b, size);
            }
        static const u32 special[] = { 0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFFFFFFFu,
                                       0x00000001u, 0x807FFFFFu, 0x3F800000u, 0x7F7FFFFFu };
        for (u32 i = 0; i < (1u << 18) + 11u; i++) {
            for (u32 c = 0; c < 4; c++) {
                const u32 w = i < 11u ? special[(i + c) % 11u] : rnd() ^ (rnd() << 21);
                b[c * 4] = (u8)(w >> 24); b[c * 4 + 1] = (u8)(w >> 16); b[c * 4 + 2] = (u8)(w >> 8); b[c * 4 + 3] = (u8)w;
            }
            one_element(RSX_VTX_TYPE_FLOAT, size, b, size * 4);
        }
    }
    for (u32 i = 0; i < 2048u + (1u << 20); i++) {   /* CMP32: every x/y (11-bit) and z (10-bit) value, then random */
        const u32 w = i < 2048u ? (i | (i << 11) | ((i & 0x3FFu) << 22)) : rnd() ^ (rnd() << 21);
        b[0] = (u8)(w >> 24); b[1] = (u8)(w >> 16); b[2] = (u8)(w >> 8); b[3] = (u8)w;
        one_element(RSX_VTX_TYPE_CMP32, 1 + (i & 3u), b, 4);
    }
    CHECK(g_bad == 0, "part 1: %llu components, every format at 4 alignments: %llu mismatches", g_cmp, g_bad);
}

/* Part 2: random draws. */
static void set_attr(rsx_vertex_fetch_plan* p, u32 attr, u32 type, u32 size, u32 stride, u32 offset, u32 freq)
{
    rsx_vertex_fetch_attr* f = &p->attr[attr];
    memset(&f->desc, 0, sizeof f->desc);
    f->desc.type = type; f->desc.size = size; f->desc.stride = stride; f->desc.offset = offset; f->desc.frequency = freq;
    f->elem_size = type ? rsx_vertex_attrib_size(type, size) : 0;
    f->stride = stride ? stride : f->elem_size;
    for (int c = 0; c < 4; c++) f->default_value[c] = (float)(rnd() % 1000) / 7.0f;
}

#define MAXV 4096u
static rsx_vertex_ref g_refs[MAXV];
static u8 g_cpu[MAXV * 256u];
static rsx_vg_draw g_d;

/* Returns the rsx_vg_plan reason; on OK compares every vertex. */
static int run_draw(rsx_vertex_fetch_plan* p, u32 count, u32 max_block, unsigned long long* comps, unsigned long long* bad)
{
    rsx_vertex_fetch_plan_prepare(p, g_refs, count);
    const int why = rsx_vg_plan(p, max_block, &g_d);
    if (why != RSX_VG_OK) return why;
    if (!rsx_vertex_fetch_many(p, g_refs, count, g_cpu, p->layout.stride)) { printf("  cpu fetch failed\n"); (*bad)++; return why; }
    if (g_d.block_bytes > sizeof g_block) { (*bad)++; return why; }
    memset(g_block, 0xEE, g_d.block_bytes + 64);
    rsx_vg_write_block(&g_d, g_block);
    g_block_len = g_d.block_bytes;
    for (u32 i = 0; i < count; i++) {
        const u32 e = rsx_vg_element(&g_refs[i]);
        for (u32 s = 0; s < p->layout.count; s++) {
            const u32 a = p->layout.attrs[s];
            u32 row[4]; float df[4];
            memcpy(row, g_block + a * 32u, 16); memcpy(df, g_block + a * 32u + 16u, 16);
            for (u32 c = 0; c < 4; c++) {
                const float got = vg_fetch(row[0], row[1], row[2], row[3], df[c], e, c);
                (*comps)++;
                if (memcmp(&got, g_cpu + (size_t)i * p->layout.stride + s * 16u + c * 4u, 4) && (*bad)++ < 8)
                    printf("  draw mismatch vertex %u attr %u comp %u type %u\n", i, a, c, p->attr[a].desc.type);
            }
        }
    }
    return why;
}

static void part2(void)
{
    for (u32 i = 0; i < SRC_BYTES; i++) g_src[i] = (u8)rnd();
    static const u32 types[] = { RSX_VTX_TYPE_FLOAT, RSX_VTX_TYPE_HALF, RSX_VTX_TYPE_UNORM8, RSX_VTX_TYPE_UINT8,
                                 RSX_VTX_TYPE_SNORM16, RSX_VTX_TYPE_SINT16, RSX_VTX_TYPE_CMP32 };
    unsigned long long comps = 0, bad = 0, oob0 = g_oob;
    u32 ok = 0, spans = 0, merged = 0, inl = 0;
    rsx_vertex_fetch_plan p;
    for (u32 iter = 0; iter < 3000u; iter++) {
        const u32 mask = (rnd() & 0xFFFFu) | 1u;
        memset(&p, 0, sizeof p);
        rsx_vertex_layout_plan_init(&p.layout, mask);
        p.guest_ptr = guest_ptr;
        const u32 interleaved = rnd() & 1u;
        const u32 vstride = 4u + rnd() % 60u;   /* odd and unaligned strides included */
        const u32 vbase = rnd() % 4096u;
        u32 nenabled = 0;
        for (u32 a = 0; a < 16u; a++) {
            if (rnd() % 6u == 0) { set_attr(&p, a, 0, 0, 0, 0, 0); continue; }   /* disabled: default */
            const u32 t = types[rnd() % 7u], sz = 1u + rnd() % 4u;
            const u32 es = rsx_vertex_attrib_size(t, sz);
            if (interleaved) set_attr(&p, a, t, sz, vstride > es ? vstride : es, vbase + rnd() % 16u, rnd() & 1u);
            else set_attr(&p, a, t, sz, es + rnd() % 9u, rnd() % 65536u, rnd() & 1u);
            nenabled++;
        }
        const u32 count = 1u + rnd() % (iter % 10u == 0 ? MAXV : 300u);
        const u32 bi = rnd() % 64u, maxid = 1u + rnd() % 2000u;
        for (u32 i = 0; i < count; i++) { g_refs[i].vertex_id = rnd() % maxid; g_refs[i].base_index = bi; }
        if (iter % 50u == 49u) {   /* INLINE_ARRAY: one packed stream, element = vertex_id + base_index */
            rsx_vertex_fetch_plan_set_inline(&p, NULL, NULL, 0);
            u32 off = 0;
            for (u32 a = 0; a < 16u; a++) { p.inline_off[a] = off; off += p.attr[a].elem_size; }
            p.inline_stride = off ? off : 4u;
            p.inline_data = g_src + 1;
            p.inline_bytes = p.inline_stride * (maxid + bi);
            if (p.inline_bytes > SRC_BYTES - 1) continue;
            inl++;
        }
        const int why = run_draw(&p, count, 64u << 20, &comps, &bad);
        if (why == RSX_VG_OK) { ok++; spans += g_d.n_span; merged += nenabled - g_d.n_span; }
    }
    printf("  %u draws on the GPU path (%u inline), %u spans, %u attribute spans merged into others\n", ok, inl, spans, merged);
    CHECK(bad == 0 && ok > 2500u, "part 2: %llu components of %u random draws vs rsx_vertex_fetch_many: %llu mismatches", comps, ok, bad);
    CHECK(g_oob == oob0, "part 2: every shader load inside the draw's block (%llu outside)", (unsigned long long)(g_oob - oob0));
}

static void part3(void)
{
    rsx_vertex_fetch_plan p;
    unsigned long long comps = 0, bad = 0;
    for (u32 i = 0; i < 16; i++) { g_refs[i].vertex_id = i; g_refs[i].base_index = 0; }
    memset(&p, 0, sizeof p); rsx_vertex_layout_plan_init(&p.layout, 0x3u); p.guest_ptr = guest_ptr;
    set_attr(&p, 0, RSX_VTX_TYPE_FLOAT, 3, 12, 0, 0); set_attr(&p, 1, RSX_VTX_TYPE_HALF, 2, 4, 4096, 2);
    CHECK(run_draw(&p, 16, 64u << 20, &comps, &bad) == RSX_VG_DIVIDED, "part 3: frequency-divided attribute stays on the CPU");
    set_attr(&p, 1, RSX_VTX_TYPE_HALF, 2, 4, SRC_BYTES - 8u, 0);
    CHECK(run_draw(&p, 16, 64u << 20, &comps, &bad) == RSX_VG_UNMAPPED, "part 3: unmapped span stays on the CPU");
    set_attr(&p, 1, RSX_VTX_TYPE_HALF, 2, 4, 4096, 0);
    CHECK(run_draw(&p, 16, 600u, &comps, &bad) == RSX_VG_BIG, "part 3: block over the cap stays on the CPU");
    CHECK(run_draw(&p, 16, 64u << 20, &comps, &bad) == RSX_VG_OK && bad == 0, "part 3: the same draw under the cap");
    {   /* an element past the span (no valid draw has one) reads the last element, inside the block */
        const unsigned long long oob0 = g_oob;
        int same = 1;
        for (u32 a = 0; a < 2; a++) {
            u32 row[4]; float df[4];
            memcpy(row, g_block + a * 32u, 16); memcpy(df, g_block + a * 32u + 16u, 16);
            for (u32 c = 0; c < 4; c++) {
                const float last = vg_fetch(row[0], row[1], row[2], row[3], df[c], 15u, c);
                const float far1 = vg_fetch(row[0], row[1], row[2], row[3], df[c], 0xFFFFFu, c);
                const float far2 = vg_fetch(row[0], row[1], row[2], row[3], df[c], 100u, c);
                const float next = vg_fetch(row[0], row[1], row[2], row[3], df[c], 16u, c);
                same &= !memcmp(&last, &far1, 4) && !memcmp(&last, &far2, 4) && !memcmp(&last, &next, 4);
            }
        }
        CHECK(same && g_oob == oob0, "part 3: out-of-span elements clamp to the last element");
    }
    {
        int same = 1;
        for (u32 i = 0; i < (1u << 20); i++) {
            rsx_vertex_ref r = { rnd() ^ (rnd() << 21), rnd() ^ (rnd() << 21) };
            same &= rsx_vg_element(&r) == rsx_vertex_element_index(r.vertex_id, r.base_index, i & 1u, (i >> 1) & 1u);
        }
        CHECK(same, "part 3: rsx_vg_element == rsx_vertex_element_index (frequency 0/1) on 2^20 random refs");
    }
    memset(&p, 0, sizeof p); rsx_vertex_layout_plan_init(&p.layout, 0u);
    CHECK(run_draw(&p, 16, 64u << 20, &comps, &bad) == RSX_VG_NO_INPUTS, "part 3: no inputs stays on the CPU");
}

int main(void)
{
    rsx_vg_lut_build(g_lut);
    const char* text = RSX_VG_SHARED(RSX_VG_TEXT);
    char n1[16], n2[16];
    snprintf(n1, sizeof n1, "%uu", RSX_VG_LUT_CMPXY); snprintf(n2, sizeof n2, "%uu", RSX_VG_LUT_CMPZ);
    CHECK(strstr(text, n1) && strstr(text, n2) && strstr(text, "256u + vg_be16") && RSX_VG_LUT_SNORM16 == 256u &&
          RSX_VG_LUT_UNORM8 == 0u && RSX_VG_LUT_WORDS == 65536u + 256u + 2048u + 1024u &&
          sizeof(rsx_vg_attr) == 32u && sizeof(RSX_VG_HLSL) > 1000u,
          "shared text: LUT offsets match RSX_VG_LUT_*, table row 32 bytes, HLSL %u bytes", (unsigned)sizeof(RSX_VG_HLSL));
    part1();
    part2();
    part3();
    if (getenv("VG_PRINT_HLSL")) puts(RSX_VG_HLSL);
    printf(g_fail ? "[FAIL] test_vtx_gpu\n" : "[PASS] test_vtx_gpu\n");
    return g_fail;
}
