#!/usr/bin/env bash
# PM argument host test (was patch 1077's; since ps3recomp v0.15.0 upstream PR #309 sets the same layout, so this
# test checks upstream's code in the patched tree): a policy module's r4 holds cellSpursModuleEntry's u64 workload
# argument in the preferred doubleword slot (SPU ABI 2.1.2: word 0 = high half, word 1 = low half). Compiles the r4
# statements of the first wkl_data site in the patched spurs_policy.c and models what KZ3's WWS job PM (EBOOT
# 0x012BF080) does with r4 at entry: word 1 (r4 << 4 bytes, LS 0x1410) is the job list header, word 0 the audit buffer
# EA (0x26D0: 0 = no audit, else a 16-byte FFFF PUT at EA + 0x40 + 16 * spuIndex). For the eleven KZ3 job lists
# (32-bit EAs from a boot log) the header must come out right and no audit PUT may land in the list (header + entries
# 0x4015A700..0x4015A78F for jm_generic). The old {lo, lo, hi, hi} layout is the negative control: it must hit the
# list (else this test proves nothing). Also fails when any r4-from-wkl_data site in spurs_policy.c has another
# layout. Self-contained: no game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_pm_arg.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
F="$P/runtime/spu/spurs_policy.c"
[[ -f "$F" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
sites="$(grep -c 'gpr\[4\]\._u32\[0\] = .*wkl_data' "$F")"
[[ "$sites" -ge 1 ]] || { echo "FAIL: no r4-from-wkl_data site in $F"; exit 1; }
badsite="$(grep -n -A1 'gpr\[4\]\._u32\[0\] = .*wkl_data' "$F" | grep -v -e '_u32\[0\] = (uint32_t)(wkl_data >> 32);' \
           -e '_u32\[1\] = (uint32_t)wkl_data;' -e '^--$')"
[[ -z "$badsite" ]] || { echo "FAIL: r4 site with another layout in $F:"; echo "$badsite" | head -4; exit 1; }
SNIP="$(awk '/gpr\[4\]\._u32\[0\] = .*wkl_data/ {on = 1} on && !/gpr\[4\]\._u32/ {exit} on {print}' "$F" \
        | sed 's/ctx->gpr\[4\]\._u32/w/g')"
cat > "$O/t_pm_arg.c" <<C
#include <stdint.h>
#include <stdio.h>
/* upstream's statements (first site of $sites), ctx->gpr[4]._u32 renamed w; old = the pre-PR-309 layout */
static void kz3_pm_arg_reg(uint32_t w[4], uint64_t wkl_data, int old)
{
    w[0] = w[1] = w[2] = w[3] = 0;
    if (old) { w[0] = w[1] = (uint32_t)wkl_data; w[2] = w[3] = (uint32_t)(wkl_data >> 32); return; }
$SNIP
}
/* KZ3 boot log (spu-60-guard): the eleven Wws_Job workloads' AddWorkload data. */
static const uint64_t k_lists[] = {0x4010CA80u, 0x40114C00u, 0x40118D80u, 0x4013BF80u, 0x40144100u, 0x40146280u,
                                   0x40148400u, 0x4014A580u, 0x4015A700u, 0x015D0200u, 0x015E0080u};
/* returns the number of audit PUTs (SPU index 0..5) landing in [hdr, hdr + 0x90) */
static int model(uint64_t data, int old, int* hdr_ok)
{
    uint32_t w[4];
    kz3_pm_arg_reg(w, data, old);
    uint32_t hdr = w[1];                 /* shlqbyi r4, 4 -> LS 0x1410 */
    uint32_t audit = w[0];               /* 0x26D0: biz r4 */
    *hdr_ok = hdr == (uint32_t)data;
    int hits = 0;
    if (audit)
        for (uint32_t i = 0; i < 6; i++) {
            uint32_t put = audit + 0x40u + 16u * i;   /* 16-byte FFFF terminator PUT, 0x2770..0x27AC */
            if (put + 16u > hdr && put < hdr + 0x90u) hits++;
        }
    return hits;
}
int main(void)
{
    int fail = 0, old_hits = 0;
    for (unsigned i = 0; i < sizeof k_lists / sizeof k_lists[0]; i++) {
        int ok, h = model(k_lists[i], 0, &ok);
        if (!ok || h) { printf("FAIL: list 0x%08X: header %s, %d audit PUTs into the list\n",
                               (unsigned)k_lists[i], ok ? "ok" : "WRONG", h); fail = 1; }
        int ok_old; old_hits += model(k_lists[i], 1, &ok_old);
    }
    /* a 64-bit argument: high half in word 0, low half in word 1 */
    uint32_t w[4]; kz3_pm_arg_reg(w, 0x0000000112345678ull, 0);
    if (w[0] != 1u || w[1] != 0x12345678u) { printf("FAIL: u64 layout %08X %08X\n", w[0], w[1]); fail = 1; }
    printf("upstream r4 layout ($sites sites): 11 lists, header ok, 0 audit PUTs into a list%s\n", fail ? " -- NO" : "");
    printf("old layout (negative control): %d audit PUTs into the lists\n", old_hits);
    if (old_hits == 0) { printf("FAIL: negative control did not reproduce the clobber\n"); fail = 1; }
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
C
cc -O1 -Wall -o "$O/t_pm_arg" "$O/t_pm_arg.c" || { echo "FAIL: compile"; exit 1; }
"$O/t_pm_arg"
