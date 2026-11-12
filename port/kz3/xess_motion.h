#ifndef KZ3_XESS_MOTION_H
#define KZ3_XESS_MOTION_H

#include <math.h>

/*
 * ENHANCEMENT: depth-aware camera reprojection for KZ3's row-major pose:
 * row 0 = right, row 1 = forward, row 2 = up, row 3 = position.
 * ASSUMPTION: that axis naming is the adapter's reading of the pose, not recovered from the binary. xess.cpp logs the
 * basis handedness once so a run can confirm it.
 */
static inline float kz3_xess_dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline int kz3_xess_view_depth(
    float depth,
    float near_plane,
    float far_plane,
    float* out_view_depth)
{
    if (!out_view_depth || !isfinite(depth) ||
        depth < 0.0f || depth > 1.0f ||
        !isfinite(near_plane) || !isfinite(far_plane) ||
        !(near_plane > 0.0f) || !(far_plane > near_plane)) {
        return 0;
    }
    const float denominator =
        far_plane - depth * (far_plane - near_plane);
    if (!(denominator > 0.0f)) {
        return 0;
    }
    *out_view_depth = near_plane * far_plane / denominator;
    return isfinite(*out_view_depth) && *out_view_depth > 0.0f;
}

static inline int kz3_xess_camera_motion(
    const float previous_pose[16],
    float previous_horizontal_fov,
    float previous_vertical_fov,
    const float current_pose[16],
    float current_horizontal_fov,
    float current_vertical_fov,
    float depth,
    float near_plane,
    float far_plane,
    float current_ndc_x,
    float current_ndc_y,
    float* out_ndc_x,
    float* out_ndc_y)
{
    float view_depth;
    if (!previous_pose || !current_pose || !out_ndc_x || !out_ndc_y ||
        !kz3_xess_view_depth(
            depth, near_plane, far_plane, &view_depth)) {
        return 0;
    }
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
    float previous_relative[3];
    for (int axis = 0; axis < 3; ++axis) {
        const float world =
            current_pose[12 + axis] + ray[axis] * view_depth;
        previous_relative[axis] = world - previous_pose[12 + axis];
    }
    const float previous_forward =
        kz3_xess_dot3(previous_relative, previous_pose + 4);
    if (!(previous_forward > 1e-5f)) {
        return 0;
    }

    const float previous_x =
        kz3_xess_dot3(previous_relative, previous_pose) /
        (previous_forward * previous_tan_x);
    const float previous_y =
        kz3_xess_dot3(previous_relative, previous_pose + 8) /
        (previous_forward * previous_tan_y);
    *out_ndc_x = previous_x - current_ndc_x;
    *out_ndc_y = previous_y - current_ndc_y;
    return isfinite(*out_ndc_x) && isfinite(*out_ndc_y);
}

#endif
