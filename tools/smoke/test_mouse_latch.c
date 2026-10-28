/* Host test for KZ3 patch 2112 (RSX pass 105, late-latch view rotation, libs/input/pad_mouse.h + pad_mouse.c): the
 * non-consuming peek (same counts as the next take, nothing cleared, stale counts of an idle stretch = 0), the render
 * delta (equal to what the next look update adds, same clamps), the pose rotation (yaw about world +Z, pitch toward up
 * whatever the basis handedness, rows stay orthonormal, position row untouched by design) and the no-jump-back rule
 * (shown = logic + untaken counts stays monotone across a take). No game data. */
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
static int near(float a, float b)
{
    return fabsf(a - b) < 1e-5f;
}
static unsigned s_frame;
unsigned cellGcm_flip_request_count(void)
{
    return s_frame;
}

static float dot(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static int ortho(float r[3][3])
{
    for (int i = 0; i < 3; i++) {
        if (fabsf(dot(r[i], r[i]) - 1.0f) > 1e-5f) return 0;
        for (int j = i + 1; j < 3; j++)
            if (fabsf(dot(r[i], r[j])) > 1e-5f) return 0;
    }
    return 1;
}

int main(void)
{
    /* peek on the pure accumulator */
    PadLookAcc a;
    memset(&a, 0, sizeof a);
    int dx, dy;
    pad_look_acc_take(&a, 1, &dx, &dy, NULL);
    pad_look_acc_add(&a, 7, -3, 1);
    pad_look_acc_add(&a, 5, 1, 2);
    pad_look_acc_peek(&a, 2, &dx, &dy);
    check("peek = counts kept since the take", dx == 12 && dy == -2);
    pad_look_acc_peek(&a, 2, &dx, &dy);
    check("peek does not clear", dx == 12 && dy == -2);
    pad_look_acc_take(&a, 2, &dx, &dy, NULL);
    check("the take after a peek hands out the same counts", dx == 12 && dy == -2);
    pad_look_acc_peek(&a, 2, &dx, &dy);
    check("peek after the take = 0", dx == 0 && dy == 0);
    pad_look_acc_add(&a, 9, 9, 3);
    pad_look_acc_peek(&a, 3 + PAD_LOOK_IDLE_FRAMES, &dx, &dy);
    check("idle stretch: counts the next take would drop peek as 0", dx == 0 && dy == 0);

    /* the runtime wrapper (lock + frame clock) */
    int tx, ty;
    s_frame = 100;
    pad_mouse_take(0.0, &tx, &ty);
    pad_mouse_add(4, -6);
    s_frame = 101;
    pad_mouse_peek(&dx, &dy);
    pad_mouse_take(0.0, &tx, &ty);
    check("pad_mouse_peek = the next pad_mouse_take", dx == 4 && dy == -6 && tx == 4 && ty == -6);

    /* render delta = what the look update will add */
    PadMouseCfg c = {0.003f, 0.5f, 0};
    float y = 0.4f, p = 0.2f, dyaw, dpitch;
    pad_mouse_look_apply(&c, 30, -40, 1.0f, 0, -1.0f, 1.0f, &y, &p);
    pad_look_latch_delta(0.003f, 0, 30, -40, 0.4f, 1.0f, -1.0f, 0.2f, -1.0f, 1.0f, &dyaw, &dpitch);
    check("delta = look_apply's change (yaw, pitch)", near(0.4f + dyaw, y) && near(0.2f + dpitch, p));
    pad_look_latch_delta(0.003f, 0, 0, -300, 0.0f, 1.0f, -1.0f, 0.9f, -1.047f, 1.047f, &dyaw, &dpitch);
    check("pitch clamped to the limit from the logic pitch", near(dpitch, 1.047f - 0.9f));
    pad_look_latch_delta(0.003f, 1, 0, -100, 0.0f, 1.0f, -1.0f, 0.0f, -1.0f, 1.0f, &dyaw, &dpitch);
    check("inverted: up = negative pitch", near(dpitch, -0.3f));
    pad_look_latch_delta(0.003f, 0, -200, 0, 0.3f, -0.61f, 0.4363f, 0.0f, -0.35f, 0.35f, &dyaw, &dpitch);
    check("gun yaw clamped to its range", near(dyaw, 0.4363f - 0.3f));

    /* rotation: right-handed basis right +x, forward +y, up +z */
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    pad_look_latch_rotate(r, 0.1f, 0.0f);
    check("yaw +0.1: heading atan2(fwd.y, fwd.x) +0.1, level",
          near(atan2f(r[1][1], r[1][0]), 1.5707963f + 0.1f) && near(r[1][2], 0.0f) && near(r[2][2], 1.0f));
    pad_look_latch_rotate(r, 0.0f, 0.2f);
    check("pitch +0.2: forward.z = sin 0.2 (up), heading kept",
          near(r[1][2], sinf(0.2f)) && near(atan2f(r[1][1], r[1][0]), 1.5707963f + 0.1f));
    check("rows stay orthonormal", ortho(r));
    float l[3][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; /* the other handedness */
    pad_look_latch_rotate(l, 0.0f, 0.2f);
    check("pitch direction does not depend on the basis handedness", near(l[1][2], sinf(0.2f)) && near(l[0][0], -1.0f));
    float t[3][3] = {{0.8f, -0.6f, 0}, {0.6f, 0.8f, 0}, {0, 0, 1}};
    pad_look_latch_rotate(t, 0.0f, 0.0f);
    check("zero delta leaves the pose alone", t[0][0] == 0.8f && t[1][1] == 0.8f);

    /* no jump back: the shown yaw = logic yaw + untaken counts; counts 10 per frame, a take every 3rd frame */
    {
        PadLookAcc m;
        memset(&m, 0, sizeof m);
        float logic = 0.0f, prev = 1e9f;
        int mono = 1, f = 0;
        pad_look_acc_take(&m, 0, &dx, &dy, NULL);
        for (f = 1; f <= 30; f++) {
            pad_look_acc_add(&m, 10, 0, f);
            if (f % 3 == 0) {
                pad_look_acc_take(&m, f, &dx, &dy, NULL);
                logic += -(float)dx * 0.003f;
            }
            pad_look_acc_peek(&m, f, &dx, &dy);
            pad_look_latch_delta(0.003f, 0, dx, dy, logic, 1.0f, -1.0f, 0.0f, -1.0f, 1.0f, &dyaw, &dpitch);
            const float shown = logic + dyaw;
            if (prev != 1e9f && !(shown < prev - 0.029f && shown > prev - 0.031f)) mono = 0;
            prev = shown;
        }
        check("shown yaw turns by the same step every frame across takes", mono);
    }
    printf("[test_mouse_latch] %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
