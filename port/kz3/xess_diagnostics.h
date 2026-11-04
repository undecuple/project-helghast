#ifndef KZ3_XESS_DIAGNOSTICS_H
#define KZ3_XESS_DIAGNOSTICS_H

#include "xess_inputs.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct kz3_xess_diagnostic_config {
    float projection_scale_x;
    float projection_scale_y;
    float execute_scale_x;
    float execute_scale_y;
    uint32_t two_phase;
    uint32_t execute_lag;
    uint32_t trace_interval;
} kz3_xess_diagnostic_config;

typedef struct kz3_xess_diagnostic_history {
    uint32_t frame;
    float nominal_x;
    float nominal_y;
    int valid;
} kz3_xess_diagnostic_history;

typedef struct kz3_xess_diagnostic_sample {
    float nominal_x;
    float nominal_y;
    float projection_x;
    float projection_y;
    float execute_x;
    float execute_y;
    int active;
    int lag_applied;
} kz3_xess_diagnostic_sample;

typedef struct kz3_xess_diagnostic_coverage {
    uint32_t draws;
    uint32_t deferred;
    uint32_t scene;
    uint32_t other;
    uint32_t shifted;
    uint32_t partial_viewport;
    uint32_t depth_disabled;
    uint32_t depth_mismatch;
    uint32_t zero_projection;
    uint32_t unavailable;
} kz3_xess_diagnostic_coverage;

static inline kz3_xess_diagnostic_config kz3_xess_diagnostic_defaults(void)
{
    const kz3_xess_diagnostic_config config = {
        1.0f, 1.0f, 1.0f, 1.0f, 0u, 0u, 0u
    };
    return config;
}

static inline int kz3_xess_diagnostic_modified(
    const kz3_xess_diagnostic_config* config)
{
    return config->two_phase || config->execute_lag ||
           config->projection_scale_x != 1.0f ||
           config->projection_scale_y != 1.0f ||
           config->execute_scale_x != 1.0f ||
           config->execute_scale_y != 1.0f;
}

static inline int kz3_xess_diagnostic_parse_scale(
    const char* text, float* value)
{
    if (!text || !*text || !value) {
        return 0;
    }
    char* end;
    errno = 0;
    const float parsed = strtof(text, &end);
    if (end == text || errno == ERANGE || !isfinite(parsed) ||
        parsed < -2.0f || parsed > 2.0f) {
        return 0;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (*end) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static inline int kz3_xess_diagnostic_parse_interval(
    const char* text, uint32_t* value)
{
    if (!text || !*text || !value) {
        return 0;
    }
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    if (!isdigit((unsigned char)*text)) {
        return 0;
    }
    char* end;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || errno == ERANGE || parsed > 60000ul) {
        return 0;
    }
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (*end) {
        return 0;
    }
    *value = (uint32_t)parsed;
    return 1;
}

static inline kz3_xess_diagnostic_sample kz3_xess_diagnostic_next_sample(
    const kz3_xess_diagnostic_config* config,
    kz3_xess_diagnostic_history* history,
    uint32_t frame,
    float halton_x,
    float halton_y,
    int active)
{
    kz3_xess_diagnostic_sample sample;
    memset(&sample, 0, sizeof sample);
    if (!active) {
        history->valid = 0;
        return sample;
    }

    sample.active = 1;
    sample.nominal_x = config->two_phase ?
        ((frame & 1u) ? -0.25f : 0.25f) : halton_x;
    sample.nominal_y = config->two_phase ? 0.0f : halton_y;
    sample.projection_x = sample.nominal_x * config->projection_scale_x;
    sample.projection_y = sample.nominal_y * config->projection_scale_y;
    const int use_history =
        config->execute_lag && history->valid &&
        frame == history->frame + 1u;
    sample.execute_x =
        (use_history ? history->nominal_x : sample.nominal_x) *
        config->execute_scale_x;
    sample.execute_y =
        (use_history ? history->nominal_y : sample.nominal_y) *
        config->execute_scale_y;
    sample.lag_applied = use_history;
    history->frame = frame;
    history->nominal_x = sample.nominal_x;
    history->nominal_y = sample.nominal_y;
    history->valid = 1;
    return sample;
}

static inline void kz3_xess_diagnostic_clear_lag(
    const kz3_xess_diagnostic_config* config,
    kz3_xess_diagnostic_sample* sample)
{
    if (sample->active && sample->lag_applied) {
        sample->execute_x = sample->nominal_x * config->execute_scale_x;
        sample->execute_y = sample->nominal_y * config->execute_scale_y;
        sample->lag_applied = 0;
    }
}

static inline void kz3_xess_diagnostic_count_draw(
    kz3_xess_diagnostic_coverage* coverage,
    const kz3_xess_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t primary_depth_frame,
    uint32_t primary_depth_slot,
    int shifted,
    int sample_available)
{
    const int full_target =
        draw->target_is_local && draw->target_width == render_width &&
        draw->target_height == render_height;
    const int deferred = full_target && draw->mrt_count >= 4u;
    const int scene = !deferred && kz3_xess_is_scene_target(
        draw->target_offset, draw->target_width, draw->target_height,
        draw->target_is_local, render_width, render_height);

    coverage->draws++;
    if (deferred) {
        coverage->deferred++;
    } else if (scene) {
        coverage->scene++;
    } else {
        coverage->other++;
        return;
    }

    if (shifted) {
        coverage->shifted++;
    } else if (!draw->full_viewport) {
        coverage->partial_viewport++;
    } else if (!draw->depth_test) {
        coverage->depth_disabled++;
    } else if (!draw->depth_slot ||
               (scene && (draw->frame != primary_depth_frame ||
                          draw->depth_slot != primary_depth_slot))) {
        coverage->depth_mismatch++;
    } else if (sample_available) {
        coverage->zero_projection++;
    } else {
        coverage->unavailable++;
    }
}

#endif
