/* kz3_fiber_stall: classification and decision sequence of the PPU fiber stall breaker (SPU pass 34).
 *
 * Facts (the RE notes "The level-stall owners are DeferredSchedule arenas", "B4 ... the 1.14 arena table"):
 * the stalled fibers of rsx r32c / spu-31a / gp-t2 wait on `arena + 0x10C` of a Gfx.PB.DeferredSchedule arena
 * (8 static slots at 0x01526800 / 1.14 0x0159BF00, stride 0xBCA0, or a heap arena) for the SPU job the fiber
 * itself submitted on WWS queue 0. The value (1) is stored by the job module 0x011D8080 with a GETLLAR/PUTLLC
 * loop (LS 0xD838-0xD86C). Both wait sites are known by their call lr: site A 0x002E4038 / 1.14 0x002EF9C8
 * (func_002E3FE0 / 0x2EF970), site B 0x002E41A4 / 0x002EFB34 (func_002E4050 / 0x2EF9E0).
 *
 * Forcing such a fiber on (the round-32 breaker) leaves the arena at +0x10C = 0 and the holder in state 2; the
 * next release spins the main thread in func_002E1BE0 forever. So a DeferredSchedule wait is never force-resumed:
 * the breaker reports, re-kicks the job manager workload after KZ3_FIBER_DEADLOCK seconds (loses nothing), and
 * after another period writes the completion value the job would have written (loses one push-buffer section).
 *
 * Pure C (no runtime); host test tools/spu/test_kz3_fiber_stall.sh. Both builds' constants are accepted at once
 * (they do not overlap), so no version switch is needed. */
#ifndef KZ3_FIBER_STALL_H
#define KZ3_FIBER_STALL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define KZ3_DS_ARENA_BASE_100 0x01526800u
#define KZ3_DS_ARENA_BASE_114 0x0159BF00u
#define KZ3_DS_ARENA_STRIDE 0xBCA0u
#define KZ3_DS_ARENA_SLOTS 8u
#define KZ3_DS_ARENA_FLAG 0x10Cu
#define KZ3_DS_WAIT_LR_A_100 0x002E4038u
#define KZ3_DS_WAIT_LR_B_100 0x002E41A4u
#define KZ3_DS_WAIT_LR_A_114 0x002EF9C8u
#define KZ3_DS_WAIT_LR_B_114 0x002EFB34u
/* The 3-argument wait stub wait(S, ptr, value) that calls the 2-argument wait: v1.00 func_000C383C (0x28 bytes,
 * calls at lr 0x000C3854), 1.14 0x000C57DC. Inside it the site's lr sits at r1 + 0x80 of its frame. */
#define KZ3_DS_WAIT_STUB_100 0x000C383Cu
#define KZ3_DS_WAIT_STUB_114 0x000C57DCu
#define KZ3_DS_WAIT_STUB_LEN 0x28u
#define KZ3_DS_WAIT_STUB_LR_SLOT 0x80u
/* The job module that completes the arena (Gfx.PB.DeferredSchedule, EA 0x011D8080, 82,400 B) and its PUTLLC
 * retry loop (LS addresses). */
#define KZ3_DS_MODULE_EA 0x011D8080u
#define KZ3_DS_PUTLLC_LO 0xD838u
#define KZ3_DS_PUTLLC_HI 0xD86Cu
/* WWS job queue table (v1.00 0x145C4B0 + q * 0x40: +4 job list EA, +8 SPURS instance EA, +0xC workload id,
 * +0x3C lock; from func_000892C4 / func_0008978C). ASSUMPTION: the 1.14 twin is not located; KZ3_DS_QUEUE_TABLE
 * (hex) overrides it. */
#define KZ3_DS_QUEUE_TABLE_100 0x0145C4B0u
#define KZ3_DS_QUEUE_TABLE_114 0x014D1B60u /* RE pass 71 */
#define KZ3_DS_QUEUE_STRIDE 0x40u
/* The WWS job manager policy module (jm image EA 0x012BF080, LS base 0xA00) and its wait loops (RE pass 71):
 *   1a  the dispatcher spinning on an unsatisfied dependency counter, no kernel call: 0x2C30-0x2D0C, 0x3040-0x30B0,
 *       0x31D4-0x31DC, the take-entry helper 0x23C8-0x2580, 0x22E0-0x230C;
 *   1ap the same spin with a selectWorkload poll: 0x2D40-0x2DBC, 0x3678-0x36D8 (polls rise);
 *   1c  PUTLLC retry loops: 0x23DC-0x24D4, 0x2598-0x25F4, 0x34E8-0x353C, 0x3800-0x385C, 0x38A4-0x38F8 (atomic_stat & 1);
 *   1b  LS >= 0x3980: job code (a job's own loop). */
#define KZ3_JM_IMAGE_EA 0x012BF080u
enum kz3_jm_pc_class {
    KZ3_JM_PC_OTHER = 0,
    KZ3_JM_PC_1A = 1,
    KZ3_JM_PC_1A_POLL = 2,
    KZ3_JM_PC_1C = 3,
    KZ3_JM_PC_1B = 4
};
int kz3_jm_pc_class(uint32_t pc);
const char* kz3_jm_pc_class_name(int cls);

enum kz3_stall_class {
    KZ3_STALL_OTHER = 0,     /* not a DeferredSchedule wait: the old breaker (force-resume) applies */
    KZ3_STALL_DS_STATIC = 1, /* a static pool arena's +0x10C (slot in *slot) */
    KZ3_STALL_DS_LR = 2      /* a heap arena, known by the wait site's lr */
};
/* Classify one stalled wait: w = watched word, lr = the wait site's lr (0 if unknown). *slot (optional) gets
 * the static slot index or -1. */
int kz3_ds_classify(uint32_t w, uint32_t lr, int* slot);
/* 1 when lr is inside the 3-argument wait stub (then the site lr is the 64-bit word at r1 + 0x80). */
int kz3_ds_lr_in_wait_stub(uint32_t lr);
/* 1 when an SPU running image `image_id` (the resident overlay) at LS pc sits in the module's PUTLLC retry loop;
 * ds_image_id is the runtime image id of module 0x011D8080 (0 = unknown, never matches). */
int kz3_ds_pc_in_putllc_loop(int image_id, int ds_image_id, uint32_t pc);

/* Decision sequence for one stall (period = KZ3_FIBER_DEADLOCK seconds, 0 = report only):
 *   reports at t0 + 5 s and every 10 s after;
 *   KZ3_DS_REKICK once at t0 + period;
 *   KZ3_DS_COMPLETE once at t0 + 2 * period;
 *   then reports only (the completion write ends the wait through the scheduler's own check; if the fiber waits
 *   again on a new job, that is a new stall and the sequence restarts).
 * step() returns the actions due now as a bit mask; a report is always included with an action. */
#define KZ3_DS_REPORT 1
#define KZ3_DS_REKICK 2
#define KZ3_DS_COMPLETE 4
struct kz3_ds_breaker {
    double t0, next_report;
    int stage; /* 0 waiting for the re-kick, 1 waiting for the completion write, 2 done */
};
void kz3_ds_breaker_reset(struct kz3_ds_breaker* b, double now);
int kz3_ds_breaker_step(struct kz3_ds_breaker* b, double now, double period);

#ifdef __cplusplus
}
#endif
#endif
