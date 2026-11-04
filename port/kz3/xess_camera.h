#ifndef KZ3_XESS_CAMERA_H
#define KZ3_XESS_CAMERA_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/*
 * ENHANCEMENT: the camera side of the XeSS-SR path (the PS3 game has no temporal upscaler). No Windows dependency, so
 * tools/smoke/test_xess_camera.c runs it on any host. The caller supplies the locking (port/kz3/xess.cpp).
 *
 * A pose is KZ3's row-major camera matrix: row 0 = right, row 1 = forward, row 2 = up, row 3 = position.
 */
enum kz3_xess_camera_flags {
    KZ3_XESS_CAMERA_CURRENT_VALID = 1u << 0,
    KZ3_XESS_CAMERA_PREVIOUS_VALID = 1u << 1,
    KZ3_XESS_CAMERA_PLANES_VALID = 1u << 2,
    KZ3_XESS_CAMERA_RESET = 1u << 3,
    KZ3_XESS_CAMERA_MULTIPLE_VIEWS = 1u << 4,
};

typedef struct kz3_xess_camera_state {
    uint64_t guest_flip;
    uint64_t view_id;
    float current_pose[16];
    float previous_pose[16];
    float current_horizontal_fov;
    float current_vertical_fov;
    float previous_horizontal_fov;
    float previous_vertical_fov;
    float near_plane;
    float far_plane;
    uint32_t flags;
} kz3_xess_camera_state;

/* Published states kept by guest flip, so the renderer can ask for the flip it is replaying. */
#define KZ3_XESS_CAMERA_RING 16u

typedef struct kz3_xess_camera_bridge {
    kz3_xess_camera_state state;
    kz3_xess_camera_state pending;
    kz3_xess_camera_state ring[KZ3_XESS_CAMERA_RING];
    uint64_t plane_flip;
    float near_plane;
    float far_plane;
    int have_planes;
    int planes_ambiguous;
} kz3_xess_camera_bridge;

/* ASSUMPTION: experimental adapter policy, not constants recovered from the game. */
#define KZ3_XESS_CUT_DISTANCE_SQUARED 25.0f
#define KZ3_XESS_CUT_ANGLE 1.5f

/* Frames of (latest published flip - consumed flip) kept when looking for the smallest offset. */
#define KZ3_XESS_PAIRING_WINDOW 120u

static inline int kz3_xess_pose_valid(const float pose[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        if (!isfinite(pose[i])) {
            return 0;
        }
    }
    for (unsigned row = 0; row < 3; ++row) {
        const float* v = pose + row * 4;
        const float length_squared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (!(length_squared > 0.81f && length_squared < 1.21f)) {
            return 0;
        }
    }
    return 1;
}

/* dot(cross(right, forward), up): +1 for the basis the reprojection and its host test use. Logged once, because a
 * wrong axis would still pass pose_valid and only show as bad motion. */
static inline float kz3_xess_pose_handedness(const float pose[16])
{
    const float* r = pose;
    const float* f = pose + 4;
    const float* u = pose + 8;
    return (r[1] * f[2] - r[2] * f[1]) * u[0] +
           (r[2] * f[0] - r[0] * f[2]) * u[1] +
           (r[0] * f[1] - r[1] * f[0]) * u[2];
}

static inline int kz3_xess_camera_cut(const float previous[16], const float current[16])
{
    const float dx = current[12] - previous[12];
    const float dy = current[13] - previous[13];
    const float dz = current[14] - previous[14];
    if (dx * dx + dy * dy + dz * dz > KZ3_XESS_CUT_DISTANCE_SQUARED) {
        return 1;
    }
    float forward_dot =
        previous[4] * current[4] + previous[5] * current[5] + previous[6] * current[6];
    if (forward_dot < -1.0f) {
        forward_dot = -1.0f;
    }
    if (forward_dot > 1.0f) {
        forward_dot = 1.0f;
    }
    return forward_dot < cosf(KZ3_XESS_CUT_ANGLE);
}

