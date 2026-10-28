/* kz3_fiber_stall_rt.c: the runtime-side readers of the fiber stall report (SPU pass 34): per-SPU policy-module
 * context (pc, resident overlay = the current job module) through spu_pm_ctx_peek (patch 1011) and the lifted
 * image name table (gen/kz3_spu_register.c). Not host-tested (needs spu_context); the pure logic is in
 * kz3_fiber_stall.c. */
#include <stdint.h>
#include <string.h>
#include "spu_context.h"
#include "kz3_spu_skip.h"
#include "kz3_fiber_stall.h"

extern void* spu_pm_ctx_peek(uint32_t wid, uint32_t spu_num);

int kz3_ds_module_image_id(void)
{
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (kz3_spu_image_names[i].ea == KZ3_DS_MODULE_EA) return kz3_spu_image_names[i].image_id;
    return 0;
}

const char* kz3_spu_image_name_of(int image_id)
{
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (kz3_spu_image_names[i].image_id == image_id) return kz3_spu_image_names[i].name;
    return "?";
}

/* One virtual SPU of workload wid: 0 when it has no context (never dispatched). pc = LS address, ovl = the
 * resident overlay image id (the job module now loaded, 0 = none), in_putllc = sitting in the DeferredSchedule
 * module's completion-store retry loop. */
int kz3_ds_spu_state(uint32_t wid, uint32_t spu_num, uint32_t* pc, int* ovl, int* policy, int* in_putllc)
{
    spu_context* c = (spu_context*)spu_pm_ctx_peek(wid, spu_num);
    if (!c) return 0;
    uint32_t p = c->pc & SPU_LS_MASK;
    if (pc) *pc = p;
    if (ovl) *ovl = c->resident_ovl;
    if (policy) *policy = c->policy_mode;
    if (in_putllc) *in_putllc = kz3_ds_pc_in_putllc_loop(c->resident_ovl, kz3_ds_module_image_id(), p);
    return 1;
}
