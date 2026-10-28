#!/usr/bin/env bash
# Taskset layout host test (patch 1092's until v0.15.0 absorbed it; now checks upstream's CreateTaskset writes plus
# CreateTask's slot and bits, upstream #388 since v0.17.0, was patch 1073): the runtime writes the guest
# CellSpursTaskset only through its big-endian layout.
# 1. Static guard: no native `taskset->field` access left in the patched cellSpurs.c (the native little-endian
#    initialized/taskCount/shutdownRequested/spurs fields sat on the BE running (+0x00) and ready (+0x10) bitsets).
# 2. A CreateTaskset + 40 x CreateTask sequence on a garbage-filled guest buffer, compiled from the patched sources:
#    spurs_create_taskset's clear + spurs_taskset_init call (CreateTaskset's guest writes), CreateTask's spurs_taskset_take_slot +
#    spurs_taskset_add_task (#388) and libs/spurs/spurs_taskset.c (spurs_taskset_init, task_info), over a stub BE memory header.
#    Required: running bitset all zero, enabled and ready = exactly tasks 0-39 (task 0 = MSB of byte 0), the other
#    bitsets zero, spurs/args/size BE, unused task_info zero, nothing written past 0x1900. A negative control adds the
#    pre-1092 native taskCount++ and must fail.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_ts_be_layout.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; O="$ROOT/work/spu/t_ts_be"; mkdir -p "$O/libs/spurs" "$O/runtime/ppu"
P="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
F="$P/libs/spurs/cellSpurs.c"
[[ -f "$F" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
fail=0
native="$(grep -nE '\btaskset->[a-zA-Z_]+' "$F" | grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)')"
if [[ -n "$native" ]]; then echo "FAIL: native CellSpursTaskset field access in $F:"; echo "$native" | head -5; fail=1
else echo "ok: no native taskset->field access in cellSpurs.c"; fi
# Since v0.15.0 CreateTaskset's guest writes are upstream's (PR #240, #283/#284 absorbed 1092): spurs_create_taskset
# clears the whole taskset, then spurs_taskset_init writes the BE header. Those statements, compiled as written.
FN="$(sed -n '/^static s32 spurs_create_taskset(/,/^}/p' "$F")"
MS="$(grep -m1 'memset(taskset, 0, taskset_size);' <<<"$FN")"
IN="$(awk '/spurs_taskset_init\(taskset_ea/ {on = 1} on {print} on && /\);/ {exit}' <<<"$FN")"
BE="static void kz3_ts_be_create(uint32_t taskset_ea, uint32_t spurs_ea, uint64_t args)
{
    uint32_t taskset_size = CSTS_STRUCT_END; void* taskset = vm_base + taskset_ea;
$MS
$IN
}"
grep -q 'spurs_taskset_take_slot(taskset_ea' "$F" ||
  { echo "FAIL: cellSpursCreateTask does not call spurs_taskset_take_slot (upstream #388) in $F"; exit 1; }
[[ -n "$MS" && -n "$IN" ]] ||
  { echo "FAIL: CreateTaskset's clear/init (upstream) missing in $F"; exit 1; }
cp "$P/libs/spurs/spurs_taskset.h" "$P/libs/spurs/spurs_taskset.c" "$O/libs/spurs/"
cat > "$O/runtime/ppu/ppu_memory.h" <<'H'
/* test stub: guest memory = one host buffer, big-endian accessors */
#pragma once
#include <stdint.h>
#include <string.h>
extern uint8_t* vm_base;
static inline uint8_t  vm_read8(uint32_t a)  { return vm_base[a]; }
static inline uint32_t vm_read32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static inline uint64_t vm_read64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static inline void vm_write32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(vm_base + a, &v, 4); }
static inline void vm_write64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(vm_base + a, &v, 8); }
H
cat > "$O/t.c" <<C
#include "libs/spurs/spurs_taskset.h"
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint8_t u8;
#define CELL_SPURS_MAX_TASK 128u
static uint8_t g_mem[0x10000]; uint8_t* vm_base = g_mem;
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
$BE
#define TS 0x1000u
static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); bad++; } } while (0)
int main(void) {
    memset(g_mem, 0xAA, sizeof g_mem);                       /* a reused heap block */
    kz3_ts_be_create(TS, 0x00ABC000u, 0x0123456789ABCDEFull); /* cellSpursCreateTaskset */
    for (u32 n = 0; n < 40; n++) {                            /* cellSpursCreateTask x 40 */
        u32 id = spurs_taskset_take_slot(TS, CELL_SPURS_MAX_TASK);
        CHECK(id == n, "create %u got id %u", n, id);
        uint32_t arg[4] = {n, 1, 2, 3}, lsp[4] = {0, 0, 0, 0};
        spurs_taskset_add_task(TS, id, 0x00F3D300u, 0, arg, lsp);   /* task_info + enabled + ready */
#ifdef OLD_NATIVE_COUNT   /* negative control: the pre-1092 native taskCount++ at +0x04 */
        { uint32_t c; memcpy(&c, vm_base + TS + 4, 4); c++; memcpy(vm_base + TS + 4, &c, 4); }
#endif
    }
    for (u32 t = 0; t < 128; t++) {
        CHECK(!spurs_bitset_test(TS + CSTS_RUNNING, t), "task %u reads as running", t);
        CHECK(spurs_bitset_test(TS + CSTS_ENABLED, t) == (t < 40), "task %u enabled bit wrong", t);
        CHECK(spurs_bitset_test(TS + CSTS_READY, t) == (t < 40), "task %u ready bit wrong", t);
        CHECK(!spurs_bitset_test(TS + CSTS_PENDING_READY, t) && !spurs_bitset_test(TS + CSTS_SIGNALLED, t)
              && !spurs_bitset_test(TS + CSTS_WAITING, t), "task %u pending/signalled/waiting set", t);
    }
    CHECK(vm_read32(TS + CSTS_ENABLED) == 0xFFFFFFFFu && vm_read32(TS + CSTS_ENABLED + 4) == 0xFF000000u,
          "enabled words %08X %08X", vm_read32(TS + CSTS_ENABLED), vm_read32(TS + CSTS_ENABLED + 4));
    CHECK(vm_read64(TS + CSTS_SPURS) == 0x00ABC000u, "spurs field");
    CHECK(vm_read64(TS + CSTS_ARGS) == 0x0123456789ABCDEFull, "args field");
    CHECK(vm_read32(TS + CSTS_SIZE_FIELD) == 0x1900u, "size field %08X", vm_read32(TS + CSTS_SIZE_FIELD));
    CHECK(vm_read32(TS + 0x70) == 0, "+0x70 (enable_clear_ls / wkl_flag_wait / last_scheduled) %08X", vm_read32(TS + 0x70));
    for (u32 t = 40; t < 128; t++)
        for (u32 o = 0; o < TI_SIZE; o += 8)
            CHECK(vm_read64(spurs_taskset_taskinfo_ea(TS, t) + o) == 0, "task_info[%u]+%u not cleared", t, o);
    CHECK(g_mem[TS - 1] == 0xAA && g_mem[TS + 0x1900] == 0xAA, "write outside the 0x1900-byte taskset");
    printf("%s: CreateTaskset + 40 CreateTask, %d check(s) failed\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
C
CC="${CC:-cc}"
"$CC" -O1 -std=gnu11 -Wall -Wno-unused-function -I"$O" "$O/t.c" "$O/libs/spurs/spurs_taskset.c" -o "$O/t" || { echo "FAIL: compile"; exit 1; }
"$O/t" || fail=1
"$CC" -O1 -std=gnu11 -DOLD_NATIVE_COUNT -I"$O" "$O/t.c" "$O/libs/spurs/spurs_taskset.c" -o "$O/t_old" 2>/dev/null \
  || { echo "FAIL: compile (control)"; exit 1; }
if "$O/t_old" > "$O/t_old.out"; then echo "FAIL: negative control (native taskCount++) not detected"; fail=1
else echo "ok: negative control (native taskCount++ at +0x04) detected: $(grep -m1 'reads as running' "$O/t_old.out")"; fi
[[ $fail -eq 0 ]] && echo "PASS: test_ts_be_layout" || { echo "FAIL: test_ts_be_layout"; exit 1; }
