#!/usr/bin/env bash
# Patch 1102 host test: the read-mostly SPU gates (SPU_READ_MOSTLY, .data$kz3rm) share no 64-byte cache line with
# any other variable in a MinGW-w64 PE link. Takes the patched toolkit's SPU_READ_MOSTLY macro (spu_context.h) and
# fence definitions (spu_channels.c), builds three TUs that interleave gate definitions with written counters
# (initialised and zero, as the runtime has them) and checks the linked exe's symbol table: the gates lie between
# the fences, every symbol outside the group lies on other lines. Also checks the patch's definitions in the
# patched sources. Needs x86_64-w64-mingw32-gcc (nix develop .#win); SKIP without it. No game data.
#   nix develop .#win --command tools/sandbox.sh -- tools/spu/test_spu_rm_layout.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t_rm_layout"; mkdir -p "$O"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
H="$P/runtime/spu/spu_context.h"; C="$P/runtime/spu/spu_channels.c"
[[ -f "$H" && -f "$C" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "SKIP: no x86_64-w64-mingw32-gcc (nix develop .#win)"; exit 0; }
fail=0
MAC="$(sed -n '/^#if defined(_WIN32) && defined(__GNUC__) && !defined(SPU_NO_READ_MOSTLY)$/,/^#endif$/p' "$H")"
FEN="$(sed -n '/^#if defined(_WIN32) && defined(__GNUC__) && !defined(SPU_NO_READ_MOSTLY)$/,/^#endif$/p' "$C")"
[[ "$MAC" == *'.data$kz3rm'* ]] || { echo "FAIL: SPU_READ_MOSTLY not found in $H"; exit 1; }
[[ "$FEN" == *'.data$kz3rl'* && "$FEN" == *'.data$kz3rn'* ]] || { echo "FAIL: fences not found in $C"; exit 1; }
for v in g_spu_stack_reset_n g_kz3_trace_ctx g_kz3_pmend_ib g_spu_ovl_skip_any g_spu_ls_watch_n g_spu_task_pol_big; do
  grep -qE "\b$v\b(\[[^]]*\])? SPU_READ_MOSTLY" "$C" || { echo "FAIL: $v is not SPU_READ_MOSTLY in spu_channels.c"; fail=1; }
done
for v in g_spu_hop_on g_spu_ib_guess g_spu_launch_diag g_pm_flow_ctx g_kz3_spu_xr g_kz3_spu_xrq g_spu_depth_max; do
  grep -qE "\b$v SPU_READ_MOSTLY" "$P/runtime/spu/spu_drain.c" || { echo "FAIL: $v is not SPU_READ_MOSTLY in spu_drain.c"; fail=1; }
done
for i in 0 1 2; do
  cat > "$O/t$i.c" <<CEOF
#include <stdint.h>
$MAC
volatile unsigned hot_a$i = 1;            /* written counters around the gates, as in the runtime */
volatile unsigned hot_b$i;
int gate_x$i SPU_READ_MOSTLY = -1;
volatile unsigned hot_c$i = 2;
uint64_t gate_bits${i}[16] SPU_READ_MOSTLY;
void* volatile gate_p$i SPU_READ_MOSTLY = 0;
volatile unsigned hot_d$i;
__attribute__((weak)) SPU_READ_MOSTLY int gate_w = -1;
$( [[ $i = 1 ]] && echo "$FEN" )
int use$i(void) { return gate_x$i + (int)gate_bits${i}[3] + (gate_p$i != 0) + gate_w + (int)hot_a$i + (int)hot_b$i + (int)hot_c$i + (int)hot_d$i; }
CEOF
done
cat > "$O/main.c" <<'CEOF'
int use0(void); int use1(void); int use2(void);
int main(void) { return use0() + use1() + use2() == 12345; }
CEOF
x86_64-w64-mingw32-gcc -O2 -o "$O/t.exe" "$O/t0.c" "$O/t1.c" "$O/t2.c" "$O/main.c" || { echo "FAIL: build"; exit 1; }
x86_64-w64-mingw32-nm -n -S "$O/t.exe" > "$O/nm.txt"
python3 - "$O/nm.txt" <<'PY' || fail=1
import sys
syms = []
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 4 and p[2] in 'bBdD':
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in 'bBdD':
        syms.append((int(p[0], 16), 4, p[2]))
d = {n: (a, s) for a, s, n in syms}
lo, hi = d['g_spu_rm_fence_lo'], d['g_spu_rm_fence_hi']
assert lo[0] % 64 == 0 and hi[0] % 64 == 0, "fences not 64-byte aligned"
bad = 0
gates = [(a, s, n) for a, s, n in syms if n.startswith(('gate_', '.weak.gate_'))]
first, last = lo[0] + 64, hi[0]
for a, s, n in gates:
    if not (first <= a and a + s <= last) and not n.startswith('.weak.'):
        print(f"FAIL: {n} at {a:#x} outside the fences [{first:#x}, {last:#x})"); bad += 1
lines = {a // 64 for a, s, n in gates for a in range(a, a + max(s, 1))}
for a, s, n in syms:
    if n.startswith(('gate_', '.weak.gate_', 'g_spu_rm_fence')) or n.startswith('.'):
        continue
    if any(x // 64 in lines for x in range(a, a + max(s, 1))):
        print(f"FAIL: {n} at {a:#x} shares a cache line with the gates"); bad += 1
hots = [n for _, _, n in syms if n.startswith('hot_')]
print(f"{len(gates)} gate symbols in {len(lines)} line(s) between {lo[0]:#x} and {hi[0]:#x}; {len(hots)} counters elsewhere")
sys.exit(1 if bad or len(hots) < 12 else 0)
PY
[[ $fail = 0 ]] && echo "PASS test_spu_rm_layout" || { echo "FAIL test_spu_rm_layout"; exit 1; }
