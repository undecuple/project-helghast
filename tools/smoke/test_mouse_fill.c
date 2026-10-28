/* Host test for KZ3 patch 2116 (RSX pass 110, render-rate fill, libs/input/pad_mouse.h): a simulated mouse (uneven
 * arrival times, turns, stops, reversals), uneven frames, 1/30 s and 2/30 s logic slots with the pass 107 slot clock
 * (each update takes the counts up to slot end - 1/30 s through the real accumulator: pad_look_acc_add_at /
 * take_until / peek), and the camera Rep model measured in pass 110 (Rep = logic - (1 - alpha) x the last take, alpha
 * = the frame's place in the slot). Checks: the fill makes the drawn yaw and pitch equal every count that arrived by
 * the frame (each count shown once: no loss, no double count, no jump at a take), the 2112 latch alone (pending only)
 * misses (1 - alpha) x take, and the fill never passes a pitch limit the logic sits at. No game data. */
#include "pad_mouse.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
static unsigned long long s_rng = 0x2116;
static double rnd(void) /* [0, 1) */
{
    s_rng = s_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(s_rng >> 11) / 9007199254740992.0;
}
static PadLookAcc s_acc;

int main(void)
{
    const unsigned long long TB = 79800000ull, T = 2660000ull; /* guest timebase, 1/30 s */
    const float k = 0.00096f;
    unsigned long long now = TB, next_ev = TB, E = TB; /* E = the end of the slot last simulated */
    unsigned long long arrived_x = 0;
    long long sum_x = 0, sum_y = 0; /* counts arrived (truth) */
    double Y = 0.0, P = 0.0;        /* logic yaw / pitch (pitch kept inside the limits by the inputs) */
    float tdy = 0.f, tdp = 0.f;     /* the last take's applied deltas */
    unsigned long long L = T;
    unsigned frame = 0;
    int steps = 0, two = 0, idle_frames = 0, frames = 0;
    double max_fill = 0.0, max_fill_p = 0.0, max_latch = 0.0, max_jump = 0.0;
    double prev_err = 0.0;
    (void)arrived_x;
    { /* a consumer is running before the first count (else the accumulator drops the first counts as stale) */
        int dx, dy;
        long long dr;
        pad_look_acc_take_until(&s_acc, frame, 0, &dx, &dy, &dr);
    }
    for (int f = 0; f < 3000; f++) {
        /* frame interval 18-63 ms, uneven (under one 2/30 s slot, as the game keeps up; a longer gap slows game time) */
        const unsigned long long fnow = now + (unsigned long long)((0.018 + 0.045 * rnd()) * (double)TB);
        /* mouse: phase by time: 0-20 s right with jitter, a stop, reversals, bursts */
        while (next_ev <= fnow) {
            const double ts = (double)next_ev / (double)TB;
            const int ph = (int)(ts / 2.5) % 4;
            int dx = 0, dy = 0;
            if (ph == 0)
                dx = 1 + (int)(3 * rnd());
            else if (ph == 1)
                dx = -(int)(4 * rnd());
            else if (ph == 2) {
                dx = rnd() < 0.3 ? 9 : 0;
                dy = (int)(3 * rnd()) - 1;
            } else
                dy = rnd() < 0.5 ? 1 : -1;
            pad_look_acc_add_at(&s_acc, dx, dy, frame, next_ev);
            sum_x += dx;
            sum_y += dy;
            next_ev += (unsigned long long)((0.0005 + 0.008 * rnd()) * (double)TB);
        }
        now = fnow;
        frame++;
        /* GameWorld::Update: one slot when the last slot end was reached; 2T when a whole slot behind */
        if (now >= E) {
            L = now >= E + T ? 2 * T : T;
            E += L;
            const unsigned long long cut = E - T; /* never in the future: now >= E_old + L - T */
            int dx = 0, dy = 0;
            long long dropped = 0;
            pad_look_acc_take_until(&s_acc, frame, cut, &dx, &dy, &dropped);
            tdy = -(float)dx * k;
            tdp = -(float)dy * k;
            Y += tdy;
            P += tdp;
            steps++;
            two += L == 2 * T;
        } else
            idle_frames++;
        const float a = 1.0f - (float)((double)(E - now) / (double)L);
        /* the camera Rep at the frame's game time (pass 110 model) */
        const double repY = Y - (1.0 - a) * tdy, repP = P - (1.0 - a) * tdp;
        int px = 0, py = 0;
        pad_look_acc_peek(&s_acc, frame, &px, &py);
        float pdy, pdp, fy, fp;
        pad_look_latch_delta(k, 0, px, py, (float)Y, 1.f, -1.f, (float)P, 1.f, -1.f, &pdy, &pdp);
        pad_look_fill_delta(a, tdy, tdp, pdy, pdp, &fy, &fp);
        const double truthY = -(double)sum_x * k, truthP = -(double)sum_y * k;
        const double eY = fabs(repY + fy - truthY), eP = fabs(repP + fp - truthP), eL = fabs(repY + pdy - truthY);
        if (eY > max_fill) max_fill = eY;
        if (eP > max_fill_p) max_fill_p = eP;
        if (eL > max_latch) max_latch = eL;
        if (frames && fabs(eY - prev_err) > max_jump) max_jump = fabs(eY - prev_err);
        prev_err = eY;
        frames++;
    }
    printf("frames %d, steps %d (2T %d), frames without a step %d, counts %lld,%lld\n", frames, steps, two, idle_frames,
           sum_x, sum_y);
    printf("max |drawn - every count| yaw: fill %.2e rad, latch only %.2e rad; pitch fill %.2e\n", max_fill, max_latch,
           max_fill_p);
    check("both slot lengths and step-less frames occur", two > 50 && idle_frames > 20 && steps - two > 50);
    check("fill: drawn yaw = every count that arrived (no loss, no double count)", max_fill < 2e-4);
    check("fill: drawn pitch = every count that arrived", max_fill_p < 2e-4);
    check("fill: no jump at a take", max_jump < 2e-4);
    check("2112 latch alone misses (1 - alpha) x take", max_latch > 20 * k);

    /* pitch at its limit: the take hit pmax, the pending counts push further up -> drawn stays at pmax */
    {
        const float pmax = 1.0472f, a = 0.4f, tp = 0.01f; /* the take raised the pitch to the limit by 0.01 */
        float pdy, pdp, fy, fp;
        pad_look_latch_delta(k, 0, 0, -50, 0.f, 1.f, -1.f, pmax, -pmax, pmax, &pdy, &pdp);
        pad_look_fill_delta(a, 0.f, tp, pdy, pdp, &fy, &fp);
        const float rep = pmax - (1.f - a) * tp;
        check("fill: pitch stays at the limit the logic sits at", fabsf(rep + fp - pmax) < 1e-6f && pdp == 0.f);
    }
    /* alpha out of range: clamped; NaN adds nothing consumed */
    {
        float fy, fp;
        pad_look_fill_delta(NAN, 0.5f, 0.5f, 0.1f, 0.2f, &fy, &fp);
        check("fill: NaN alpha adds only the pending part", fy == 0.1f && fp == 0.2f);
        pad_look_fill_delta(-3.f, 0.5f, 0.f, 0.f, 0.f, &fy, &fp);
        check("fill: alpha < 0 clamps to the whole take", fy == 0.5f);
        pad_look_fill_delta(7.f, 0.5f, 0.f, 0.f, 0.f, &fy, &fp);
        check("fill: alpha > 1 clamps to nothing consumed", fy == 0.f);
    }
    printf("test_mouse_fill: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
