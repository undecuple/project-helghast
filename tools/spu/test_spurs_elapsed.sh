#!/usr/bin/env bash
# Patch 1042 host test: the [spurs-stuck] watcher's elapsed time never wraps when a run's start stamp is later than
# the watcher's clock read. Compiles kz3_spurs_elapsed_ms as it stands in the patched cellSpurs.c (between the
# kz3-spurs-elapsed markers) and checks it, plus a negative control (the old plain now - t0 wraps).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_spurs_elapsed.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
F="$P/libs/spurs/cellSpurs.c"
SNIP="$(sed -n '/kz3-spurs-elapsed begin/,/kz3-spurs-elapsed end/p' "$F" | sed '1d;$d')"
[[ -n "$SNIP" ]] || { echo "FAIL: kz3_spurs_elapsed_ms not found in $F (patch 1042 missing)"; exit 1; }
cat > "$O/t_elapsed.c" <<C
#include <stdio.h>
#include <stdint.h>
typedef uint64_t u64;
$SNIP
static int fails;
static void chk(const char* what, u64 got, u64 want) {
    if (got != want) { printf("FAIL %s: %llu want %llu\n", what, (unsigned long long)got, (unsigned long long)want); fails++; }
}
int main(void) {
    chk("normal", kz3_spurs_elapsed_ms(10000, 6000), 4000);
    chk("equal", kz3_spurs_elapsed_ms(5000, 5000), 0);
    chk("t0 193 ms after now (RSX pass 30 case)", kz3_spurs_elapsed_ms(1000, 1193), 0);
    chk("t0 far after now", kz3_spurs_elapsed_ms(0, ~0ull), 0);
    /* negative control: the pre-1042 expression gives the reported 18446744073709551423 */
    u64 now = 1000, t0 = 1193;
    chk("negative control (old now - t0 wraps)", now - t0, 18446744073709551423ull);
    printf(fails ? "test_spurs_elapsed: %d FAIL\n" : "test_spurs_elapsed: all pass%.0d\n", fails);
    return fails != 0;
}
C
gcc -std=gnu17 -O2 -Wall -o "$O/t_elapsed" "$O/t_elapsed.c" 2>&1 | grep -m5 -E 'error|warning'
"$O/t_elapsed"
