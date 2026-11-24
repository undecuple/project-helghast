#include "xess_inputs.h"

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
    kz3_xess_draw_info draw;
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

    check("input delay applies after the previous flip", kz3_xess_inputs_delay_applies(1, 1, 100, 101));
    check("input delay applies to a held camera (same flip)", kz3_xess_inputs_delay_applies(1, 1, 100, 100));
    check("input delay does not span a skipped dispatch", !kz3_xess_inputs_delay_applies(1, 1, 100, 102));
    check("input delay does not go backwards", !kz3_xess_inputs_delay_applies(1, 1, 100, 99));
    check("input delay needs a previous dispatch", !kz3_xess_inputs_delay_applies(1, 0, 100, 101));
    check("KZ3_XESS_INPUT_DELAY=0 turns it off", !kz3_xess_inputs_delay_applies(0, 1, 100, 101));

    check(
        "full-resolution four-target depth pass selects deferred depth",
        kz3_xess_is_deferred_primary(&draw, 1280, 720));
    draw.mrt_count = 1;
    check(
        "single-target depth pass is not the deferred primary",
        !kz3_xess_is_deferred_primary(&draw, 1280, 720));

    draw.target_offset = KZ3_XESS_SCENE_TARGET_OFFSET;
    check(
        "depth-tested scene draw reuses deferred jitter",
        kz3_xess_should_jitter(&draw, 1280, 720, 42, 7));
    check(
        "scene draw rejects a stale deferred-depth frame",
        !kz3_xess_should_jitter(&draw, 1280, 720, 41, 7));

    draw.depth_test = 0;
    draw.blend = 0;
    draw.sampled_full_size_render_target = 1;
    draw.sampled_render_target_offset =
        KZ3_XESS_DEFERRED_LIGHTING_OFFSET;
    check(
        "validated lighting resolve selects opaque capture",
        kz3_xess_is_opaque_resolve(&draw, 1280, 720));
    draw.blend = 1;
    check(
        "blended scene draw cannot replace opaque capture",
        !kz3_xess_is_opaque_resolve(&draw, 1280, 720));

    kz3_xess_capture_key opaque = {
        42, 3, 9, 1280, 720, (uintptr_t)0x1234u};
    kz3_xess_capture_key pre = opaque;
    check(
        "same-frame scene captures form a responsive pair",
        kz3_xess_capture_pair_matches(&opaque, &pre));
    pre.frame++;
    check(
        "capture pair rejects a different frame",
        !kz3_xess_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.resource++;
    check(
        "capture pair rejects a different resource",
        !kz3_xess_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.context_generation++;
    check(
        "capture pair rejects a recreated XeSS context",
        !kz3_xess_capture_pair_matches(&opaque, &pre));

    check(
        "exact 16:9 outputs match",
        kz3_xess_aspect_matches(1280, 720, 1920, 1080) &&
            kz3_xess_aspect_matches(1280, 720, 3840, 2160));
    check(
        "one output pixel of rounding matches",
        kz3_xess_aspect_matches(1280, 720, 1366, 768) &&
            kz3_xess_aspect_matches(1280, 720, 1778, 1000) &&
            kz3_xess_aspect_matches(1280, 720, 1921, 1080) &&
            kz3_xess_aspect_matches(1280, 720, 1920, 1081));
    check(
        "a real shape change does not",
        !kz3_xess_aspect_matches(1280, 720, 1920, 1200) &&
            !kz3_xess_aspect_matches(1280, 720, 2560, 1080) &&
            !kz3_xess_aspect_matches(1280, 720, 1360, 768));

    check(
        "mip bias is log2(input / target) in 4.8 fixed point",
        kz3_xess_mip_bias_fp(1280, 2560) == -256 &&
            kz3_xess_mip_bias_fp(1280, 1920) == -150 &&
            kz3_xess_mip_bias_fp(1280, 3840) == -406 &&
            kz3_xess_mip_bias_fp(1280, 1280) == 0 &&
            kz3_xess_mip_bias_fp(0, 1920) == 0);
    check(
        "bias adds to the guest's and keeps the rest of FILTER",
        kz3_xess_filter_with_bias(0x05060100u, -256) == 0x05060000u &&
            kz3_xess_filter_with_bias(0x05060000u, -150) ==
                (0x05060000u | (0x2000u - 150u)));
    check(
        "a negative guest bias stays signed",
        kz3_xess_filter_with_bias(0x1F00u, -256) == 0x1E00u);
    check(
        "bias saturates at the field's range",
        kz3_xess_filter_with_bias(0x1000u, -256) == 0x1000u &&
            kz3_xess_filter_with_bias(0x0FFFu, 256) == 0x0FFFu);

    printf("[test_xess_inputs] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
