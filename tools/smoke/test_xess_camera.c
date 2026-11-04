#include "xess_camera.h"

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

static void pose_at(float out[16], float px)
{
    memset(out, 0, sizeof(float) * 16);
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[12] = px;
    out[15] = 1.0f;
}

static void note(kz3_xess_camera_bridge* bridge, uint64_t flip, uint64_t view, float px)
{
    float pose[16];
    pose_at(pose, px);
    kz3_xess_bridge_note_camera(bridge, flip, view, pose, 1.4f, 0.9f);
}

static void test_publication(void)
{
    kz3_xess_camera_bridge bridge;
    memset(&bridge, 0, sizeof(bridge));

    note(&bridge, 100, 1, 0.0f);
    check(
        "a camera alone is not published",
        !(bridge.state.flags & KZ3_XESS_CAMERA_CURRENT_VALID));
    kz3_xess_bridge_note_planes(&bridge, 100, 0.2f, 2048.0f);
    check(
        "camera plus planes is published, first frame reset",
        bridge.state.guest_flip == 100 &&
            (bridge.state.flags & KZ3_XESS_CAMERA_PLANES_VALID) &&
            (bridge.state.flags & KZ3_XESS_CAMERA_RESET) &&
            !(bridge.state.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID));

    kz3_xess_bridge_note_planes(&bridge, 101, 0.2f, 2048.0f);
    note(&bridge, 101, 1, 0.5f);
    check(
        "planes may arrive first; the next flip carries previous",
        bridge.state.guest_flip == 101 &&
            (bridge.state.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) &&
            !(bridge.state.flags & KZ3_XESS_CAMERA_RESET) &&
            bridge.state.previous_pose[12] == 0.0f &&
            bridge.state.current_pose[12] == 0.5f);

    note(&bridge, 102, 1, 1.0f);
    check(
        "a flip without planes leaves the last coherent state visible",
        bridge.state.guest_flip == 101);
    kz3_xess_bridge_note_planes(&bridge, 102, 0.2f, 2048.0f);
    kz3_xess_bridge_note_planes(&bridge, 102, 0.1f, 512.0f);
    check(
        "conflicting planes clear the planes flag",
        bridge.state.guest_flip == 102 &&
            !(bridge.state.flags & KZ3_XESS_CAMERA_PLANES_VALID));

    note(&bridge, 105, 1, 1.0f);
    kz3_xess_bridge_note_planes(&bridge, 105, 0.2f, 2048.0f);
    check(
        "a gap in flips has no previous and resets",
        bridge.state.guest_flip == 105 &&
            !(bridge.state.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) &&
            (bridge.state.flags & KZ3_XESS_CAMERA_RESET));

    note(&bridge, 104, 1, 9.0f);
    check("an older flip is ignored", bridge.state.current_pose[12] == 1.0f);
}

/* The flip counter is advanced by another thread, so it can move twice between two camera updates. Seen in gameplay:
 * cameras for ..., 1510, 1512, ... and nothing for 1511. The renderer asks for 1511 and must find a coherent state. */
static void test_single_skipped_flip(void)
{
    kz3_xess_camera_bridge bridge;
    kz3_xess_camera_state found;
    memset(&bridge, 0, sizeof(bridge));

    note(&bridge, 1510, 1, 0.0f);
    kz3_xess_bridge_note_planes(&bridge, 1510, 0.2f, 2048.0f);
    note(&bridge, 1512, 1, 0.5f);
    kz3_xess_bridge_note_planes(&bridge, 1512, 0.2f, 2048.0f);
    check(
        "a single skipped flip is filled as a hold of the previous camera",
        kz3_xess_bridge_find(&bridge, 1511, &found) &&
            found.guest_flip == 1511 &&
            (found.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) &&
            (found.flags & KZ3_XESS_CAMERA_PLANES_VALID) &&
            !(found.flags & KZ3_XESS_CAMERA_RESET) &&
            found.current_pose[12] == 0.0f && found.previous_pose[12] == 0.0f &&
            bridge.filled_flips == 1);
    check(
        "the flip after it continues from the held camera without a reset",
        kz3_xess_bridge_find(&bridge, 1512, &found) &&
            (found.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) &&
            !(found.flags & KZ3_XESS_CAMERA_RESET) &&
            found.previous_pose[12] == 0.0f && found.current_pose[12] == 0.5f);

    note(&bridge, 1515, 1, 1.0f);
    kz3_xess_bridge_note_planes(&bridge, 1515, 0.2f, 2048.0f);
    check(
        "two skipped flips are not filled and reset",
        !kz3_xess_bridge_find(&bridge, 1514, &found) &&
            (bridge.state.flags & KZ3_XESS_CAMERA_RESET) && bridge.filled_flips == 1);

    note(&bridge, 1516, 2, 1.0f);
    kz3_xess_bridge_note_planes(&bridge, 1516, 0.2f, 2048.0f);
    note(&bridge, 1518, 1, 1.0f);
    kz3_xess_bridge_note_planes(&bridge, 1518, 0.2f, 2048.0f);
    check(
        "a skipped flip across a view change is not filled",
        !kz3_xess_bridge_find(&bridge, 1517, &found) && bridge.filled_flips == 1);
}

