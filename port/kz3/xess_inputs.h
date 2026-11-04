#ifndef KZ3_XESS_INPUTS_H
#define KZ3_XESS_INPUTS_H

#include <math.h>
#include <stdint.h>

/*
 * ENHANCEMENT: pure selection rules for the XeSS-SR path. Everything here is host-tested (tools/smoke/test_xess_inputs.c).
 *
 * ASSUMPTION: these two local offsets were observed in 1.14 campaign frames. They are not recovered from the binary, and
 * a level or update that lays out memory differently falls back through the capture and depth checks.
 */
#define KZ3_XESS_SCENE_TARGET_OFFSET 0x01300000u
#define KZ3_XESS_DEFERRED_LIGHTING_OFFSET 0x01E40000u

typedef struct kz3_xess_draw_info {
    uint32_t frame;
    uint32_t target_offset;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t mrt_count;
    uint32_t depth_slot;
    uint32_t sampled_render_target_offset;
    int target_is_local;
    int full_viewport;
    int depth_test;
    int blend;
    int sampled_full_size_render_target;
} kz3_xess_draw_info;

typedef struct kz3_xess_capture_key {
    uint32_t frame;
    uint32_t context_generation;
    uint32_t target;
    uint32_t width;
    uint32_t height;
    uintptr_t resource;
} kz3_xess_capture_key;

/* The same shape, allowing the one output pixel of rounding that rsx_dm_fit and real panels (1366x768) introduce. */
static inline int kz3_xess_aspect_matches(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    const int64_t difference =
        (int64_t)render_width * output_height -
        (int64_t)render_height * output_width;
    const int64_t magnitude = difference < 0 ? -difference : difference;
    return magnitude <=
           (int64_t)(render_width > render_height ? render_width : render_height);
}

/* Intel's texture mip bias, log2(input / target), in the RSX FILTER field's signed 4.8 fixed point. */
static inline int kz3_xess_mip_bias_fp(uint32_t render_width, uint32_t output_width)
{
    if (!render_width || !output_width) {
        return 0;
    }
    return (int)lroundf(log2f((float)render_width / (float)output_width) * 256.0f);
}

/* Adds to the LOD bias in RSX FILTER [12:0], so sampler_key keeps the biased sampler distinct. */
static inline uint32_t kz3_xess_filter_with_bias(uint32_t filter, int bias_fp)
{
    int32_t bias = (int32_t)(filter & 0x1FFFu);
    if (bias & 0x1000) {
        bias -= 0x2000;
    }
    bias += bias_fp;
    if (bias < -0x1000) {
        bias = -0x1000;
    }
    if (bias > 0x0FFF) {
        bias = 0x0FFF;
    }
    return (filter & ~(uint32_t)0x1FFFu) | ((uint32_t)bias & 0x1FFFu);
}

static inline int kz3_xess_is_full_render_target(
    const kz3_xess_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return draw && draw->target_is_local && draw->full_viewport &&
           draw->target_width == render_width &&
           draw->target_height == render_height;
}

static inline int kz3_xess_is_deferred_primary(
    const kz3_xess_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return kz3_xess_is_full_render_target(
               draw, render_width, render_height) &&
           draw->mrt_count >= 4u && draw->depth_slot && draw->depth_test;
}

static inline int kz3_xess_is_scene_target(
    uint32_t target_offset,
    uint32_t target_width,
    uint32_t target_height,
    int target_is_local,
    uint32_t render_width,
    uint32_t render_height)
{
    return target_is_local &&
           target_offset == KZ3_XESS_SCENE_TARGET_OFFSET &&
           target_width == render_width &&
           target_height == render_height;
}

static inline int kz3_xess_should_jitter(
    const kz3_xess_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t primary_depth_frame,
    uint32_t primary_depth_slot)
{
    if (kz3_xess_is_deferred_primary(
            draw, render_width, render_height)) {
        return 1;
    }
    return kz3_xess_is_full_render_target(
               draw, render_width, render_height) &&
           kz3_xess_is_scene_target(
               draw->target_offset,
               draw->target_width,
               draw->target_height,
               draw->target_is_local,
               render_width,
               render_height) &&
           draw->depth_test && draw->depth_slot &&
           draw->frame == primary_depth_frame &&
           draw->depth_slot == primary_depth_slot;
}

static inline int kz3_xess_is_opaque_resolve(
    const kz3_xess_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return kz3_xess_is_scene_target(
               draw ? draw->target_offset : 0u,
               draw ? draw->target_width : 0u,
               draw ? draw->target_height : 0u,
               draw ? draw->target_is_local : 0,
               render_width,
               render_height) &&
           draw->full_viewport && !draw->blend && !draw->depth_test &&
           draw->sampled_full_size_render_target &&
           draw->sampled_render_target_offset ==
               KZ3_XESS_DEFERRED_LIGHTING_OFFSET;
}

static inline int kz3_xess_capture_pair_matches(
    const kz3_xess_capture_key* opaque,
    const kz3_xess_capture_key* pre_upscale)
{
    return opaque && pre_upscale && opaque->resource &&
           opaque->frame == pre_upscale->frame &&
           opaque->context_generation == pre_upscale->context_generation &&
           opaque->target == pre_upscale->target &&
           opaque->width == pre_upscale->width &&
           opaque->height == pre_upscale->height &&
           opaque->resource == pre_upscale->resource;
}

#endif
