#include "fsr31_motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char* name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        failures++;
    }
}

static void pose(float out[16], float yaw, float px, float py, float pz)
{
    memset(out, 0, sizeof(float) * 16);
    out[0] = sinf(yaw);
    out[1] = -cosf(yaw);
    out[4] = cosf(yaw);
    out[5] = sinf(yaw);
    out[10] = 1.0f;
    out[12] = px;
    out[13] = py;
    out[14] = pz;
    out[15] = 1.0f;
}

int main(void)
{
    const float hfov = 1.4f;
    const float vfov = 0.9f;
    const float near_plane = 0.1f;
    const float far_plane = 1000.0f;
    const float depth_10 =
        (far_plane - near_plane * far_plane / 10.0f) /
        (far_plane - near_plane);
    float previous[16], current[16], mx, my;

    pose(previous, 0.0f, 0.0f, 0.0f, 0.0f);
    pose(current, 0.0f, 0.0f, 0.0f, 0.0f);
    check(
        "identity camera has zero center motion",
        kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            depth_10, near_plane, far_plane,
            0.0f, 0.0f, &mx, &my) &&
            fabsf(mx) < 1e-6f && fabsf(my) < 1e-6f);
    check(
        "identity camera has zero edge motion",
        kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            depth_10, near_plane, far_plane,
            0.8f, -0.6f, &mx, &my) &&
            fabsf(mx) < 1e-6f && fabsf(my) < 1e-6f);

    pose(current, 0.1f, 0.0f, 0.0f, 0.0f);
    const float expected = -tanf(0.1f) / tanf(hfov * 0.5f);
    check(
        "yaw reprojects center into the previous frame",
        kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            depth_10, near_plane, far_plane,
            0.0f, 0.0f, &mx, &my) &&
            fabsf(mx - expected) < 1e-5f && fabsf(my) < 1e-6f);

    pose(current, 0.0f, 0.0f, 1.0f, 0.0f);
    const float expected_translation =
        -1.0f / (10.0f * tanf(hfov * 0.5f));
    check(
        "depth reconstructs camera translation",
        kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            depth_10, near_plane, far_plane,
            0.0f, 0.0f, &mx, &my) &&
            fabsf(mx - expected_translation) < 1e-5f &&
            fabsf(my) < 1e-6f);

    float near_motion;
    const int near_valid = kz3_fsr31_camera_motion(
        previous, hfov, vfov, current, hfov, vfov,
        0.0f, near_plane, far_plane,
        0.0f, 0.0f, &near_motion, &my);
    float far_motion;
    const int far_valid = kz3_fsr31_camera_motion(
        previous, hfov, vfov, current, hfov, vfov,
        1.0f, near_plane, far_plane,
        0.0f, 0.0f, &far_motion, &my);
    check(
        "translation decreases with reconstructed distance",
        near_valid && far_valid &&
            fabsf(near_motion) > fabsf(far_motion));

    pose(current, 0.0f, 0.0f, 0.0f, 0.0f);
    check(
        "a wider current field of view produces finite reprojection",
        kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov * 1.1f, vfov * 1.1f,
            depth_10, near_plane, far_plane,
            0.7f, 0.5f, &mx, &my) &&
            isfinite(mx) && isfinite(my) && mx > 0.0f && my > 0.0f);

    check(
        "invalid field of view is rejected",
        !kz3_fsr31_camera_motion(
            previous, 0.0f, vfov, current, hfov, vfov,
            depth_10, near_plane, far_plane,
            0.0f, 0.0f, &mx, &my));
    check(
        "depth outside the normal range is rejected",
        !kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            1.1f, near_plane, far_plane,
            0.0f, 0.0f, &mx, &my));
    check(
        "invalid projection planes are rejected",
        !kz3_fsr31_camera_motion(
            previous, hfov, vfov, current, hfov, vfov,
            depth_10, far_plane, near_plane,
            0.0f, 0.0f, &mx, &my));

    printf("[test_fsr31_motion] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
