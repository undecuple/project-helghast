/* Host test for KZ3 patches 2200/2201: Y16_X16_FLOAT channel order. Built by test_y16x16f.sh, which extracts
 * ld_float_tex_mapping and the surf-rg16 shader's write expression from the patched rsx_live_draw.c into
 * y16x16f_extract.h (no copy of the logic lives here) and links the patched rsx_texture_layout.c.
 * Model: half decode, the compute pass (b = R,G,B,A bytes of the guest word), the guest-memory decode
 * (per-half byte swap, memory order) and the D3D12 SRV mapping presentation. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "y16x16f_extract.h"
#include "rsx_texture_layout.h"

static int fails;
#define CHECK(name, cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s (line %d)\n", name, __LINE__);                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* presented value of one output channel: comp halves c[2], mapping bits 3*out */
static uint32_t present(uint32_t mapping, const uint16_t c[2], int out)
{
    const uint32_t sel = (mapping >> (3 * out)) & 7u;
    return sel == 4u ? 0u : sel == 5u ? 0x3C00u : sel <= 1u ? c[sel] : 0xDEADu; /* 5 = one as half 1.0 */
}
/* patch 2065's pass: word bytes A,R,G,B in guest memory order mem[0..3] -> b.x=R b.y=G b.z=B b.w=A */
static void pass(const uint8_t mem[4], uint16_t c[2])
{
    const uint32_t b[4] = {mem[1], mem[2], mem[3], mem[0]};
    c[0] = (uint16_t)((b[SH_C0_HI] << 8) | b[SH_C0_LO]);
    c[1] = (uint16_t)((b[SH_C1_HI] << 8) | b[SH_C1_LO]);
}
/* decode_guest_texture_float: per-half byte swap, memory order (comp 0 = first half) */
static void decode(const uint8_t mem[4], uint16_t c[2])
{
    uint8_t d[4];
    for (int k = 0; k < 4; k += 2) {
        d[k] = mem[k + 1];
        d[k + 1] = mem[k];
    }
    memcpy(c, d, 4); /* little-endian host, as the GPU reads R16G16 */
}

int main(void)
{
    const uint32_t remap = 0x29E4u, map = ld_float_tex_mapping(0x9Fu, remap);
    /* known G-buffer pixel: A=0x3C R=0x00 -> Y = 0x3C00, G=0x40 B=0x00 -> X = 0x4000 */
    const uint8_t px[4] = {0x3C, 0x00, 0x40, 0x00};
    uint16_t c[2];
    pass(px, c);
    CHECK("gbuffer-R-is-X", present(map, c, 0) == 0x4000u);
    CHECK("gbuffer-G-is-Y", present(map, c, 1) == 0x3C00u);
    CHECK("gbuffer-B-zero-A-one", present(map, c, 2) == 0u && present(map, c, 3) == 0x3C00u);
    /* net unchanged: the old pair (pass comp0 = X, sel {1,0,1,0}) gives the same presented R,G for every word */
    {
        const uint32_t old_sel[2] = {0u, 1u}; /* old sel_rg[R=1]=0, sel_rg[G=2]=1 */
        int same = 1;
        for (uint32_t w = 0; w < 0x20000u; w++) {
            uint8_t m[4] = {(uint8_t)(w * 2654435761u >> 24), (uint8_t)(w >> 1), (uint8_t)w,
                            (uint8_t)(w * 40503u >> 8)};
            uint16_t o[2] = {(uint16_t)((m[2] << 8) | m[3]), (uint16_t)((m[0] << 8) | m[1])}; /* old pass: X, Y */
            pass(m, c);
            if (present(map, c, 0) != o[old_sel[0]] || present(map, c, 1) != o[old_sel[1]]) same = 0;
        }
        CHECK("gbuffer-net-unchanged-vs-old-pair", same);
    }
    /* a true guest-memory Y16_X16_FLOAT texel: first half Y = 1.0 (BE 3C 00), second X = 2.0 (BE 40 00) */
    const uint8_t tex[4] = {0x3C, 0x00, 0x40, 0x00};
    decode(tex, c);
    CHECK("guest-tex-R-is-X", present(map, c, 0) == 0x4000u);
    CHECK("guest-tex-G-is-Y", present(map, c, 1) == 0x3C00u);
    /* the same 4 memory bytes presented identically through both paths, for many words */
    {
        int same = 1;
        for (uint32_t w = 0; w < 0x20000u; w++) {
            uint8_t m[4] = {(uint8_t)(w * 2654435761u >> 24), (uint8_t)(w >> 1), (uint8_t)w,
                            (uint8_t)(w * 40503u >> 8)};
            uint16_t a[2], t[2];
            pass(m, a);
            decode(m, t);
            for (int o = 0; o < 4; o++)
                if (present(map, a, o) != present(map, t, o)) same = 0;
        }
        CHECK("surface-path-equals-texture-path", same);
    }
    /* backend (patch 2200) vs live-draw: every pure-crossbar remap 0xAAxx picks the same component per output */
    {
        int agree = 1;
        for (uint32_t x = 0; x < 256u; x++) {
            u8 m[4];
            rsx_texture_component_remap(0xAA00u | x, 0x9Fu, m); /* m[] is in crossbar order A,R,G,B */
            const uint32_t lm = ld_float_tex_mapping(0x9Fu, 0xAA00u | x);
            for (int o = 0; o < 4; o++)
                if (m[(o + 1) & 3] != ((lm >> (3 * o)) & 7u)) agree = 0;
        }
        CHECK("backend-equals-live-draw-mapping", agree);
        u8 id[4];
        rsx_texture_component_remap(0xAAE4u, 0x9Fu,
                                    id); /* identity crossbar, A,R,G,B <- comp 0,1,0,1 (RPCS3 {R,G,R,G}) */
        CHECK("backend-identity-0101", id[0] == 0 && id[1] == 1 && id[2] == 0 && id[3] == 1);
    }
    if (fails) {
        printf("test_y16x16f: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_y16x16f: all checks pass\n");
    return 0;
}
