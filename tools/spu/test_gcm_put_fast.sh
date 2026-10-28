#!/usr/bin/env bash
# Patch 1101 host test: the SPU PUT hook's EA -> IO lookup (gcm_ea2io_range in the patched cellGcmSys.c) answers
# "not RSX-visible" from the 1 MB offset table, and otherwise gives the old list scan's answer.
#  1. Source invariant: every IO mapping list activation (`active = 1`) in cellGcmSys.c has a populate_offset_table
#     call next to it, every deactivation (`active = 0`) a clear_offset_table call, and both resets clear the table.
#  2. Compiles the patched populate/clear helpers and gcm_ea2io_range with stubs, maps 38 x 1 MB chunks at
#     0x20000000 (KZ3's level-load MapMainMemory), an IO region at 0x00100000 and a 3 MB mapping, unmaps two, and
#     compares the result (io and room) with the old scan (KZ3_GCM_PUT_FAST=0, same code) for 200,000 EAs:
#     page edges, mapping edges, random. Self-contained: no game data.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_gcm_put_fast.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
C="$P/libs/video/cellGcmSys.c"
[[ -f "$C" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
grep -q 's_ea2io_fast' "$C" || { echo "FAIL: patch 1101 missing from $C"; exit 1; }
fail=0
# 1. writers keep the table and the list together
while IFS=: read -r ln _; do
  sed -n "$((ln-16)),$((ln+4))p" "$C" | grep -q 'populate_offset_table(' ||
    { echo "FAIL: list activation at $C:$ln has no populate_offset_table next to it"; fail=1; }
done < <(grep -n 'active = 1' "$C" | grep -v 's_nv309e')
while IFS=: read -r ln _; do
  sed -n "$((ln-3)),$((ln+3))p" "$C" | grep -q 'clear_offset_table(' ||
    { echo "FAIL: list deactivation at $C:$ln has no clear_offset_table next to it"; fail=1; }
done < <(grep -n 'active = 0' "$C")
nres=$(grep -c 'memset(s_io_mappings, 0, sizeof(s_io_mappings));' "$C")
ntab=$(grep -c 'memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));' "$C")
[[ "$nres" -ge 1 && "$ntab" -ge "$nres" ]] || { echo "FAIL: list resets $nres, table resets $ntab"; fail=1; }

# 2. lookup equivalence
POP="$(sed -n '/^static void populate_offset_table(u32 ea, u32 io, u32 size)/,/^}/p' "$C")"
CLR="$(sed -n '/^static void clear_offset_table(u32 ea, u32 io, u32 size)/,/^}/p' "$C")"
LOOK="$(sed -n '/^static int s_ea2io_fast/,/^}/p' "$C")"
[[ -n "$POP" && -n "$CLR" && -n "$LOOK" ]] || { echo "FAIL: helpers not found in $C"; exit 1; }
cat > "$O/t_gcm_put_fast.c" <<CEOF
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32; typedef uint16_t u16;
#define CELL_GCM_MAX_IO_MAPPINGS 512
typedef struct IoMapping { u32 ea; u32 io; u32 size; int active; } IoMapping;
static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];
static int s_io_mapping_hw; /* patch 1119 (PR #449): one past the highest slot used, raised by map() below */
/* patch 2083: populate_offset_table notes IO-mapped pages for the PPU store gate; off in this test (weak, unset). */
static int s_ppu_note_on;
extern int ppu_store_note_pages(uint32_t ea, uint32_t size, int on) __attribute__((weak));
$POP
$CLR
$LOOK
static int slot_n = 0;
static int map(u32 ea, u32 io, u32 size)
{   /* a list writer: free slot, offset table, activation (cellGcmMapEaIoAddress's order) */
    int i = 0; while (s_io_mappings[i].active) i++;
    populate_offset_table(ea, io, size);
    if (i >= s_io_mapping_hw) s_io_mapping_hw = i + 1;
    s_io_mappings[i] = (IoMapping){ea, io, size, 1}; slot_n++;
    return i;
}
static void unmap(int i)
{
    clear_offset_table(s_io_mappings[i].ea, s_io_mappings[i].io, s_io_mappings[i].size);
    s_io_mappings[i].active = 0;
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)rng; }
static long checks, bad, hits;
static void cmp(u32 ea)
{
    u32 rf = 0, rs = 0;
    s_ea2io_fast = 1; const u32 a = gcm_ea2io_range(ea, &rf);
    s_ea2io_fast = 0; const u32 b = gcm_ea2io_range(ea, &rs);
    checks++; if (b != 0xFFFFFFFFu) hits++;
    if (a != b || (b != 0xFFFFFFFFu && rf != rs)) {
        if (bad++ < 5) printf("mismatch ea 0x%08X: fast 0x%08X room 0x%X, scan 0x%08X room 0x%X\n", ea, a, rf, b, rs);
    }
}
int main(void)
{
    memset(s_io_address_table, 0xFF, sizeof s_io_address_table);
    memset(s_ea_address_table, 0xFF, sizeof s_ea_address_table);
    map(0x00100000u, 0, 0x00200000u);                                   /* cellGcmInit's IO region */
    int ch[38];
    for (int i = 0; i < 38; i++) ch[i] = map(0x20000000u + (u32)i * 0x100000u, 0x00200000u + (u32)i * 0x100000u, 0x100000u);
    const int big = map(0x30000000u, 0x02800000u, 0x300000u);
    unmap(ch[5]); unmap(ch[17]);
    map(0x40000000u, 0x00700000u, 0x100000u);                           /* reuses a freed slot */
    unmap(big);
    map(0x30100000u, 0x03000000u, 0x100000u);                           /* inside the old 3 MB range */
    for (u32 p = 0; p < 4096; p++) {                                     /* every page: edges and middle */
        const u32 b = p << 20;
        cmp(b); cmp(b + 0x7FFF0u); cmp(b + 0xFFFFFu); cmp(b - 1u);
    }
    for (int i = 0; i < 40; i++) { const u32 e = 0x20000000u + (u32)i * 0x100000u; cmp(e - 16); cmp(e + 16); }
    for (int i = 0; i < 160000; i++) {
        u32 ea = rnd();
        if (i & 1) ea = 0x1FF00000u + (ea % 0x02800000u);                 /* around the chunks */
        cmp(ea);
    }
    printf("%ld lookups, %ld hits, %ld mismatches\n", checks, hits, bad);
    if (hits < 1000) { printf("too few hits\n"); return 1; }
    return bad ? 1 : 0;
}
CEOF
CCX="${CC:-cc}"
"$CCX" -O2 -w -o "$O/t_gcm_put_fast" "$O/t_gcm_put_fast.c" || { echo "FAIL: compile"; exit 1; }
"$O/t_gcm_put_fast" || fail=1
[[ $fail = 0 ]] && echo "PASS test_gcm_put_fast" || { echo "FAIL test_gcm_put_fast"; exit 1; }
