/* Stub for host tests without a PPU lift (public CI): the generic preamble that ps3recomp's ppu_lifter.py writes
 * at the top of every ppu_recomp.h (MIT), up to the function table, without the game's function declarations.
 * Copied from a generated header on 2026-10-12; regenerate if the lifter's preamble changes. */
#pragma once
#include <stdint.h>

/* Guest memory barriers: the lowering of sync/lwsync/eieio/isync/ptesync.
 *
 * These used to be no-ops, which x86-64 forgave -- its store order is total,
 * so the ordering a guest asked for was already there. A weakly ordered host
 * does not give it away. The gcm command buffer is the shape that breaks
 * first: the SDK writes its command words, separates them with eieio or
 * lwsync, then writes the FIFO put pointer, and with the barrier dropped the
 * pointer can become visible before the words it publishes. Every lock-free
 * handoff in a title has the same exposure.
 *
 * The macro lives in the header so the lifted code, the boot scaffold and the
 * smoke title all spell a fence the same way. On x86-64 the acquire, release
 * and acq_rel forms emit no instruction at all and cost only the reordering
 * the compiler no longer does; only seq_cst becomes a barrier instruction
 * (mfence, or the locked op LLVM prefers to it). On arm64 they are one dmb
 * each, around half a nanosecond. */
#ifndef PPU_FENCE
#  ifdef __cplusplus
#    include <atomic>
#    define PPU_FENCE(o) std::atomic_thread_fence(std::memory_order_##o)
#  else
#    include <stdatomic.h>
#    define PPU_FENCE(o) atomic_thread_fence(memory_order_##o)
#  endif
#endif

/* 128-bit vector register for VMX/AltiVec */
typedef union {
    uint8_t  b[16];
    uint16_t h[8];
    uint32_t w[4];
    uint64_t d[2];
    float    f[4];
} ppu_vr;

/* Field order and types match runtime/ppu/ppu_context.h exactly, so the
 * same pointer can flow between recompiled code and runtime syscall
 * handlers without layout drift. Keep the two in sync. */
typedef struct ppu_context {
    uint64_t gpr[32];   /* General-purpose registers  */
    double   fpr[32];   /* Floating-point registers   */
#ifdef _MSC_VER
    __declspec(align(16)) ppu_vr vr[32];  /* VMX/AltiVec vector regs */
#else
    ppu_vr   vr[32] __attribute__((aligned(16)));
#endif
    uint32_t cr;        /* Condition register          */
    uint64_t lr;        /* Link register               */
    uint64_t ctr;       /* Count register              */
    uint32_t xer;       /* Fixed-point exception reg   */
    uint32_t fpscr;     /* FP status/control register  */
    uint32_t vscr;      /* Vector status/control reg   */
    uint64_t cia;       /* Current instruction address */
    uint64_t thread_id;     /* PPU thread ID              */
    uint64_t reserve_addr;  /* lwarx/stwcx. reservation address */
    uint64_t reserve_value; /* lwarx/stwcx. reservation value   */
    int      reserve_valid; /* lwarx/stwcx. reservation flag    */
} ppu_context;

/* Memory access helpers (provided by runtime) */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t  vm_read8 (uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
void     vm_write8 (uint64_t addr, uint8_t  val);
void     vm_write16(uint64_t addr, uint16_t val);
void     vm_write32(uint64_t addr, uint32_t val);
void     vm_write64(uint64_t addr, uint64_t val);
/* stwcx./stdcx. store-conditional: atomically store `val` at `addr` iff the raw
 * big-endian guest word still equals `expected` (the value the paired lwarx/ldarx
 * loaded). Returns 1 if the reservation held (store applied), 0 otherwise. This
 * makes lock-free sequences (free-list CAS, refcounts) correct across host
 * threads -- a plain conditional write races and corrupts under real concurrency. */
int      ppu_stwcx32(uint64_t addr, uint32_t expected, uint32_t val);
int      ppu_stdcx64(uint64_t addr, uint64_t expected, uint64_t val);
#ifdef __cplusplus
}
#endif

/* Syscall handler */
#ifdef __cplusplus
extern "C"
#endif
void lv2_syscall(ppu_context* ctx);

/* Thread-local storage qualifier. The runtime's boot scaffold declares the
 * same objects the generated code does (g_active_ctx, g_trampoline_fn), so
 * this has to live in the HEADER both sides include -- not in the generated
 * source, where only the lifted code could see it. */
#ifndef PPU_THREAD_LOCAL
#  ifdef _MSC_VER
#    define PPU_THREAD_LOCAL __declspec(thread)
#  else
#    define PPU_THREAD_LOCAL __thread
#  endif
#endif

/* Function table (defined in the generated source, sentinel-terminated).
 * The game project's indirect-call dispatcher builds its lookup from this. */
typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } func_entry;
#ifdef __cplusplus
extern "C" {
#endif
extern const func_entry function_table[];
extern const uint64_t  function_table_count;  /* entries, excluding sentinel */
#ifdef __cplusplus
}
#endif

