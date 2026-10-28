#!/usr/bin/env bash
# Patch 1103 host test: kz3_dec_ticks (SPU_RdDec's ns -> decrementer ticks) has no 64-bit overflow.
# Takes the function from the patched runtime/spu/spu_channels.c when present, else from the patch file itself, and
# compares it with a 128-bit reference for values below and above the old expression's wrap (231.2 s), including the
# wrap point +/- 1 ns and 1 h / 24 h / 10 days. Negative control: the pre-1103 expression must FAIL past the wrap.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_dec_ticks.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t_dec"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
F="$P/runtime/spu/spu_channels.c"; PF="$ROOT/port/patches/1103-spu-decrementer-overflow.patch"
if [[ -f "$F" ]] && grep -q kz3-dec-ticks "$F"; then SRC="$F"
elif [[ -f "$PF" ]]; then SRC="$PF"
else echo "SKIP: neither patched toolkit nor $PF"; exit 0; fi
{ echo '#include <stdint.h>'; echo '#include <stdio.h>'; echo '#define SPU_DECREMENTER_HZ 79800000ull'
  sed -n '/kz3-dec-ticks begin/,/kz3-dec-ticks end/p' "$SRC" | sed 's/^+//'
  cat <<'C'
static uint32_t old_ticks(uint64_t e) { return (uint32_t)((e * SPU_DECREMENTER_HZ) / 1000000000ull); }
static uint32_t ref_ticks(uint64_t e) { return (uint32_t)(((unsigned __int128)e * SPU_DECREMENTER_HZ) / 1000000000ull); }
int main(void) {
    const uint64_t wrap = 0xFFFFFFFFFFFFFFFFull / SPU_DECREMENTER_HZ;   /* first ns whose product overflows */
    uint64_t v[] = {0, 1, 999999999ull, 1000000000ull, 123456789012ull, wrap - 1, wrap, wrap + 1, wrap + 1000000000ull,
                    300ull * 1000000000ull, 3600ull * 1000000000ull, 86400ull * 1000000000ull, 864000ull * 1000000000ull};
    int bad = 0, oldbad = 0, n = sizeof v / sizeof v[0];
    for (int i = 0; i < n; i++) {
        if (kz3_dec_ticks(v[i]) != ref_ticks(v[i])) { printf("FAIL new %llu: %u != %u\n", (unsigned long long)v[i], kz3_dec_ticks(v[i]), ref_ticks(v[i])); bad++; }
        if (v[i] <= wrap && old_ticks(v[i]) != kz3_dec_ticks(v[i])) { printf("FAIL differs from old below the wrap at %llu\n", (unsigned long long)v[i]); bad++; }
        if (old_ticks(v[i]) != ref_ticks(v[i])) oldbad++;
    }
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 200000; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t e = x >> (x & 31);
        if (kz3_dec_ticks(e) != ref_ticks(e)) { bad++; if (bad < 5) printf("FAIL random %llu\n", (unsigned long long)e); } }
    printf("wrap at %llu ns (%.1f s); new expression: %d failures; old expression wrong on %d of %d fixed points\n",
           (unsigned long long)wrap, wrap / 1e9, bad, oldbad, n);
    if (oldbad == 0) { puts("FAIL: negative control (old expression) did not fail past the wrap"); return 2; }
    return bad ? 1 : 0;
}
C
} > "$O/t.c"
gcc -O1 -o "$O/t" "$O/t.c" 2>&1 | head -5 && "$O/t" && echo "PASS test_dec_ticks (source: ${SRC#$ROOT/})"
