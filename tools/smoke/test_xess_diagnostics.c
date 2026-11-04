#include "xess.h"
#include "xess_diagnostics.h"

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

static void test_configuration(void)
{
    kz3_xess_diagnostic_config config = kz3_xess_diagnostic_defaults();
    float scale = 17.0f;
    uint32_t interval = 17u;

    check("default settings do not modify jitter",
          !kz3_xess_diagnostic_modified(&config));
    check("finite scale accepts bounds and trailing space",
          kz3_xess_diagnostic_parse_scale(" -2 \t", &scale) &&
          scale == -2.0f &&
          kz3_xess_diagnostic_parse_scale("2", &scale) && scale == 2.0f);
    check("invalid scale is rejected without changing its destination",
          !kz3_xess_diagnostic_parse_scale("", &scale) &&
          !kz3_xess_diagnostic_parse_scale("nan", &scale) &&
          !kz3_xess_diagnostic_parse_scale("inf", &scale) &&
          !kz3_xess_diagnostic_parse_scale("1e1000", &scale) &&
          !kz3_xess_diagnostic_parse_scale("-2.01", &scale) &&
          !kz3_xess_diagnostic_parse_scale("1.0x", &scale) &&
          scale == 2.0f);
    check("trace interval accepts zero and its maximum",
          kz3_xess_diagnostic_parse_interval("0", &interval) &&
          interval == 0 &&
          kz3_xess_diagnostic_parse_interval("60000 \t", &interval) &&
          interval == 60000);
    check("invalid trace interval is rejected",
          !kz3_xess_diagnostic_parse_interval("", &interval) &&
          !kz3_xess_diagnostic_parse_interval("-1", &interval) &&
          !kz3_xess_diagnostic_parse_interval("60001", &interval) &&
          !kz3_xess_diagnostic_parse_interval("1frame", &interval) &&
          interval == 60000);
    config.projection_scale_x = 0.0f;
    check("a changed projection scale enables diagnostic behavior",
          kz3_xess_diagnostic_modified(&config));
    config = kz3_xess_diagnostic_defaults();
    config.trace_interval = 1u;
    check("logging alone does not modify jitter",
          !kz3_xess_diagnostic_modified(&config));
}

