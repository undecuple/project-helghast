#!/usr/bin/env bash
# Host test (SPU pass 133): the same-run toggle of native gates. C half: kz3_native_toggle.h (slot list, phase rule,
# period text) and, when the patched toolkit exists, that a toggled slot's gate is native only in odd phases and
# KZ3_NATIVE_TOGGLE unset leaves the gates as before (native_default_probe). Python half: tools/spu/toggle_ab.py on a
# made-up KZ3FTRC1 trace (calls in phases, one straddling a boundary). The design is KZ2's (SPU pass 22).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_native_toggle.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu"; mkdir -p "$O"
cat > "$O/t_native_toggle.c" <<'C'
#include "kz3_native_toggle.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    unsigned char m[64];
    CHECK(kz3_native_toggle_parse("0,4,6", m, 64) == 3 && m[0] && m[4] && m[6] && !m[5]);
    CHECK(kz3_native_toggle_parse("4-6", m, 64) == 3 && m[4] && m[5] && m[6] && !m[7]);
    CHECK(kz3_native_toggle_parse("6,4-5,5", m, 64) == 3);
    CHECK(kz3_native_toggle_parse("63", m, 64) == 1 && m[63]);
    CHECK(kz3_native_toggle_parse("64", m, 64) == -1 && !m[63]);
    CHECK(kz3_native_toggle_parse("6-4", m, 64) == -1);
    CHECK(kz3_native_toggle_parse("4,x", m, 64) == -1 && !m[4]);
    CHECK(kz3_native_toggle_parse("4;6", m, 64) == -1);
    CHECK(kz3_native_toggle_parse("-1", m, 64) == -1);
    CHECK(kz3_native_toggle_parse("", m, 64) == 0);
    CHECK(kz3_native_toggle_phase(0, 2000000000ull) == 0 && kz3_native_toggle_phase(1999999999ull, 2000000000ull) == 0);
    CHECK(kz3_native_toggle_phase(2000000000ull, 2000000000ull) == 1 && (kz3_native_toggle_phase(5000000000ull, 2000000000ull) & 1) == 0);
    CHECK(kz3_native_toggle_period_ns(0) == 2000000000ull && kz3_native_toggle_period_ns("0.5") == 500000000ull);
    CHECK(kz3_native_toggle_period_ns("0") == 2000000000ull && kz3_native_toggle_period_ns("999") == 2000000000ull);
    if (fails) return 1;
    puts("kz3_native_toggle.h: ok");
    return 0;
}
C
${CC:-cc} -std=gnu17 -O2 -Wall -Werror -I "$ROOT/port/spu" -o "$O/t_native_toggle" "$O/t_native_toggle.c" 2>&1 | grep -m5 error
"$O/t_native_toggle" || { echo "FAIL test_native_toggle (C)"; exit 1; }
grep -q 's_tog\[e->slot\]' "$ROOT/port/spu/kz3_native_jobs.c" || { echo "FAIL: no toggle gate in gate_add()"; exit 1; }
python3 - "$O/t_native_toggle.bin" <<'PY'
import struct, sys
P = 2_000_000_000
def ev(t0, t1, img): return struct.pack("<QQIBBH", t0, t1, img, 33, 0, 0)
e = []
# image 227: phase 0 (lifted) 100 us, phase 1 (native) 60 us, phase 2 lifted 110 us, phase 3 native 70 us, phase 4 lifted 100 us
for k, us in ((0, 100), (1, 60), (2, 110), (3, 70), (4, 100)):
    for j in range(10):
        t = k * P + 1_000_000 + j * 1_000_000
        e.append(ev(t, t + us * 1000, 227))
e.append(ev(P - 50_000, P + 50_000, 227))            # straddles 0|1: dropped
e.append(ev(3 * P + 5, 3 * P + 99_000, 241))         # another image, ignored by --img 227
with open(sys.argv[1], "wb") as f:
    f.write(b"KZ3FTRC1" + struct.pack("<IIQ", 0, len(e), 0) + b"".join(e))
PY
out=$(python3 "$ROOT/tools/spu/toggle_ab.py" "$O/t_native_toggle.bin" --period 2 --img 227)
echo "$out" | grep -q "1 straddling calls dropped" || { echo "FAIL: straddle"; echo "$out"; exit 1; }
echo "$out" | grep -q "lifted n=30 med 100.0 p95 110.0 mean 103.3 us | native n=20 med 65.0 p95 70.0 mean 65.0 us (x0.650) | paired 2: native lower 2, median diff -40.0 us, sign p 0.5" ||
  { echo "FAIL: numbers"; echo "$out"; exit 1; }
echo "test_native_toggle: ok"
