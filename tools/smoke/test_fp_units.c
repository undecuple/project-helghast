/*
 * Host test for KZ3 patch 2042: rsx_fp_texture_unit_mask() in ps3recomp's rsx_fp_decompiler.c returns the
 * texture units a fragment program samples (TEX/TXP/TXD/TXL/TXB), skipping inline constants, so the PSO key and
 * HLSL can ignore state left bound on other units. Native build, no D3D12, no game data:
 *
 *   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_units.sh
 */
#include <stdio.h>
#include <string.h>
#include "rsx_fp_decompiler.h"

static int g_pass, g_fail;

static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24);
    p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);
    p[3] = (u8)be;
}

#define OPC(x) ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END 1u
#define UNIT(u) ((u32)(u) << 17)
#define SWZ_IDENT ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define T_TEMP 0u
#define T_INPUT 1u
#define T_CONST 2u

/* One instruction; a constant source appends a 16-byte literal filled with `fill` words. */
static u32 emit(u8* p, u32 op, u32 unit, u32 src_type, int end, u32 fill)
{
    put_word(p + 0, OPC(op & 0x3F) | UNIT(unit) | OUTMASK_ALL | (end ? END : 0));
    put_word(p + 4, src_type | SWZ_IDENT | EXEC_ALWAYS);
    put_word(p + 8, 0);
    put_word(p + 12, 0);
    if (src_type != T_CONST) return 16;
    for (int i = 0; i < 4; i++) put_word(p + 16 + 4 * i, fill);
    return 32;
}

static void expect(const char* name, u32 got, u32 want)
{
    if (got == want) {
        printf("[PASS] %s: 0x%04X\n", name, got);
        g_pass++;
    } else {
        printf("[FAIL] %s: got 0x%04X want 0x%04X\n", name, got, want);
        g_fail++;
    }
}

int main(void)
{
    u8 prog[512];
    u32 n;

    /* TEX unit 3, MOV with a constant whose words look like "TEX unit 15", TXP unit 9 (end). */
    memset(prog, 0, sizeof prog);
    n = emit(prog, 0x17, 3, T_INPUT, 0, 0);
    n += emit(prog + n, 0x01, 0, T_CONST, 0, OPC(0x17) | UNIT(15));
    n += emit(prog + n, 0x18, 9, T_INPUT, 1, 0);
    expect("tex3 + const + txp9", rsx_fp_texture_unit_mask(prog, n), (1u << 3) | (1u << 9));

    /* TXD 1, TXL 2, TXB 4 (opcodes 0x19, 0x2F, 0x31); a MUL whose unit field is 7 does not count. */
    memset(prog, 0, sizeof prog);
    n = emit(prog, 0x19, 1, T_INPUT, 0, 0);
    n += emit(prog + n, 0x2F, 2, T_INPUT, 0, 0);
    n += emit(prog + n, 0x02, 7, T_TEMP, 0, 0);
    n += emit(prog + n, 0x31, 4, T_INPUT, 1, 0);
    expect("txd1 txl2 txb4, mul ignored", rsx_fp_texture_unit_mask(prog, n), (1u << 1) | (1u << 2) | (1u << 4));

    /* No texture instruction. */
    memset(prog, 0, sizeof prog);
    n = emit(prog, 0x01, 5, T_INPUT, 1, 0);
    expect("mov only", rsx_fp_texture_unit_mask(prog, n), 0);

    /* No END inside the buffer: malformed, 0. */
    memset(prog, 0, sizeof prog);
    n = emit(prog, 0x17, 6, T_INPUT, 0, 0);
    expect("no end", rsx_fp_texture_unit_mask(prog, n), 0);
    expect("null", rsx_fp_texture_unit_mask(NULL, 64), 0);

    printf("test_fp_units: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