static void test_resets(void)
{
    kz3_xess_camera_bridge bridge;
    memset(&bridge, 0, sizeof(bridge));
    note(&bridge, 10, 1, 0.0f);
    kz3_xess_bridge_note_planes(&bridge, 10, 0.2f, 2048.0f);

    note(&bridge, 11, 2, 0.0f);
    kz3_xess_bridge_note_planes(&bridge, 11, 0.2f, 2048.0f);
    check(
        "a different view resets",
        (bridge.state.flags & KZ3_XESS_CAMERA_RESET) &&
            !(bridge.state.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID));

    note(&bridge, 12, 2, 6.0f);
    kz3_xess_bridge_note_planes(&bridge, 12, 0.2f, 2048.0f);
    check(
        "a move over the cut distance resets",
        (bridge.state.flags & KZ3_XESS_CAMERA_RESET) != 0);

    note(&bridge, 13, 2, 6.5f);
    kz3_xess_bridge_note_planes(&bridge, 13, 0.2f, 2048.0f);
    check(
        "a small move does not",
        !(bridge.state.flags & KZ3_XESS_CAMERA_RESET));

    note(&bridge, 13, 3, 6.5f);
    check(
        "a second view in one flip is published as multiple views",
        (bridge.state.flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS) &&
            (bridge.state.flags & KZ3_XESS_CAMERA_RESET));

    float a[16], b[16];
    pose_at(a, 0.0f);
    pose_at(b, 0.0f);
    b[4] = -1.0f;
    b[5] = 0.0f;
    b[0] = 0.0f;
    b[1] = 1.0f;
    check("a quarter turn is a cut", kz3_xess_camera_cut(a, b));
    pose_at(b, 0.1f);
    check("no rotation and a short move is not", !kz3_xess_camera_cut(a, b));
}

static void test_ring(void)
{
    kz3_xess_camera_bridge bridge;
    kz3_xess_camera_state found;
    memset(&bridge, 0, sizeof(bridge));
    for (uint64_t flip = 200; flip < 200 + KZ3_XESS_CAMERA_RING + 2u; ++flip) {
        note(&bridge, flip, 1, (float)(flip - 200) * 0.01f);
        kz3_xess_bridge_note_planes(&bridge, flip, 0.2f, 2048.0f);
    }
    const uint64_t last = 200 + KZ3_XESS_CAMERA_RING + 1u;
    check(
        "the ring returns the exact recent flip",
        kz3_xess_bridge_find(&bridge, last - 3u, &found) &&
            found.guest_flip == last - 3u);
    check(
        "an overwritten flip is not returned",
        !kz3_xess_bridge_find(&bridge, 200, &found));
    check(
        "a flip not yet published is not returned",
        !kz3_xess_bridge_find(&bridge, last + 1u, &found));
}

