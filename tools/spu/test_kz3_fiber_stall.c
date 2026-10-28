/* Host test of port/spu/kz3_fiber_stall.c (SPU pass 34): wait classification (static arena slots of both builds,
 * heap arenas by wait-site lr, everything else), the wait-stub lr window, the PUTLLC retry-loop test and the
 * breaker's decision sequence (report at 5 s then every 10 s; re-kick at one period, completion write at two,
 * reports only afterwards; period 0 = reports only). No game data, no runtime. */
#include "kz3_fiber_stall.h"
#include <stdio.h>

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %d: ", __LINE__);                                                                             \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

int main(void)
{
    int slot;
    /* the five words of rsx r32c / spu-31a and gp-t2 (the RE notes pass 67/68) */
    CHECK(kz3_ds_classify(0x0152690Cu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 0, "slot 0 v1.00");
    CHECK(kz3_ds_classify(0x015325ACu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 1, "slot 1 v1.00");
    CHECK(kz3_ds_classify(0x0153E24Cu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 2, "slot 2 v1.00");
    CHECK(kz3_ds_classify(0x01549EECu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 3, "slot 3 v1.00");
    CHECK(kz3_ds_classify(0x0156D4CCu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 6, "slot 6 v1.00 (gp-t2)");
    CHECK(kz3_ds_classify(0x01526800u + 7 * 0xBCA0u + 0x10Cu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 7, "slot 7");
    CHECK(kz3_ds_classify(0x01526800u + 8 * 0xBCA0u + 0x10Cu, 0, &slot) == KZ3_STALL_OTHER && slot == -1,
          "slot 8: past the pool");
    CHECK(kz3_ds_classify(0x0152690Cu + 4, 0, &slot) == KZ3_STALL_OTHER, "+0x110 is not the flag");
    CHECK(kz3_ds_classify(0x0152690Cu - 0x10Cu - 0x10u + 0x11Cu, 0, &slot) == KZ3_STALL_DS_STATIC,
          "the round-32 +0x11C reading is the same word");
    CHECK(kz3_ds_classify(0x0159BF00u + 0x10Cu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 0, "1.14 slot 0");
    CHECK(kz3_ds_classify(0x0159BF00u + 5 * 0xBCA0u + 0x10Cu, 0, &slot) == KZ3_STALL_DS_STATIC && slot == 5,
          "1.14 slot 5");
    CHECK(kz3_ds_classify(0x00000000u, 0, &slot) == KZ3_STALL_OTHER, "zero");
    CHECK(kz3_ds_classify(0x00000100u, 0, &slot) == KZ3_STALL_OTHER, "below the base (no wrap)");
    /* heap arenas: only the wait lr says */
    CHECK(kz3_ds_classify(0x4394807Cu, 0x002E4038u, &slot) == KZ3_STALL_DS_LR && slot == -1, "heap, site A v1.00");
    CHECK(kz3_ds_classify(0x43D9370Cu, 0x002E41A4u, &slot) == KZ3_STALL_DS_LR, "heap, site B v1.00");
    CHECK(kz3_ds_classify(0x4394807Cu, 0x002EF9C8u, &slot) == KZ3_STALL_DS_LR, "heap, site A 1.14");
    CHECK(kz3_ds_classify(0x4394807Cu, 0x002EFB34u, &slot) == KZ3_STALL_DS_LR, "heap, site B 1.14");
    CHECK(kz3_ds_classify(0x4394807Cu, 0x002E4034u, &slot) == KZ3_STALL_OTHER, "the bl address is not the lr");
    CHECK(kz3_ds_classify(0x4394807Cu, 0x000C3854u, &slot) == KZ3_STALL_OTHER,
          "the stub's inner lr classifies nothing");
    CHECK(kz3_ds_classify(0x4000011Cu, 0x0033489Cu, &slot) == KZ3_STALL_OTHER,
          "an entity +0x11C wait from the entity fiber");
    CHECK(kz3_ds_classify(0x4394807Cu, 0x002E4038u, NULL) == KZ3_STALL_DS_LR, "slot out optional");
    /* the wait stub window */
    CHECK(kz3_ds_lr_in_wait_stub(0x000C3854u) && kz3_ds_lr_in_wait_stub(0x000C383Cu) &&
              kz3_ds_lr_in_wait_stub(0x000C3860u),
          "v1.00 stub");
    CHECK(!kz3_ds_lr_in_wait_stub(0x000C3864u) && !kz3_ds_lr_in_wait_stub(0x000C3838u), "v1.00 stub bounds");
    CHECK(kz3_ds_lr_in_wait_stub(0x000C57F4u) && !kz3_ds_lr_in_wait_stub(0x000C5804u), "1.14 stub");
    CHECK(!kz3_ds_lr_in_wait_stub(0x002E4038u) && !kz3_ds_lr_in_wait_stub(0), "sites are outside");
    /* the PUTLLC loop */
    CHECK(kz3_ds_pc_in_putllc_loop(7, 7, 0xD838u) && kz3_ds_pc_in_putllc_loop(7, 7, 0xD86Cu) &&
              kz3_ds_pc_in_putllc_loop(7, 7, 0xD850u),
          "in the loop");
    CHECK(!kz3_ds_pc_in_putllc_loop(7, 7, 0xD834u) && !kz3_ds_pc_in_putllc_loop(7, 7, 0xD870u), "outside the loop");
    CHECK(!kz3_ds_pc_in_putllc_loop(6, 7, 0xD850u) && !kz3_ds_pc_in_putllc_loop(0, 0, 0xD850u),
          "other module / unknown module");
    /* the jm's wait loops (RE pass 71) */
    CHECK(kz3_jm_pc_class(0x2C30u) == KZ3_JM_PC_1A && kz3_jm_pc_class(0x2D0Cu) == KZ3_JM_PC_1A &&
              kz3_jm_pc_class(0x3040u) == KZ3_JM_PC_1A && kz3_jm_pc_class(0x31D8u) == KZ3_JM_PC_1A &&
              kz3_jm_pc_class(0x22F0u) == KZ3_JM_PC_1A && kz3_jm_pc_class(0x23C8u) == KZ3_JM_PC_1A,
          "1a ranges");
    CHECK(kz3_jm_pc_class(0x2D40u) == KZ3_JM_PC_1A_POLL && kz3_jm_pc_class(0x36A0u) == KZ3_JM_PC_1A_POLL,
          "1a poll ranges");
    CHECK(kz3_jm_pc_class(0x23DCu) == KZ3_JM_PC_1C && kz3_jm_pc_class(0x2480u) == KZ3_JM_PC_1C &&
              kz3_jm_pc_class(0x25F4u) == KZ3_JM_PC_1C && kz3_jm_pc_class(0x3500u) == KZ3_JM_PC_1C &&
              kz3_jm_pc_class(0x3800u) == KZ3_JM_PC_1C && kz3_jm_pc_class(0x38F8u) == KZ3_JM_PC_1C,
          "1c ranges win inside the helper");
    CHECK(kz3_jm_pc_class(0x2580u) == KZ3_JM_PC_1A && kz3_jm_pc_class(0x24D8u) == KZ3_JM_PC_1A,
          "helper outside its PUTLLC loops is 1a");
    CHECK(kz3_jm_pc_class(0x3980u) == KZ3_JM_PC_1B && kz3_jm_pc_class(0xD850u) == KZ3_JM_PC_1B, "job code");
    CHECK(kz3_jm_pc_class(0xA00u) == KZ3_JM_PC_OTHER && kz3_jm_pc_class(0x2D10u) == KZ3_JM_PC_OTHER &&
              kz3_jm_pc_class(0x397Cu) == KZ3_JM_PC_OTHER,
          "other");
    CHECK(kz3_jm_pc_class_name(KZ3_JM_PC_1C)[0] == '1' && kz3_jm_pc_class_name(99)[0] == 'j', "names");
    /* decision sequence, period 2 */
    struct kz3_ds_breaker b;
    kz3_ds_breaker_reset(&b, 100.0);
    CHECK(kz3_ds_breaker_step(&b, 100.0, 2.0) == 0, "t0: nothing");
    CHECK(kz3_ds_breaker_step(&b, 101.9, 2.0) == 0, "1.9 s: nothing");
    CHECK(kz3_ds_breaker_step(&b, 102.0, 2.0) == (KZ3_DS_REPORT | KZ3_DS_REKICK), "2.0 s: report + re-kick");
    CHECK(kz3_ds_breaker_step(&b, 102.1, 2.0) == 0, "2.1 s: nothing (re-kick once)");
    CHECK(kz3_ds_breaker_step(&b, 103.9, 2.0) == 0, "3.9 s: nothing");
    CHECK(kz3_ds_breaker_step(&b, 104.0, 2.0) == (KZ3_DS_REPORT | KZ3_DS_COMPLETE), "4.0 s: report + completion");
    CHECK(kz3_ds_breaker_step(&b, 104.5, 2.0) == 0, "4.5 s: nothing");
    CHECK(kz3_ds_breaker_step(&b, 114.0, 2.0) == KZ3_DS_REPORT, "14 s: report only (10 s cadence)");
    CHECK(kz3_ds_breaker_step(&b, 120.0, 2.0) == 0 && kz3_ds_breaker_step(&b, 124.0, 2.0) == KZ3_DS_REPORT,
          "24 s: next report");
    CHECK(b.stage == 2, "stage done");
    /* a long period: the 5 s report comes first */
    kz3_ds_breaker_reset(&b, 0.0);
    CHECK(kz3_ds_breaker_step(&b, 4.9, 30.0) == 0 && kz3_ds_breaker_step(&b, 5.0, 30.0) == KZ3_DS_REPORT, "5 s report");
    CHECK(kz3_ds_breaker_step(&b, 14.9, 30.0) == 0 && kz3_ds_breaker_step(&b, 15.0, 30.0) == KZ3_DS_REPORT,
          "15 s report");
    CHECK(kz3_ds_breaker_step(&b, 30.0, 30.0) == (KZ3_DS_REPORT | KZ3_DS_REKICK), "30 s re-kick");
    CHECK(kz3_ds_breaker_step(&b, 59.0, 30.0) == KZ3_DS_REPORT, "59 s report");
    CHECK(kz3_ds_breaker_step(&b, 60.0, 30.0) == (KZ3_DS_REPORT | KZ3_DS_COMPLETE), "60 s completion");
    /* period 0: reports only, forever */
    kz3_ds_breaker_reset(&b, 0.0);
    CHECK(kz3_ds_breaker_step(&b, 5.0, 0.0) == KZ3_DS_REPORT && kz3_ds_breaker_step(&b, 15.0, 0.0) == KZ3_DS_REPORT &&
              kz3_ds_breaker_step(&b, 1000.0, 0.0) == KZ3_DS_REPORT && b.stage == 0,
          "period 0: reports only");
    /* reset restarts the sequence (a new stall after a completion write) */
    kz3_ds_breaker_reset(&b, 200.0);
    CHECK(b.stage == 0 && kz3_ds_breaker_step(&b, 202.0, 2.0) == (KZ3_DS_REPORT | KZ3_DS_REKICK), "reset restarts");

    printf("%s: %d checks failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
