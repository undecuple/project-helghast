#!/usr/bin/env bash
# Pass 121: tools/spu/leaf_native.py (register-promoted native copy of a lifted leaf group) on a synthetic lift
# (r121_synth_lift.c): the generated entry must leave registers, pc and LS bit-identical to the lifted functions on
# random inputs, and a function with a channel op must be refused. Self-contained: patched toolkit only, no game data.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_helpers.h ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
S="$ROOT/tools/spu"; fail=0
python3 "$S/leaf_native.py" "$S/r121_synth_lift.c" --group 0x4000,0x4100 --entries 0x4000 -o "$OUT/leaf.c" >/dev/null ||
  { echo "FAIL generate"; exit 1; }
sed -i 's/#include "spu_recomp.h"/#include "spu_context.h"/' "$OUT/leaf.c"
cat > "$OUT/main.c" <<'C'
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
SPU_ABI void synth_spu_func_00004000(spu_context* ctx);
SPU_ABI void synth_leaf_04000(spu_context* ctx);
SPU_ABI void spu_indirect_branch(spu_context* c) { (void)c; }
/* runtime globals and hooks the lifted-code macros and spu_drain.c / spu_interp.c reach (as job_replay.c stubs them) */
__attribute__((weak)) volatile unsigned g_spu_stack_reset_n;
__attribute__((weak)) volatile int g_yz_lockstep_on;
unsigned g_spu_stack_reset_count;
int g_spu_ls_watch_n = 0;
int g_kz3_jobcost = 0;
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t) = 0;
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)a; (void)w; (void)p; (void)pc; (void)lr; }
void spu_jmp_no_unwind(void* env) { (void)env; }
void yz_lockstep_tick(spu_context* c) { (void)c; }
void spu_check_stack_reset(spu_context* c, void (*f)(spu_context*)) { (void)c; (void)f; }
void kz3_jobcall_close(spu_context* c, uint32_t pc) { (void)c; (void)pc; }
void kz3_jobcost_init_once(void) {}
int kz3_wws_interp_on(void) { return 0; }
void spu_halt(spu_context* c) { (void)c; abort(); }
void spu_restart_dispatch(spu_context* c) { (void)c; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
int spu_overlay_match_sig(const uint8_t h[16]) { (void)h; return 0; }
void (*spu_lookup(uint32_t a, int img))(spu_context*) { (void)a; (void)img; return 0; }
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }
spu_lifted_fn spu_lookup_located(const spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }
spu_lifted_fn spu_lookup_ovl_exact(uint32_t a, int img) { (void)a; (void)img; return 0; }
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
static void seed(spu_context* c, unsigned s)
{
    srand(s);
    for (unsigned i = 0; i < SPU_LS_SIZE; i++) c->ls[i] = (uint8_t)rand();
    for (int r = 0; r < 128; r++) for (int k = 0; k < 4; k++) c->gpr[r]._u32[k] = (uint32_t)rand() * 2654435761u;
    for (int k = 0; k < 4; k++) {   /* plausible floats for the arithmetic, a byte-shuffle control, a loop count */
        c->gpr[5]._f32[k] = 0.5f + (float)(rand() % 1000) / 7.0f;
        c->gpr[6]._f32[k] = (float)(rand() % 1000) / 3.0f;
        c->gpr[11]._u32[k] = 0x03020100u + 0x04040404u * (uint32_t)((k + 1) & 3);
    }
    c->gpr[3] = spu_splat_u32(0x1000); c->gpr[4] = spu_splat_u32(0x2000);
    c->gpr[9] = spu_splat_u32(1 + rand() % 200); c->gpr[0] = spu_splat_u32(0x3F00);
    c->host_depth = 1; c->pc = 0x4000;
}
int main(void)
{
    static spu_context a, b;
    spu_context_init(&a, 0); spu_context_init(&b, 0);
    int bad = 0;
    for (unsigned s = 1; s <= 50; s++) {
        seed(&a, s); seed(&b, s);
        synth_spu_func_00004000(&a);
        while (a.tramp_fn) { void (*f)(spu_context*) = a.tramp_fn; a.tramp_fn = 0; f(&a); }
        synth_leaf_04000(&b);
        while (b.tramp_fn) { void (*f)(spu_context*) = b.tramp_fn; b.tramp_fn = 0; f(&b); }
        if (memcmp(a.gpr, b.gpr, sizeof a.gpr) || memcmp(a.ls, b.ls, SPU_LS_SIZE) || a.pc != b.pc) bad++;
    }
    printf("%d of 50 seeds differ\n", bad);
    return bad != 0;
}
C
INC=(-I "$T/runtime/spu" -I "$T/include" -I "$T/runtime")
FL=(-std=gnu17 -O2 -msse4.1 -mfma -DKZ3_SPU_NEEDS_FMA -w -ffp-contract=off -fno-strict-aliasing)
CC="${CC:-cc}"
$CC "${FL[@]}" "${INC[@]}" -o "$OUT/t" "$OUT/main.c" "$S/r121_synth_lift.c" "$OUT/leaf.c" \
    "$T/runtime/spu/spu_drain.c" "$T/runtime/spu/spu_interp.c" -lm >"$OUT/b.log" 2>&1 || { echo "FAIL build: $(grep -m2 -E 'error|undefined' "$OUT/b.log" | cut -c1-200)"; exit 1; }
r="$("$OUT/t")" && echo "PASS leaf copy bit-identical: $r" || { echo "FAIL leaf copy: $r"; fail=1; }
sed 's/ctx->gpr\[26\] = spu_xsbh(ctx->gpr\[25\]);/ctx->gpr[26] = spu_rdch(ctx, 8);/' "$S/r121_synth_lift.c" > "$OUT/notleaf.c"
if python3 "$S/leaf_native.py" "$OUT/notleaf.c" --group 0x4000,0x4100 -o "$OUT/x.c" >/dev/null 2>&1; then
  echo "FAIL a channel op was accepted"; fail=1; else echo "PASS a function with a channel op is refused"; fi
[[ $fail == 0 ]] && echo "== test_r121_leaf: PASS" || { echo "== test_r121_leaf: FAIL"; exit 1; }
