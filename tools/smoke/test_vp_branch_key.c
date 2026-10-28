/* Host test for KZ3 patch 2077 (RSX pass 73): the VP start slot is part of the pipeline key exactly when it reaches
 * the HLSL. The walker keys a pipeline by the VP words; the decompiler makes a branching program's absolute targets
 * relative to rsx_vp_set_branch_base(start), so the same words uploaded at two slots give two shaders.
 * Built by test_vp_branch_key.sh against the patched toolkit's rsx_vp_decompiler.c. */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char hlsl_a[65536], hlsl_b[65536];

/* Words in host order (the decompiler reads them little-endian, as the walker hands them over). */
static void decompile(const u32* w, u32 n, u32 start, char* out)
{
    rsx_vp_set_branch_base(start);
    const int r = rsx_vp_decompile_ex((const u8*)w, n * 16u, 0, out, (u32)sizeof hlsl_a);
    CHECK(r > 0);
}

int main(void)
{
    const u64 seed = 1469598103934665603ull;
    /* branch-free: four zero (NOP) instructions, end bit on the last */
    u32 flat[16]; memset(flat, 0, sizeof flat); flat[15] = 1u;
    /* branching: instruction 0 = BRA (scalar opcode 0x08 in D1[27:31]) to absolute slot 2 (D3[29:31] = 2) */
    u32 br[16]; memset(br, 0, sizeof br);
    br[1] = 0x08u << 27; br[3] = 2u << 29; br[15] = 1u;

    CHECK(rsx_vp_has_flow((const u8*)flat, sizeof flat) == 0);
    CHECK(rsx_vp_has_flow((const u8*)br, sizeof br) == 1);
    /* flow after the end bit is not part of the program */
    { u32 tail[16]; memcpy(tail, flat, sizeof tail); tail[3] = 1u; tail[5] = 0x08u << 27;
      CHECK(rsx_vp_has_flow((const u8*)tail, sizeof tail) == 0); }

    /* two records that differ only in vp_start (0 vs 1, and 0 vs 8) */
    decompile(br, 4, 0, hlsl_a); decompile(br, 4, 1, hlsl_b);
    CHECK(strcmp(hlsl_a, hlsl_b) != 0);   /* the slot reaches the shader: target block 2 vs 1 */
    CHECK(rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 0) != rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 1));
    CHECK(rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 0) != rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 8));
    CHECK(rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 8) == rsx_vp_key_branch_base(seed, (const u8*)br, sizeof br, 8));

    decompile(flat, 4, 0, hlsl_a); decompile(flat, 4, 8, hlsl_b);
    CHECK(strcmp(hlsl_a, hlsl_b) == 0);   /* branch-free: same shader at any slot */
    CHECK(rsx_vp_key_branch_base(seed, (const u8*)flat, sizeof flat, 0) == seed);   /* key unchanged (old keys stay valid) */
    CHECK(rsx_vp_key_branch_base(seed, (const u8*)flat, sizeof flat, 8) == seed);
    rsx_vp_set_branch_base(0);

    if (fails) { printf("test_vp_branch_key: %d FAILED\n", fails); return 1; }
    printf("test_vp_branch_key: PASS\n");
    return 0;
}