static void test_pairing(void)
{
    kz3_xess_pairing pairing;

    memset(&pairing, 0, sizeof(pairing));
    check(
        "in step: the frame's own flip",
        kz3_xess_pair_flip(&pairing, 500, 500, 0) == 500);
    check(
        "the PPU one ahead: still the frame's own flip",
        kz3_xess_pair_flip(&pairing, 502, 501, 0) == 501);
    check(
        "ahead by two: still the frame's own flip",
        kz3_xess_pair_flip(&pairing, 504, 502, 0) == 502);

    memset(&pairing, 0, sizeof(pairing));
    (void)kz3_xess_pair_flip(&pairing, 1040, 1000, 0);
    check(
        "the counters need not share a base: the offset is measured",
        kz3_xess_pair_flip(&pairing, 1042, 1001, 0) == 1041);

    memset(&pairing, 0, sizeof(pairing));
    uint64_t want = 0;
    for (uint64_t consumed = 700; consumed < 720; ++consumed) {
        want = kz3_xess_pair_flip(&pairing, consumed + 1u, consumed, 0);
    }
    check(
        "ahead on every frame: the latest flip, no worse than before",
        want == 720);

    memset(&pairing, 0, sizeof(pairing));
    check(
        "lag steps back by whole flips",
        kz3_xess_pair_flip(&pairing, 300, 300, 1) == 299);

    memset(&pairing, 0, sizeof(pairing));
    check(
        "never below flip zero",
        kz3_xess_pair_flip(&pairing, 0, 0, 3) == 0);

    memset(&pairing, 0, sizeof(pairing));
    (void)kz3_xess_pair_flip(&pairing, 100, 110, 0);
    for (uint64_t frame = 0; frame < 2u * KZ3_XESS_PAIRING_WINDOW; ++frame) {
        want = kz3_xess_pair_flip(&pairing, 120 + frame, 120 + frame, 0);
    }
    check(
        "an old low offset ages out of the window",
        want == 120 + 2u * KZ3_XESS_PAIRING_WINDOW - 1u);
}

static void test_steps(void)
{
    check(
        "no last dispatch is not a jump",
        kz3_xess_camera_step_from(0, 0, 900) == KZ3_XESS_CAMERA_STEP_NEXT);
    check(
        "one flip on is normal",
        kz3_xess_camera_step_from(1, 50, 51) == KZ3_XESS_CAMERA_STEP_NEXT);
    check(
        "the same flip repeats",
        kz3_xess_camera_step_from(1, 50, 50) == KZ3_XESS_CAMERA_STEP_REPEAT);
    check(
        "an older flip repeats",
        kz3_xess_camera_step_from(1, 50, 49) == KZ3_XESS_CAMERA_STEP_REPEAT);
    check(
        "a skipped flip jumps",
        kz3_xess_camera_step_from(1, 50, 52) == KZ3_XESS_CAMERA_STEP_JUMP);

    kz3_xess_camera_state state;
    memset(&state, 0, sizeof(state));
    pose_at(state.current_pose, 2.0f);
    pose_at(state.previous_pose, 1.0f);
    state.current_horizontal_fov = 1.4f;
    state.previous_horizontal_fov = 1.2f;
    state.flags = KZ3_XESS_CAMERA_CURRENT_VALID | KZ3_XESS_CAMERA_RESET;
    kz3_xess_camera_hold(&state);
    check(
        "a held camera has no motion and asks for no reset",
        state.previous_pose[12] == 2.0f &&
            state.previous_horizontal_fov == 1.4f &&
            (state.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) &&
            !(state.flags & KZ3_XESS_CAMERA_RESET));
}

static void test_pose(void)
{
    float pose[16];
    pose_at(pose, 0.0f);
    check("a unit basis is valid", kz3_xess_pose_valid(pose));
    check(
        "right x forward = up has handedness +1",
        fabsf(kz3_xess_pose_handedness(pose) - 1.0f) < 1e-6f);
    pose[0] = -1.0f;
    check(
        "a mirrored basis has handedness -1 and still passes pose_valid",
        kz3_xess_pose_valid(pose) &&
            fabsf(kz3_xess_pose_handedness(pose) + 1.0f) < 1e-6f);
    pose[5] = 2.0f;
    check("a scaled row is invalid", !kz3_xess_pose_valid(pose));
    pose[5] = NAN;
    check("a NaN is invalid", !kz3_xess_pose_valid(pose));
}

int main(void)
{
    test_publication();
    test_single_skipped_flip();
    test_resets();
    test_ring();
    test_pairing();
    test_steps();
    test_pose();
    printf("[test_xess_camera] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
