/* Host test for KZ3 patch 2114 (RSX pass 107, libs/input/cam_interp.h): render-time camera interpolation between two
 * fixed-step poses. Checks: alpha 0/1 give prev/cur, the heading of a pure yaw step grows with alpha within 0.2 % of
 * the step (steps up to 0.3 rad), rows stay orthonormal with the input's handedness (both handednesses), the position
 * row is lerped, alpha is clamped, and the cut test (invalid pose, move over the limit, turn over the limit). No game
 * data. */
#include "cam_interp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char* what, int ok)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
/* z-up pose with heading h: row 1 = forward, row 0 = right, row 2 = up * hand; position p */
static void pose(float m[4][4], float h, float hand, float px, float py, float pz)
{
    memset(m, 0, sizeof(float) * 16);
    m[1][0] = cosf(h); m[1][1] = sinf(h);
    m[0][0] = sinf(h); m[0][1] = -cosf(h);
    m[2][2] = hand;
    m[3][0] = px; m[3][1] = py; m[3][2] = pz; m[3][3] = 1.0f;
}
static float maxdiff(const float a[4][4], const float b[4][4])
{
    float d = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) d = fmaxf(d, fabsf(a[i][j] - b[i][j]));
    return d;
}
static float handed(const float m[4][4])
{
    float c[3] = {m[0][1] * m[1][2] - m[0][2] * m[1][1], m[0][2] * m[1][0] - m[0][0] * m[1][2], m[0][0] * m[1][1] - m[0][1] * m[1][0]};
    return cam_dot3(c, m[2]);
}

int main(void)
{
    float P[4][4], C[4][4], O[4][4];
    pose(P, 0.4f, 1.0f, 1, 2, 3);
    pose(C, 0.55f, 1.0f, 2, 2, 5);
    cam_interp_pose(P, C, 0.0f, O);
    check("alpha 0 = prev", maxdiff(O, P) < 1e-6f);
    cam_interp_pose(P, C, 1.0f, O);
    check("alpha 1 = cur", maxdiff(O, C) < 1e-6f);
    cam_interp_pose(P, C, -3.0f, O);
    check("alpha below 0 clamps to prev", maxdiff(O, P) < 1e-6f);
    cam_interp_pose(P, C, 7.0f, O);
    check("alpha above 1 clamps to cur", maxdiff(O, C) < 1e-6f);
    cam_interp_pose(P, C, NAN, O);
    check("alpha NaN = prev", maxdiff(O, P) < 1e-6f);
    cam_interp_pose(P, C, 0.25f, O);
    check("position lerped", fabsf(O[3][0] - 1.25f) < 1e-6f && fabsf(O[3][2] - 3.5f) < 1e-6f);

    float worst = 0, worst_orth = 0;
    int mono = 1, hand_ok = 1;
    for (int hs = 0; hs < 2; hs++) {
        const float hand = hs ? -1.0f : 1.0f;
        for (float th = 0.005f; th <= 0.3001f; th += 0.005f) {
            pose(P, -2.0f, hand, 0, 0, 0);
            pose(C, -2.0f + th, hand, 0, 0, 0);
            float last = -1.0f;
            for (int k = 0; k <= 20; k++) {
                const float a = k / 20.0f;
                cam_interp_pose(P, C, a, O);
                float y = cam_pose_yaw(O) - -2.0f;
                worst = fmaxf(worst, fabsf(y - a * th) / th);
                if (y < last - 1e-7f) mono = 0;
                last = y;
                if (!cam_pose_ok(O, 1e-5f)) worst_orth = 1;
                if (!(handed(O) * hand > 0.99f)) hand_ok = 0;
            }
        }
    }
    printf("worst heading error %.4f %% of the step\n", worst * 100);
    check("heading = alpha x step within 0.2 % (steps <= 0.3 rad)", worst < 0.002f);
    check("heading monotone in alpha", mono);
    check("rows orthonormal", worst_orth == 0);
    check("handedness kept (both)", hand_ok);

    /* pitch + yaw step: rows still orthonormal */
    pose(P, 0.1f, 1.0f, 0, 0, 0);
    pose(C, 0.3f, 1.0f, 0, 0, 0);
    const float cp = cosf(0.2f), sp = sinf(0.2f);
    for (int j = 0; j < 3; j++) { const float f = C[1][j], u = C[2][j]; C[1][j] = cp * f + sp * u; C[2][j] = -sp * f + cp * u; }
    cam_interp_pose(P, C, 0.5f, O);
    check("yaw+pitch step orthonormal", cam_pose_ok(O, 1e-5f) && handed(O) > 0.99f);

    pose(P, 0.0f, 1.0f, 0, 0, 0);
    pose(C, 0.05f, 1.0f, 0.1f, 0, 0);
    check("cut: none for a normal step", cam_interp_cut(P, C, 5.0f, 1.5f) == CAM_CUT_NONE);
    pose(C, 0.05f, 1.0f, 6.0f, 0, 0);
    check("cut: move over 5 m", cam_interp_cut(P, C, 5.0f, 1.5f) == CAM_CUT_MOVE);
    pose(C, 2.0f, 1.0f, 0, 0, 0);
    check("cut: turn over 1.5 rad", cam_interp_cut(P, C, 5.0f, 1.5f) == CAM_CUT_TURN);
    pose(C, 0.05f, 1.0f, 0, 0, 0);
    C[0][0] = NAN;
    check("cut: NaN pose", cam_interp_cut(P, C, 5.0f, 1.5f) == CAM_CUT_INVALID);
    memset(C, 0, sizeof C);
    check("cut: zero pose (never written)", cam_interp_cut(P, C, 5.0f, 1.5f) == CAM_CUT_INVALID);
    printf("[test_cam_interp] %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
