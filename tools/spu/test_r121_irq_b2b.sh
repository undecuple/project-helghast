#!/usr/bin/env bash
# Pass 121, runtime patch 1142: an SPU interrupt taken at the resume point of a handler whose iret was not completed
# yet (pc == srr0, interrupts on, registers not restored) must first restore the interrupted code's registers, so the
# second handler's iret hands those back (not the first handler's). Self-contained: patched toolkit only.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -f $T/runtime/spu/spu_drain.c ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q g_spu_irq_b2b "$T/runtime/spu/spu_drain.c" || { echo "FAIL: $T has no patch 1142"; exit 1; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/main.c" <<'C'
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
extern volatile unsigned long g_spu_irq_b2b;
int spu_irq_regs_maybe_restore(spu_context* ctx);
static void SPU_ABI handler(spu_context* c) { (void)c; }
int main(void)
{
    static spu_context c;
    spu_context_init(&c, 0);
    c.ls[0] = 0x30; c.ls[1] = 0x00; c.ls[2] = 0x10; c.ls[3] = 0x00;   /* LS 0: bra 0x80 (the handler vector) */
    for (int r = 0; r < 128; r++) c.gpr[r] = spu_splat_u32(0x1000u + (uint32_t)r);   /* the interrupted code */
    c.pc = 0x4770; c.int_enable = 1; c.event_mask = 2; c.event_status = 2; c.host_depth = 0;
    spu_take_interrupt(&c, handler);                                   /* first take */
    if (!c.irq_saved || c.int_enable || (c.pc & SPU_LS_MASK) != 0x80) { puts("first take not taken"); return 1; }
    for (int r = 0; r < 128; r++) c.gpr[r] = spu_splat_u32(0xBAD00000u + (uint32_t)r);  /* the handler's registers */
    c.pc = 0x4770; c.int_enable = 1;                                   /* its iret: back at srr0, enabled */
    spu_take_interrupt(&c, handler);                                   /* the next event, before any dispatch */
    for (int r = 0; r < 128; r++) c.gpr[r] = spu_splat_u32(0xBEEF0000u + (uint32_t)r);
    c.pc = 0x4770; c.int_enable = 1;                                   /* the second handler's iret */
    if (!spu_irq_regs_maybe_restore(&c)) { puts("second iret did not restore"); return 1; }
    for (int r = 0; r < 128; r++)
        if (c.gpr[r]._u32[0] != 0x1000u + (uint32_t)r) {
            printf("r%d = 0x%08X after the second iret (want 0x%08X)\n", r, c.gpr[r]._u32[0], 0x1000u + (uint32_t)r);
            return 1;
        }
    printf("registers of the interrupted code back after two takes (b2b=%lu)\n", (unsigned long)g_spu_irq_b2b);
    return g_spu_irq_b2b == 1 ? 0 : 1;
}
C
INC=(-I "$T/runtime/spu" -I "$T/include" -I "$T/runtime")
${CC:-cc} -std=gnu17 -O1 -w -fno-strict-aliasing -msse4.1 -mfma "${INC[@]}" -o "$OUT/t" "$OUT/main.c" \
    "$T/runtime/spu/spu_drain.c" "$T/runtime/spu/spu_interp.c" -lm >"$OUT/b.log" 2>&1 ||
  { echo "FAIL build: $(grep -m2 -E 'error|undefined' "$OUT/b.log" | cut -c1-200)"; exit 1; }
if r="$("$OUT/t")"; then echo "PASS $r"; echo "== test_r121_irq_b2b: PASS"; else echo "FAIL $r"; echo "== test_r121_irq_b2b: FAIL"; exit 1; fi
