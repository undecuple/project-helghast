/* kz3_fiber_stall.c: see kz3_fiber_stall.h. Pure C, host-tested by tools/spu/test_kz3_fiber_stall.sh. */
#include "kz3_fiber_stall.h"

static int static_slot(uint32_t w, uint32_t base)
{
    uint32_t off = w - base;
    if (w < base) return -1;
    if (off % KZ3_DS_ARENA_STRIDE != KZ3_DS_ARENA_FLAG) return -1;
    if (off / KZ3_DS_ARENA_STRIDE >= KZ3_DS_ARENA_SLOTS) return -1;
    return (int)(off / KZ3_DS_ARENA_STRIDE);
}

int kz3_ds_classify(uint32_t w, uint32_t lr, int* slot)
{
    int s = static_slot(w, KZ3_DS_ARENA_BASE_100);
    if (s < 0) s = static_slot(w, KZ3_DS_ARENA_BASE_114);
    if (slot) *slot = s;
    if (s >= 0) return KZ3_STALL_DS_STATIC;
    if (lr == KZ3_DS_WAIT_LR_A_100 || lr == KZ3_DS_WAIT_LR_B_100 || lr == KZ3_DS_WAIT_LR_A_114 ||
        lr == KZ3_DS_WAIT_LR_B_114)
        return KZ3_STALL_DS_LR;
    return KZ3_STALL_OTHER;
}

int kz3_ds_lr_in_wait_stub(uint32_t lr)
{
    return (lr - KZ3_DS_WAIT_STUB_100 < KZ3_DS_WAIT_STUB_LEN) || (lr - KZ3_DS_WAIT_STUB_114 < KZ3_DS_WAIT_STUB_LEN);
}

int kz3_ds_pc_in_putllc_loop(int image_id, int ds_image_id, uint32_t pc)
{
    return ds_image_id > 0 && image_id == ds_image_id && pc >= KZ3_DS_PUTLLC_LO && pc <= KZ3_DS_PUTLLC_HI;
}

void kz3_ds_breaker_reset(struct kz3_ds_breaker* b, double now)
{
    b->t0 = now;
    b->next_report = now + 5.0;
    b->stage = 0;
}

int kz3_ds_breaker_step(struct kz3_ds_breaker* b, double now, double period)
{
    int act = 0;
    if (period > 0) {
        if (b->stage == 0 && now - b->t0 >= period) {
            b->stage = 1;
            act |= KZ3_DS_REKICK;
        } else if (b->stage == 1 && now - b->t0 >= 2 * period) {
            b->stage = 2;
            act |= KZ3_DS_COMPLETE;
        }
    }
    if (act || now >= b->next_report) {
        act |= KZ3_DS_REPORT;
        b->next_report = now + 10.0;
    }
    return act;
}

int kz3_jm_pc_class(uint32_t pc)
{
    /* the PUTLLC loops first: 0x23DC-0x24D4 and 0x2598-0x25F4 lie inside the take-entry helper's 1a range */
    static const uint32_t c[][2] = {
        {0x23DC, 0x24D4}, {0x2598, 0x25F4}, {0x34E8, 0x353C}, {0x3800, 0x385C}, {0x38A4, 0x38F8}};
    static const uint32_t ap[][2] = {{0x2D40, 0x2DBC}, {0x3678, 0x36D8}};
    static const uint32_t a[][2] = {
        {0x2C30, 0x2D0C}, {0x3040, 0x30B0}, {0x31D4, 0x31DC}, {0x23C8, 0x2580}, {0x22E0, 0x230C}};
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++)
        if (pc >= c[i][0] && pc <= c[i][1]) return KZ3_JM_PC_1C;
    for (i = 0; i < sizeof ap / sizeof ap[0]; i++)
        if (pc >= ap[i][0] && pc <= ap[i][1]) return KZ3_JM_PC_1A_POLL;
    for (i = 0; i < sizeof a / sizeof a[0]; i++)
        if (pc >= a[i][0] && pc <= a[i][1]) return KZ3_JM_PC_1A;
    if (pc >= 0x3980) return KZ3_JM_PC_1B;
    return KZ3_JM_PC_OTHER;
}

const char* kz3_jm_pc_class_name(int cls)
{
    switch (cls) {
    case KZ3_JM_PC_1A:
        return "1a dependency spin";
    case KZ3_JM_PC_1A_POLL:
        return "1a spin with poll";
    case KZ3_JM_PC_1C:
        return "1c PUTLLC retry";
    case KZ3_JM_PC_1B:
        return "1b job code";
    default:
        return "jm other";
    }
}
