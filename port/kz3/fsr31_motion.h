#ifndef KZ3_FSR31_MOTION_H
#define KZ3_FSR31_MOTION_H

#include <math.h>

/*
 * Camera-only reprojection for KZ3's row-major pose:
 * row 0 = right, row 1 = forward, row 2 = up, row 3 = position.
 * Translation and object motion are intentionally absent until reliable
 * per-pixel depth and object transforms are available.
 */
static inline float kz3_fsr31_dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline int kz3_fsr31_rotation_motion(
    const float previous_pose[16],
    float previous_horizontal_fov,
    float previous_vertical_fov,
    const float current_pose[16],
    float current_horizontal_fov,
    float current_vertical_fov,
    float current_ndc_x,
    float current_ndc_y,
    float* out_ndc_x,
    float* out_ndc_y)
{
    const float current_tan_x = tanf(current_horizontal_fov * 0.5f);
    const float current_tan_y = tanf(current_vertical_fov * 0.5f);
    const float previous_tan_x = tanf(previous_horizontal_fov * 0.5f);
    const float previous_tan_y = tanf(previous_vertical_fov * 0.5f);
    if (!(current_tan_x > 0.0f) || !(current_tan_y > 0.0f) ||
        !(previous_tan_x > 0.0f) || !(previous_tan_y > 0.0f)) {
        return 0;
    }

    float ray[3];
    for (int axis = 0; axis < 3; ++axis) {
        ray[axis] =
            current_pose[4 + axis] +
            current_pose[axis] * current_ndc_x * current_tan_x +
            current_pose[8 + axis] * current_ndc_y * current_tan_y;
    }
    const float previous_forward = kz3_fsr31_dot3(ray, previous_pose + 4);
    if (!(previous_forward > 1e-5f)) {
        return 0;
    }

    const float previous_x =
        kz3_fsr31_dot3(ray, previous_pose) /
        (previous_forward * previous_tan_x);
    const float previous_y =
        kz3_fsr31_dot3(ray, previous_pose + 8) /
        (previous_forward * previous_tan_y);
    *out_ndc_x = previous_x - current_ndc_x;
    *out_ndc_y = previous_y - current_ndc_y;
    return isfinite(*out_ndc_x) && isfinite(*out_ndc_y);
}

#endif