static inline int kz3_xess_plane_matches(float left, float right)
{
    const float a = fabsf(left);
    const float b = fabsf(right);
    float scale = a > b ? a : b;
    if (scale < 1.0f) {
        scale = 1.0f;
    }
    return fabsf(left - right) <= scale * 1.0e-4f;
}

static inline void kz3_xess_bridge_publish(kz3_xess_camera_bridge* bridge, int force)
{
    const uint32_t ready =
        KZ3_XESS_CAMERA_CURRENT_VALID | KZ3_XESS_CAMERA_PLANES_VALID;
    if ((bridge->pending.flags & ready) == ready ||
        (force && (bridge->pending.flags & KZ3_XESS_CAMERA_CURRENT_VALID))) {
        bridge->state = bridge->pending;
        bridge->ring[bridge->pending.guest_flip % KZ3_XESS_CAMERA_RING] =
            bridge->pending;
    }
}

static inline void kz3_xess_bridge_note_camera(
    kz3_xess_camera_bridge* bridge,
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov)
{
    kz3_xess_camera_state* pending = &bridge->pending;
    if ((pending->flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        guest_flip < pending->guest_flip) {
        return;
    }
    if ((pending->flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        pending->guest_flip == guest_flip) {
        if (pending->view_id != view_id) {
            pending->flags |=
                KZ3_XESS_CAMERA_MULTIPLE_VIEWS | KZ3_XESS_CAMERA_RESET;
            kz3_xess_bridge_publish(bridge, 1);
            return;
        }
        memcpy(pending->current_pose, pose, sizeof(pending->current_pose));
        pending->current_horizontal_fov = horizontal_fov;
        pending->current_vertical_fov = vertical_fov;
        kz3_xess_bridge_publish(bridge, 0);
        return;
    }

    kz3_xess_camera_state next;
    memset(&next, 0, sizeof(next));
    next.guest_flip = guest_flip;
    next.view_id = view_id;
    memcpy(next.current_pose, pose, sizeof(next.current_pose));
    next.current_horizontal_fov = horizontal_fov;
    next.current_vertical_fov = vertical_fov;
    next.flags = KZ3_XESS_CAMERA_CURRENT_VALID | KZ3_XESS_CAMERA_RESET;

    const kz3_xess_camera_state* state = &bridge->state;
    if ((state->flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        state->guest_flip + 1u == guest_flip &&
        state->view_id == view_id &&
        !(state->flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS)) {
        memcpy(
            next.previous_pose,
            state->current_pose,
            sizeof(next.previous_pose));
        next.previous_horizontal_fov = state->current_horizontal_fov;
        next.previous_vertical_fov = state->current_vertical_fov;
        next.flags |= KZ3_XESS_CAMERA_PREVIOUS_VALID;
        if (!kz3_xess_camera_cut(next.previous_pose, next.current_pose)) {
            next.flags &= ~(uint32_t)KZ3_XESS_CAMERA_RESET;
        }
    }

    if (bridge->have_planes && bridge->plane_flip == guest_flip &&
        !bridge->planes_ambiguous) {
        next.near_plane = bridge->near_plane;
        next.far_plane = bridge->far_plane;
        next.flags |= KZ3_XESS_CAMERA_PLANES_VALID;
    }
    *pending = next;
    kz3_xess_bridge_publish(bridge, 0);
}

static inline void kz3_xess_bridge_note_planes(
    kz3_xess_camera_bridge* bridge,
    uint64_t guest_flip,
    float near_plane,
    float far_plane)
{
    if (bridge->have_planes && guest_flip < bridge->plane_flip) {
        return;
    }
    if (!bridge->have_planes || bridge->plane_flip != guest_flip) {
        bridge->have_planes = 1;
        bridge->plane_flip = guest_flip;
        bridge->near_plane = near_plane;
        bridge->far_plane = far_plane;
        bridge->planes_ambiguous = 0;
    } else if (!kz3_xess_plane_matches(bridge->near_plane, near_plane) ||
               !kz3_xess_plane_matches(bridge->far_plane, far_plane)) {
        bridge->planes_ambiguous = 1;
    }
    kz3_xess_camera_state* pending = &bridge->pending;
    if ((pending->flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        pending->guest_flip == guest_flip) {
        if (bridge->planes_ambiguous) {
            pending->flags &= ~(uint32_t)KZ3_XESS_CAMERA_PLANES_VALID;
        } else {
            pending->near_plane = bridge->near_plane;
            pending->far_plane = bridge->far_plane;
            pending->flags |= KZ3_XESS_CAMERA_PLANES_VALID;
        }
        kz3_xess_bridge_publish(bridge, bridge->planes_ambiguous);
    }
}

static inline int kz3_xess_bridge_find(
    const kz3_xess_camera_bridge* bridge,
    uint64_t guest_flip,
    kz3_xess_camera_state* out_state)
{
    const kz3_xess_camera_state* state =
        &bridge->ring[guest_flip % KZ3_XESS_CAMERA_RING];
    if (!(state->flags & KZ3_XESS_CAMERA_CURRENT_VALID) ||
        state->guest_flip != guest_flip) {
        return 0;
    }
    *out_state = *state;
    return 1;
}

/*
 * Which published flip belongs to the renderer frame being built. The PPU counts flip requests and the renderer counts
 * completed presentations. Their difference is constant for a given frame relationship, and is not assumed to be zero.
 * A frame's own camera is published before its draws are built, and the PPU may have moved on by the time the primary
 * draw is reached, so (latest - renderer frame) is never below the true offset: its smallest recent value is that
 * offset. If the PPU is ahead on every frame of the window this returns the latest flip, which is no worse than taking
 * the latest state. lag moves the answer further back by whole flips (KZ3_XESS_CAMERA_LAG).
 */
typedef struct kz3_xess_pairing {
    int valid;
    uint32_t age;
    int64_t window_min;
    int64_t previous_min;
} kz3_xess_pairing;

static inline uint64_t kz3_xess_pair_flip(
    kz3_xess_pairing* pairing,
    uint64_t latest_flip,
    uint64_t renderer_frame,
    uint32_t lag)
{
    const int64_t offset = (int64_t)latest_flip - (int64_t)renderer_frame;
    if (!pairing->valid) {
        pairing->valid = 1;
        pairing->age = 0;
        pairing->window_min = offset;
        pairing->previous_min = offset;
    }
    if (offset < pairing->window_min) {
        pairing->window_min = offset;
    }
    const int64_t best = pairing->window_min < pairing->previous_min ?
        pairing->window_min : pairing->previous_min;
    if (++pairing->age >= KZ3_XESS_PAIRING_WINDOW) {
        pairing->age = 0;
        pairing->previous_min = pairing->window_min;
        pairing->window_min = offset;
    }
    const int64_t want = (int64_t)renderer_frame + best - (int64_t)lag;
    return want > 0 ? (uint64_t)want : 0u;
}

enum kz3_xess_camera_step {
    KZ3_XESS_CAMERA_STEP_NEXT = 0,
    KZ3_XESS_CAMERA_STEP_REPEAT = 1,
    KZ3_XESS_CAMERA_STEP_JUMP = 2,
};

/* How a camera relates to the one used by the last dispatch. */
static inline enum kz3_xess_camera_step kz3_xess_camera_step_from(
    int have_last,
    uint64_t last_flip,
    uint64_t guest_flip)
{
    if (!have_last || guest_flip == last_flip + 1u) {
        return KZ3_XESS_CAMERA_STEP_NEXT;
    }
    return guest_flip <= last_flip ? KZ3_XESS_CAMERA_STEP_REPEAT :
                                     KZ3_XESS_CAMERA_STEP_JUMP;
}

/* The same camera again: nothing moved since the last dispatch, whatever delta the state carries. */
static inline void kz3_xess_camera_hold(kz3_xess_camera_state* state)
{
    memcpy(
        state->previous_pose,
        state->current_pose,
        sizeof(state->previous_pose));
    state->previous_horizontal_fov = state->current_horizontal_fov;
    state->previous_vertical_fov = state->current_vertical_fov;
    state->flags |= KZ3_XESS_CAMERA_PREVIOUS_VALID;
    state->flags &= ~(uint32_t)KZ3_XESS_CAMERA_RESET;
}

#endif
