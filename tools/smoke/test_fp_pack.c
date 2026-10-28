/*
 * Host test for patch 2010: the RSX fragment-program pack/unpack opcodes (PK4 UP4 PK2 UP2 PKB UPB PK16 UP16
 * PKG UPG) in ps3recomp's rsx_fp_decompiler.c. Native build, no D3D12:
 *
 *   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fp_pack.sh
 *
 * Hand-assembles two programs, decompiles them, checks the HLSL names the helper for every opcode, carries the
 * helper definitions only when a pack opcode is used, has no "unhandled FP opcode" left, and writes each HLSL to
 * build/smoke-host/ so the script can compile it with glslangValidator's HLSL front end (syntax/type check; the
 * D3D12 compiler is not run here).
 * Program 1 is KZ3's level idiom (pass 11 RPCS3 capture): PKG r1 <- const; UPB r0 <- r1.
 */
#include <stdio.h>
#include <string.h>
#include "rsx_fp_decompiler.h"

static int g_pass, g_fail;

static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}
static void put_float(u8* p, float f) { u32 h; memcpy(&h, &f, 4); put_word(p, h); }

static void check(const char* t, const char* hlsl, const char* needle, int want)
{
    int has = strstr(hlsl, needle) != NULL;
    if (has == want) { printf("[PASS] %s: %s\"%s\"\n", t, want ? "" : "no ", needle); g_pass++; }
    else { printf("[FAIL] %s: %s\"%s\"\n", t, want ? "missing " : "unexpected ", needle); g_fail++; }
}

#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         1u
#define OUTREG(i)   ((u32)(i) << 1)
#define SWZ_IDENT   ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define T_TEMP 0u
#define T_CONST 2u
#define REG(i) ((u32)(i) << 2)
#define CTRL_32BIT 0x40u

/* One instruction (opcode, dest reg, src0) at p; a constant source appends 16 bytes. Returns bytes written. */
static u32 emit(u8* p, u32 op, u32 dst, u32 src_type, u32 src_reg, int end)
{
    put_word(p + 0, OPC(op & 0x3F) | OUTREG(dst) | OUTMASK_ALL | (end ? END : 0));
    put_word(p + 4, src_type | REG(src_reg) | SWZ_IDENT | EXEC_ALWAYS);
    put_word(p + 8, (op & 0x40) ? (1u << 31) : 0);
    put_word(p + 12, 0);
    if (src_type != T_CONST) return 16;
    put_float(p + 16, 0.5f); put_float(p + 20, 0.25f); put_float(p + 24, 1.0f); put_float(p + 28, 0.0f);
    return 32;
}

static void dump(const char* path, const char* hlsl)
{
    FILE* f = fopen(path, "w");
    if (!f) { printf("[FAIL] cannot write %s\n", path); g_fail++; return; }
    fputs(hlsl, f);
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* outdir = argc > 1 ? argv[1] : ".";
    static char hlsl[65536];
    char path[512];
    u8 prog[1024];

    /* 1: PKG then UPB (KZ3's in-shader linear -> sRGB step). */
    memset(prog, 0, sizeof prog);
    u32 n = emit(prog, 0x2C, 1, T_CONST, 0, 0);
    n += emit(prog + n, 0x28, 0, T_TEMP, 1, 1);
    int c = rsx_fp_decompile(prog, n, CTRL_32BIT, hlsl, sizeof hlsl);
    check("pkg-upb decodes", c >= 2 ? "ok" : "", "ok", 1);
    check("pkg-upb", hlsl, "rsx_pk_u8(rsx_lin_to_srgb(", 1);
    check("pkg-upb", hlsl, "rsx_up_u8(((r[1]).xyzw).x)", 1);
    check("pkg-upb", hlsl, "uint rsx_pk_u8(float4 v)", 1);
    check("pkg-upb", hlsl, "unhandled FP opcode", 0);
    snprintf(path, sizeof path, "%s/fp_pack_1.hlsl", outdir); dump(path, hlsl);

    /* 2: every pack/unpack opcode once, from a constant into r2, then MOV r0 <- r2. */
    static const u32 ops[] = {0x13, 0x14, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2A, 0x2C, 0x2D};
    static const char* want[] = {"rsx_pk_s8(", "rsx_up_s8(", "rsx_pk_h2(", "rsx_up_h2(", "rsx_pk_u8(",
                                 "rsx_up_u8(", "rsx_pk_u16(", "rsx_up_u16(", "rsx_lin_to_srgb(", "rsx_srgb_to_lin("};
    memset(prog, 0, sizeof prog);
    n = 0;
    for (u32 i = 0; i < sizeof ops / sizeof ops[0]; i++) n += emit(prog + n, ops[i], 2, T_CONST, 0, 0);
    n += emit(prog + n, 0x01, 0, T_TEMP, 2, 1);
    c = rsx_fp_decompile(prog, n, CTRL_32BIT, hlsl, sizeof hlsl);
    check("all-ops decodes", c >= 11 ? "ok" : "", "ok", 1);
    for (u32 i = 0; i < sizeof ops / sizeof ops[0]; i++) check("all-ops", hlsl, want[i], 1);
    check("all-ops", hlsl, "unhandled FP opcode", 0);
    snprintf(path, sizeof path, "%s/fp_pack_2.hlsl", outdir); dump(path, hlsl);

    /* 3: no pack opcode -> no helpers (programs without PK/UP keep their previous text). */
    memset(prog, 0, sizeof prog);
    n = emit(prog, 0x01, 0, T_CONST, 0, 1);
    rsx_fp_decompile(prog, n, CTRL_32BIT, hlsl, sizeof hlsl);
    check("plain MOV", hlsl, "rsx_pk_u8", 0);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
