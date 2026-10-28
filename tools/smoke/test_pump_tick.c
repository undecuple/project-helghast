/* Host test for RSX pass 117 (port/kz3/main.cpp frame_clock: the message pump on every vblank tick, KZ3_PUMP_TICK).
 * A model of frame_clock's two loops on a simulated millisecond clock, with the real look accumulator and slot clock
 * (pad_mouse.h: pad_look_acc_add_at / pad_look_acc_take_until). Raw mouse reports arrive every 1 ms (4 counts, as the
 * player's ~660-1000 reports/s); the window's messages reach the accumulator only when the loop pumps, stamped with the
 * pump time (WM_INPUT handling calls pad_mouse_add then). The look update runs every 1/15 s slot and takes the counts up
 * to the slot end minus 1/30 s. While a FIFO drain costs more than the 16 ms tick (a heavy level, 50 ms here):
 *   old loop (pump only in the outer loop): the catch-up pass grows to up to 240 ticks, the pump waits seconds, the
 *     look update takes nothing for many slots and then thousands of counts at once (the player's hold-then-jump);
 *   new loop (pump after every tick): every slot gets its share, the longest pump gap is one drain + one tick.
 * The model is a copy of the loop's shape, not the loop itself: it checks the reasoning, the Proton runs check the exe.
 *   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_pump_tick.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad_mouse.h"

#define SIM_MS     20000.0
#define TICK_MS    16.0
#define SLOT_MS    (1000.0 / 15.0)
#define TB_PER_MS  79800.0
#define RATE_CPMS  4            /* counts per 1 ms report */

static double cost(double t) { return (t >= 5000.0 && t < 15000.0) ? 50.0 : 2.0; }   /* one tick's FIFO drain */

static double s_pumps[200000];
static int s_np;

/* frame_clock's shape (Sleep, the catch-up loop with its 240 cap and resync, the outer drain + pump). */
static void run_loop(int pump_each_tick)
{
    double t = 0.0, next_tick = 0.0;
    s_np = 0;
    while (t < SIM_MS) {
        t += 4.0;                                   /* Sleep(4) */
        const double now = t;
        int fired = 0;
        while (now - next_tick >= 0.0 && fired < 240) {
            t += cost(t);                           /* TickVBlank/TickFlip, present, cellGcm_rsx_process_fifo */
            if (pump_each_tick) s_pumps[s_np++] = t;
            next_tick += TICK_MS;
            fired++;
        }
        if (fired >= 240) next_tick = now;
        t += 1.0;                                   /* outer drain (the catch-up pass left little queued) */
        s_pumps[s_np++] = t;                        /* kz3_rsx_pump */
    }
}

typedef struct { double max_gap; long long max_take; int max_zero_run; long long total; } Res;

static Res run_look(void)
{
    static PadLookAcc a;
    memset(&a, 0, sizeof a);
    Res r = {0, 0, 0, 0};
    double arrived = 0.0;      /* reports up to this time are in the window's queue or delivered */
    double delivered = 0.0;    /* reports up to this time are in the accumulator */
    int p = 0, zero_run = 0;
    unsigned frame = 0;
    for (int k = 1; k * SLOT_MS < SIM_MS; k++) {
        const double slot_end = k * SLOT_MS;
        while (p < s_np && s_pumps[p] <= slot_end) {        /* pumps before this look update */
            const double pt = s_pumps[p];
            const double gap = p ? pt - s_pumps[p - 1] : pt;
            if (gap > r.max_gap && pt > 5500.0 && pt < 15000.0) r.max_gap = gap;
            const int n = (int)(pt - delivered);             /* 1 ms reports queued since the last pump */
            if (n > 0) {
                pad_look_acc_add_at(&a, n * RATE_CPMS, 0, frame, (unsigned long long)(pt * TB_PER_MS));
                delivered += n;
            }
            p++;
        }
        (void)arrived;
        int dx = 0, dy = 0;
        long long dropped = 0;
        const double cut = slot_end - SLOT_MS / 2.0;
        pad_look_acc_take_until(&a, frame, (unsigned long long)(cut * TB_PER_MS), &dx, &dy, &dropped);
        frame++;
        r.total += dx;
        if (slot_end > 5500.0 && slot_end < 15000.0) {
            if (dx > r.max_take) r.max_take = dx;
            zero_run = dx ? 0 : zero_run + 1;
            if (zero_run > r.max_zero_run) r.max_zero_run = zero_run;
        }
        if (dropped) { fprintf(stderr, "FAIL: %lld counts dropped\n", dropped); exit(1); }
    }
    return r;
}

int main(void)
{
    int fail = 0;
    run_loop(0);
    const Res o = run_look();
    run_loop(1);
    const Res n = run_look();
    const long long per_slot = (long long)(SLOT_MS * RATE_CPMS);
    printf("[test_pump_tick] heavy phase (drain 50 ms > 16 ms tick), %lld counts per slot at a steady hand:\n", per_slot);
    printf("  old loop: longest pump gap %.0f ms, largest take %lld counts, %d slot(s) in a row with no counts\n",
           o.max_gap, o.max_take, o.max_zero_run);
    printf("  new loop: longest pump gap %.0f ms, largest take %lld counts, %d slot(s) in a row with no counts\n",
           n.max_gap, n.max_take, n.max_zero_run);
    /* the model must show the player's symptom on the old loop, or it is not testing the cause */
    if (!(o.max_gap > 1000.0 && o.max_take > 10 * per_slot && o.max_zero_run >= 10)) {
        printf("FAIL: the old loop does not hold the counts back in the model\n"); fail = 1;
    }
    if (!(n.max_gap <= 50.0 + TICK_MS + 1.0)) { printf("FAIL: new loop pump gap %.0f ms\n", n.max_gap); fail = 1; }
    if (!(n.max_take <= 2 * per_slot)) { printf("FAIL: new loop take %lld > 2 slots\n", n.max_take); fail = 1; }
    if (!(n.max_zero_run == 0)) { printf("FAIL: new loop has %d empty slots\n", n.max_zero_run); fail = 1; }
    printf("[test_pump_tick] %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
