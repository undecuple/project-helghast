/* Host test for KZ3 patch 2100 (RSX pass 95; based on sp00nznet/ps3recomp PR #485 by n0madic): BRB/CLB branch on
 * SET_TRANSFORM_BRANCH_BITS (0x1FF8) and block leaders reach slot 543 (the 544-slot transform program memory).
 * Built by test_vp_branch_bits.sh against the patched toolkit's rsx_vp_decompiler.c. -DKZ3_TEST_OLD builds only the
 * long-program case, so the script can show that case fails on a decompiler without the patch. */
#include "rsx_vp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char hlsl[512 * 1024];

/* Flow instruction sca_op (0x09 BRI, 0x0C RET, 0x11 BRB, 0x12 CLB) to VP slot target: cond = 7 (TR), the target
 * split over D0[23], D2[0:5], D3[29:31]; a boolean branch tests bit `bit` (D3[23:27]) against D3[28]. No writes.
 * Encoding as PR #485's test (and the decompiler's vp_branch_target). */
static void vp_branch(u8* p, u32 sca_op, u32 target, u32 bit, u32 if_set)
{
    const u32 d0 = (0x3Fu << 15) | (7u << 10) | (((target >> 9) & 1u) << 23);
    const u32 d1 = sca_op << 27;
    const u32 d2 = (target >> 3) & 0x3Fu;
    const u32 d3 = (0x3Fu << 7) | (bit << 23) | (if_set << 28) | ((target & 7u) << 29);
    rsx_test_put_le32(p + 0, d0); rsx_test_put_le32(p + 4, d1);
    rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}
static void mov(u8* p, u32 out, u32 in, int end) { rsx_test_vp_mov_out(p, out, RSX_TEST_VP_SWZ_IDENT, in, end); }

static int balanced(const char* s)
{
    long d = 0;
    for (; *s; s++) { if (*s == '{') d++; else if (*s == '}') { if (--d < 0) return 0; } }
    return d == 0;
}

/* n-slot program: MOVs, a BRI at slot n-6 to n-4, another at n-3 to n-1 (both past slot 511 when n > 517). */
static u8 long_vp[544 * 16];
static void build_long(u32 n)
{
    memset(long_vp, 0, sizeof long_vp);
    for (u32 i = 0; i < n - 6; i++) mov(long_vp + i * 16, 0, 0, 0);
    vp_branch(long_vp + (n - 6) * 16, 0x09, n - 4, 0, 0);
    mov(long_vp + (n - 5) * 16, 3, 1, 0);   /* skipped */
    mov(long_vp + (n - 4) * 16, 1, 1, 0);
    vp_branch(long_vp + (n - 3) * 16, 0x09, n - 1, 0, 0);
    mov(long_vp + (n - 2) * 16, 3, 2, 0);   /* skipped */
    mov(long_vp + (n - 1) * 16, 2, 2, 1);   /* END */
}

int main(void)
{
    const u32 ns[2] = { 519, 544 };
    for (u32 t = 0; t < 2; t++) {
        const u32 n = ns[t];
        build_long(n);
        rsx_vp_set_branch_base(0);
        const int r = rsx_vp_decompile((const u8*)long_vp, n * 16u, hlsl, (u32)sizeof hlsl);
        CHECK(r > 0);
        if (r <= 0) continue;
        char want[48];
        for (u32 k = 0; k < 4; k++) {
            const u32 slot = k == 0 ? n - 5 : k == 1 ? n - 4 : k == 2 ? n - 2 : n - 1;   /* every leader past 511 */
            snprintf(want, sizeof want, "if (_pc == %u) {", slot);
            if (!strstr(hlsl, want)) { fprintf(stderr, "FAIL %u-slot VP: no \"%s\"\n", n, want); fails++; }
        }
        if (!balanced(hlsl)) { fprintf(stderr, "FAIL %u-slot VP: unbalanced braces\n", n); fails++; }
    }
#ifndef KZ3_TEST_OLD
    /* 0 MOV o0,v0  1 BRB !b3 -> 3  2 MOV o1,v1  3 CLB b12 -> 6  4 MOV o2,v2  5 BRI -> 8  6 MOV o3,v3  7 RET
     * 8 MOV o4,v4 END (PR #485's case). */
    static u8 vp[9 * 16];
    mov(vp + 0 * 16, 0, 0, 0); vp_branch(vp + 1 * 16, 0x11, 3, 3, 0); mov(vp + 2 * 16, 1, 1, 0);
    vp_branch(vp + 3 * 16, 0x12, 6, 12, 1); mov(vp + 4 * 16, 2, 2, 0); vp_branch(vp + 5 * 16, 0x09, 8, 0, 0);
    mov(vp + 6 * 16, 3, 3, 0); vp_branch(vp + 7 * 16, 0x0C, 0, 0, 0); mov(vp + 8 * 16, 4, 4, 1);
    CHECK(rsx_vp_uses_branch_bits(vp, sizeof vp) == 1);
    build_long(519);
    CHECK(rsx_vp_uses_branch_bits(long_vp, 519 * 16u) == 0);   /* BRI only: keys unchanged */
    { static u8 tail[3 * 16]; mov(tail, 0, 0, 1); vp_branch(tail + 16, 0x11, 0, 0, 0); mov(tail + 32, 0, 0, 1);
      CHECK(rsx_vp_uses_branch_bits(tail, sizeof tail) == 0); }   /* BRB after the end bit is not the program */
    static const struct { u32 bits; const char* brb; const char* clb; } cases[] = {
        { 0x0000u, "BRB @1 -> 3 cond=7 test=0 swz=xxxx */\n        _pc = 3;",
                   "CLB @3 -> 6 cond=7 test=0 swz=xxxx */\n        _pc = 4;" },
        { 0x1008u, "BRB @1 -> 3 cond=7 test=0 swz=xxxx */\n        _pc = 2;",
                   "CLB @3 -> 6 cond=7 test=0 swz=xxxx */\n        _cs[_csp] = 4; _csp = min(_csp + 1, 7); _pc = 6;" },
        { 0x0008u, "BRB @1 -> 3 cond=7 test=0 swz=xxxx */\n        _pc = 2;",
                   "CLB @3 -> 6 cond=7 test=0 swz=xxxx */\n        _pc = 4;" },
    };
    for (u32 i = 0; i < 3; i++) {
        rsx_vp_set_branch_base(0);
        rsx_vp_set_branch_bits(cases[i].bits);
        const int r = rsx_vp_decompile(vp, sizeof vp, hlsl, (u32)sizeof hlsl);
        CHECK(r > 0);
        if (r <= 0) continue;
        if (!strstr(hlsl, cases[i].brb)) { fprintf(stderr, "FAIL bits %04x: BRB\n", cases[i].bits); fails++; }
        if (!strstr(hlsl, cases[i].clb)) { fprintf(stderr, "FAIL bits %04x: CLB\n", cases[i].bits); fails++; }
        CHECK(strstr(hlsl, "not modelled") == NULL);
        CHECK(balanced(hlsl));
    }
    rsx_vp_set_branch_bits(0);
#endif
    if (fails) { printf("test_vp_branch_bits: %d FAILED\n", fails); return 1; }
    printf("test_vp_branch_bits: PASS\n");
    return 0;
}
