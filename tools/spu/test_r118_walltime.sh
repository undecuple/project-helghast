#!/usr/bin/env bash
# Patch 9633 host test: the Windows sys_time_get_current_time converts FILETIME (100 ns since 1601) to Unix seconds and
# nanoseconds. Extracts the conversion block from the patched sys_timer.c (work/ps3recomp-patched or SPU_TREE=DIR; SKIP
# without it) and checks fixed points and a round trip against the host's CLOCK_REALTIME.
#   nix develop --command tools/sandbox.sh -- tools/spu/test_r118_walltime.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
T="${SPU_TREE:-$ROOT/work/ps3recomp-patched}"
S="$T/runtime/syscalls/sys_timer.c"
[[ -f "$S" ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "KZ3 patch 9633 begin" "$S" || { echo "FAIL: $T has no patch 9633"; exit 1; }
grep -q 'GetSystemTimePreciseAsFileTime(&ft)' "$S" || { echo "FAIL: Windows branch does not read FILETIME"; exit 1; }
sed -n '/KZ3 patch 9633 begin/,/KZ3 patch 9633 end/p' "$S" > "$O/walltime_block.h"
cat > "$O/t_walltime.c" <<'C'
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include WT_BLOCK
static int fails;
static void chk(uint64_t ticks, uint64_t es, uint64_t en)
{
    uint64_t s, n; sys_time_filetime_to_unix(ticks, &s, &n);
    if (s != es || n != en) { printf("FAIL ticks=%llu -> %llu.%09llu, want %llu.%09llu\n", (unsigned long long)ticks,
                                     (unsigned long long)s, (unsigned long long)n, (unsigned long long)es, (unsigned long long)en); fails++; }
}
int main(void)
{
    chk(116444736000000000ULL, 0, 0);                         /* 1970-01-01T00:00:00Z */
    chk(133000000000000000ULL, 1655526400ULL, 0);             /* 2022-06-18T04:26:40Z */
    chk(133000000001234567ULL, 1655526400ULL, 123456700ULL);  /* sub-second ticks -> ns */
    chk(0, 0, 0);                                              /* before 1970 clamps to 0 */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    const uint64_t ticks = (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100ULL + 116444736000000000ULL;
    chk(ticks, (uint64_t)ts.tv_sec, (uint64_t)ts.tv_nsec / 100ULL * 100ULL);
    printf("walltime: %s (now %lld s since 1970)\n", fails ? "mismatch" : "5 points ok", (long long)ts.tv_sec);
    return fails != 0;
}
C
rm -f "$O/t_walltime"
${CC:-gcc} -std=gnu11 -O2 -DWT_BLOCK="\"$O/walltime_block.h\"" -o "$O/t_walltime" "$O/t_walltime.c" 2>&1 | grep -m8 error
[[ -x "$O/t_walltime" ]] || { echo "FAIL: build"; exit 1; }
"$O/t_walltime"; r=$?
[[ $r == 0 ]] && echo PASS || echo FAIL
exit $r