static void test_samples(void)
{
    kz3_xess_diagnostic_config config = kz3_xess_diagnostic_defaults();
    kz3_xess_diagnostic_history history = {0};
    kz3_xess_diagnostic_sample sample =
        kz3_xess_diagnostic_next_sample(
            &config, &history, 42, 0.25f, -0.5f, 1);
    check("defaults preserve both Halton offsets",
          sample.active && !sample.lag_applied &&
          sample.projection_x == 0.25f && sample.projection_y == -0.5f &&
          sample.execute_x == 0.25f && sample.execute_y == -0.5f);

    int exact_defaults = 1;
    config.trace_interval = 1;
    for (int i = 0; i < 5000; ++i) {
        const float x = i == 0 ? -0.0f : (float)((i % 97) - 48) / 97.0f;
        const float y = (float)((i % 89) - 44) / 89.0f;
        sample = kz3_xess_diagnostic_next_sample(
            &config, &history, (uint32_t)i, x, y, 1);
        exact_defaults &= memcmp(&x, &sample.projection_x, sizeof x) == 0 &&
                          memcmp(&y, &sample.projection_y, sizeof y) == 0 &&
                          memcmp(&x, &sample.execute_x, sizeof x) == 0 &&
                          memcmp(&y, &sample.execute_y, sizeof y) == 0;
    }
    check("trace-only defaults preserve 5000 Halton samples bit for bit", exact_defaults);

    config.projection_scale_x = -1.0f;
    config.projection_scale_y = 0.0f;
    config.execute_scale_x = 0.0f;
    config.execute_scale_y = -1.0f;
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 43, 0.25f, -0.5f, 1);
    check("projection and execute axes can be zeroed and reversed independently",
          sample.projection_x == -0.25f && sample.projection_y == 0.0f &&
          sample.execute_x == 0.0f && sample.execute_y == 0.5f);

    config = kz3_xess_diagnostic_defaults();
    config.two_phase = 1;
    config.execute_lag = 1;
    history.valid = 0;
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 50, 0.1f, 0.2f, 1);
    check("two-phase lag starts with the current sample",
          sample.nominal_x == 0.25f && sample.nominal_y == 0.0f &&
          sample.execute_x == 0.25f && !sample.lag_applied);
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 51, 0.1f, 0.2f, 1);
    check("lag uses the previous contiguous nominal sample",
          sample.projection_x == -0.25f && sample.execute_x == 0.25f &&
          sample.lag_applied);
    kz3_xess_diagnostic_clear_lag(&config, &sample);
    check("a mid-frame reset replaces lag with the current sample",
          sample.execute_x == -0.25f && !sample.lag_applied);
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 53, 0.1f, 0.2f, 1);
    check("a frame gap prevents stale lag",
          sample.execute_x == -0.25f && !sample.lag_applied);
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 54, 0.1f, 0.2f, 0);
    check("a hold clears the sample and history",
          !sample.active && !history.valid &&
          sample.projection_x == 0.0f && sample.execute_x == 0.0f);
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 55, 0.1f, 0.2f, 1);
    check("the first frame after a hold uses its current sample",
          sample.execute_x == -0.25f && !sample.lag_applied);
    history.valid = 0;
    sample = kz3_xess_diagnostic_next_sample(
        &config, &history, 56, 0.1f, 0.2f, 1);
    check("context recreation can invalidate lag history",
          sample.execute_x == 0.25f && !sample.lag_applied);

    kz3_xess_camera_dispatch_desc dispatch = {0};
    dispatch.jitter_x = 0.125f;
    dispatch.jitter_y = -0.25f;
    dispatch.execute_jitter_x = -0.5f;
    dispatch.execute_jitter_y = 0.5f;
    kz3_xess_jitter_offset execute = kz3_xess_execute_jitter(&dispatch);
    check("zero-initialized callers retain projection jitter at execute",
          execute.x == 0.125f && execute.y == -0.25f);
    dispatch.use_execute_jitter_override = 1;
    execute = kz3_xess_execute_jitter(&dispatch);
    check("execute override leaves projection jitter independent",
          execute.x == -0.5f && execute.y == 0.5f &&
          dispatch.jitter_x == 0.125f && dispatch.jitter_y == -0.25f);
}

static void test_coverage(void)
{
    kz3_xess_diagnostic_coverage coverage = {0};
    kz3_xess_draw_info draw;
    memset(&draw, 0, sizeof draw);
    draw.frame = 42;
    draw.target_width = 1280;
    draw.target_height = 720;
    draw.target_is_local = 1;
    draw.full_viewport = 1;
    draw.depth_test = 1;
    draw.depth_slot = 7;
    draw.mrt_count = 4;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 1, 1);
    draw.mrt_count = 1;
    draw.target_offset = KZ3_XESS_SCENE_TARGET_OFFSET;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 1, 1);
    draw.full_viewport = 0;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);
    draw.full_viewport = 1;
    draw.depth_test = 0;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);
    draw.depth_test = 1;
    draw.depth_slot = 8;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);
    draw.depth_slot = 7;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 1);
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);
    draw.target_offset = 0;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);

    check("target categories account for every evaluated draw",
          coverage.draws == 8 && coverage.deferred == 1 &&
          coverage.scene == 6 && coverage.other == 1);
    check("candidate outcomes account for every deferred and scene draw",
          coverage.shifted == 2 && coverage.partial_viewport == 1 &&
          coverage.depth_disabled == 1 && coverage.depth_mismatch == 1 &&
          coverage.zero_projection == 1 && coverage.unavailable == 1);

    memset(&coverage, 0, sizeof coverage);
    draw.target_offset = KZ3_XESS_SCENE_TARGET_OFFSET;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 41, 7, 0, 0);
    draw.mrt_count = 4;
    draw.depth_slot = 0;
    kz3_xess_diagnostic_count_draw(&coverage, &draw, 1280, 720, 42, 7, 0, 0);
    check("stale and absent primary depth are counted as mismatches",
          coverage.depth_mismatch == 2);
}

int main(void)
{
    test_configuration();
    test_samples();
    test_coverage();
    printf("[test_xess_diagnostics] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
