/* kz3_native_jobs.h -- native host versions of hot SPU job code (SPU pass 122, D88-). The per-function gate follows
 * KZ2's (Killzone 2 port, spu rounds 15-16: re/spu/native_gates.tsv, native_common.c, kz2_native_jobs.c); credit to
 * that project for the design: a check at the top of a lifted SPU function (inserted by tools/spu/lift_spu.py from
 * re/spu/native_gates.tsv). When g_kz3_native_gate[slot] is set and returns 1, the native code did the whole
 * subroutine (its local-store writes and the registers its caller reads) and the lifted function returns to its link
 * as the SPU's `bi $r0` would. Returning 0 runs the lifted body (unexpected arguments, a debug LS gate armed, the
 * module's constant pool not what the native code was written for).
 *
 * Default off: a gate is filled only when one of its entry's flags is 1 (`env` lists KZ3_NATIVE_JOB_<NAME>
 * alternatives separated by '|'); KZ3_NATIVE_JOB_POST=1 fills every gate of the Gfx.Post jobs. The gate array is
 * defined once, in kz3_native_jobs.c. KZ3_NATIVE_REC=DIR records real gate inputs instead (see kz3_native_jobs.c);
 * tools/spu/native_diff.py replays records and synthetic inputs through lifted and native code and compares.
 */
#ifndef KZ3_NATIVE_JOBS_H
#define KZ3_NATIVE_JOBS_H

#include <stdint.h>
#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*kz3_native_fn)(spu_context*);

typedef struct {
    int slot;              /* g_kz3_native_gate index (re/spu/native_gates.tsv) */
    const char* env;       /* KZ3_NATIVE_JOB_<NAME>, alternatives separated by '|' */
    const char* job;       /* census name */
    const char* image;     /* lifted image name as in native_gates.tsv (1.14: job_<offset>; the 1.00 twin has its own row) */
    uint32_t addr;         /* LS address of the gated function */
    kz3_native_fn fn;
    const char* kind;      /* kernel family (the diff harness picks its synthetic inputs by it) */
    uint32_t pool_lo, pool_hi;   /* constant pool the kernel was written for: LS [lo, hi), 8-byte aligned */
    uint64_t pool_hash;          /* FNV-1a 64 over its little-endian 8-byte words (tools/spu/native_pool.py) */
    uint8_t def_on;              /* 1: native unless a switch says =0 (tsv column "default", SPU pass 127); 0: opt-in */
} kz3_native_entry;

extern const kz3_native_entry kz3_native_post[];
extern const unsigned kz3_native_post_n;
extern const kz3_native_entry kz3_native_dof[];
extern const unsigned kz3_native_dof_n;
extern const kz3_native_entry kz3_native_lp[];
extern const unsigned kz3_native_lp_n;

const kz3_native_entry* kz3_native_find(int slot);
int kz3_native_run(spu_context* ctx, const kz3_native_entry* e);

/* Record file layout (host little-endian): magic "KZ3NJ001", u32 slot, u32 addr, s32 image_id, u32 0, then 128 x 16
 * bytes of registers (the runtime's u128 layout), then the 256 KB local store. */
#define KZ3_NJ_MAGIC "KZ3NJ001"

extern volatile long g_kz3_native_calls[64], g_kz3_native_fallback[64];
void kz3_native_count_print(int slot, long calls);
void kz3_native_win_hit(int slot);   /* per-window call counter (10 s windows since the process start, SPU pass 125) */
static inline void kz3_native_count(int slot)
{
    const long c = __atomic_add_fetch(&g_kz3_native_calls[slot], 1, __ATOMIC_RELAXED);
    kz3_native_win_hit(slot);
    if (__builtin_expect(c >= 1024 && !(c & (c - 1)), 0)) kz3_native_count_print(slot, c);
}

#ifdef __cplusplus
}
#endif
#endif
