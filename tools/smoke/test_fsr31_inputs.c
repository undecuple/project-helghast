#include "fsr31_inputs.h"

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

int main(void)
{
    char sharpness[16];
    check(
        "sharpness accepts 0.0 through 1.0 and rounds to tenths",
        kz3_fsr31_parse_sharpness("0") == 0 &&
        kz3_fsr31_parse_sharpness(" 0.2 ") == 200 &&
        kz3_fsr31_parse_sharpness("0.25") == 300 &&
        kz3_fsr31_parse_sharpness("1.000") == 1000);
    check(
        "sharpness rejects malformed and out-of-range values",
        kz3_fsr31_parse_sharpness("1.01") < 0 &&
        kz3_fsr31_parse_sharpness("-0.1") < 0 &&
        kz3_fsr31_parse_sharpness("nan") < 0 &&
        kz3_fsr31_parse_sharpness("0.") < 0 &&
        kz3_fsr31_parse_sharpness(NULL) < 0);
    check(
        "sharpness names use the rounded portable representation",
        !strcmp(
            kz3_fsr31_sharpness_name(250, sharpness, sizeof(sharpness)),
            "0.3") &&
        !strcmp(
            kz3_fsr31_sharpness_name(1001, sharpness, sizeof(sharpness)),
            "1.0"));

    kz3_fsr31_draw_info draw;
    memset(&draw, 0, sizeof(draw));
    draw.frame = 42;
    draw.target_offset = 0x02200000u;
    draw.target_width = 1280;
    draw.target_height = 720;
    draw.mrt_count = 4;
    draw.depth_slot = 7;
    draw.target_is_local = 1;
    draw.full_viewport = 1;
    draw.depth_test = 1;

    check(
        "full-resolution four-target depth pass selects deferred depth",
        kz3_fsr31_is_deferred_primary(&draw, 1280, 720));
    draw.mrt_count = 1;
    check(
        "single-target depth pass is not the deferred primary",
        !kz3_fsr31_is_deferred_primary(&draw, 1280, 720));

    draw.target_offset = KZ3_FSR31_SCENE_TARGET_OFFSET;
    check(
        "depth-tested scene draw reuses deferred jitter",
        kz3_fsr31_should_jitter(&draw, 1280, 720, 42, 7));
    check(
        "scene draw rejects a stale deferred-depth frame",
        !kz3_fsr31_should_jitter(&draw, 1280, 720, 41, 7));

    draw.depth_test = 0;
    draw.blend = 0;
    draw.sampled_full_size_render_target = 1;
    draw.sampled_render_target_offset =
        KZ3_FSR31_DEFERRED_LIGHTING_OFFSET;
    check(
        "validated lighting resolve selects opaque capture",
        kz3_fsr31_is_opaque_resolve(&draw, 1280, 720));
    draw.blend = 1;
    check(
        "blended scene draw cannot replace opaque capture",
        !kz3_fsr31_is_opaque_resolve(&draw, 1280, 720));

    kz3_fsr31_capture_key opaque = {
        42, 3, 9, 1280, 720, (uintptr_t)0x1234u};
    kz3_fsr31_capture_key pre = opaque;
    check(
        "same-frame scene captures form a reactive pair",
        kz3_fsr31_capture_pair_matches(&opaque, &pre));
    pre.frame++;
    check(
        "capture pair rejects a different frame",
        !kz3_fsr31_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.resource++;
    check(
        "capture pair rejects a different resource",
        !kz3_fsr31_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.context_generation++;
    check(
        "capture pair rejects a recreated FSR context",
        !kz3_fsr31_capture_pair_matches(&opaque, &pre));

    printf("[test_fsr31_inputs] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
