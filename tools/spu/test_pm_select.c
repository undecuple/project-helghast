/* Patch 1058 host test: the selectWorkload answer (runtime/spu/spurs_select.h) against the WWS job-manager PM's
 * own check (wrapper 0x3678, `ceq r5, cur` at 0x36C8, RE pass 86 s.6.2): a run keeps going only while the answer
 * equals wklCurrentId (LS 0x1DC). Pure: no game data, no runtime. */
#include <stdio.h>
#include "spurs_select.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
/* The PM's reading of the answer: 1 = keep running (0x2D40 returns 1), 0 = another workload, stop. */
static int pm_keeps_running(unsigned cur, unsigned ans) { return ans == cur; }

int main(void)
{
    for (unsigned cur = 0; cur < 16; cur++) {
        /* new: within budget and nothing waiting -> the PM keeps running (several jobs per run) */
        CHECK(pm_keeps_running(cur, spurs_pm_select_answer(cur, 0, 0, 0)), "new keep wid %u", cur);
        /* new: over budget or the kernel wants the SPU -> the PM stops, for every wid (1048's old 1 kept wid 1) */
        CHECK(!pm_keeps_running(cur, spurs_pm_select_answer(cur, 0, 1, 0)), "new budget wid %u", cur);
        CHECK(!pm_keeps_running(cur, spurs_pm_select_answer(cur, 0, 0, 1)), "new want wid %u", cur);
        CHECK(spurs_pm_select_answer(cur, 0, 0, 1) < 16, "answer is a workload id, wid %u", cur);
        /* old (KZ3_PM_SELECT_OLD=1): 0 / 1, i.e. the bug: every wid != 0 stops after one job */
        CHECK(spurs_pm_select_answer(cur, 1, 0, 0) == 0 && spurs_pm_select_answer(cur, 1, 1, 0) == 1, "old values wid %u", cur);
        CHECK(pm_keeps_running(cur, spurs_pm_select_answer(cur, 1, 0, 0)) == (cur == 0), "old keep only wid 0, wid %u", cur);
        CHECK(pm_keeps_running(cur, spurs_pm_select_answer(cur, 1, 1, 0)) == (cur == 1), "old budget accident wid %u", cur);
        /* the old mode ignores the kernel's wish (it never had one) */
        CHECK(spurs_pm_select_answer(cur, 1, 0, 1) == 0, "old ignores want, wid %u", cur);
    }
    printf("%s: test_pm_select (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
