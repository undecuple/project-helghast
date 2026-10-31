#ifndef KZ3_FSR31_INPUTS_H
#define KZ3_FSR31_INPUTS_H

#include <stdint.h>

#define KZ3_FSR31_SCENE_TARGET_OFFSET 0x01300000u
#define KZ3_FSR31_DEFERRED_LIGHTING_OFFSET 0x01E40000u

typedef struct kz3_fsr31_draw_info {
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
} kz3_fsr31_draw_info;

typedef struct kz3_fsr31_capture_key {
    uint32_t frame;
    uint32_t context_generation;
    uint32_t target;
    uint32_t width;
    uint32_t height;
    uintptr_t resource;
} kz3_fsr31_capture_key;

static inline int kz3_fsr31_is_full_render_target(
    const kz3_fsr31_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return draw && draw->target_is_local && draw->full_viewport &&
           draw->target_width == render_width &&
           draw->target_height == render_height;
}

static inline int kz3_fsr31_is_deferred_primary(
    const kz3_fsr31_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return kz3_fsr31_is_full_render_target(
               draw, render_width, render_height) &&
           draw->mrt_count >= 4u && draw->depth_slot && draw->depth_test;
}

static inline int kz3_fsr31_is_scene_target(
    uint32_t target_offset,
    uint32_t target_width,
    uint32_t target_height,
    int target_is_local,
    uint32_t render_width,
    uint32_t render_height)
{
    return target_is_local &&
           target_offset == KZ3_FSR31_SCENE_TARGET_OFFSET &&
           target_width == render_width &&
           target_height == render_height;
}

static inline int kz3_fsr31_should_jitter(
    const kz3_fsr31_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t primary_depth_frame,
    uint32_t primary_depth_slot)
{
    if (kz3_fsr31_is_deferred_primary(
            draw, render_width, render_height)) {
        return 1;
    }
    return kz3_fsr31_is_full_render_target(
               draw, render_width, render_height) &&
           kz3_fsr31_is_scene_target(
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

static inline int kz3_fsr31_is_opaque_resolve(
    const kz3_fsr31_draw_info* draw,
    uint32_t render_width,
    uint32_t render_height)
{
    return kz3_fsr31_is_scene_target(
               draw ? draw->target_offset : 0u,
               draw ? draw->target_width : 0u,
               draw ? draw->target_height : 0u,
               draw ? draw->target_is_local : 0,
               render_width,
               render_height) &&
           draw->full_viewport && !draw->blend && !draw->depth_test &&
           draw->sampled_full_size_render_target &&
           draw->sampled_render_target_offset ==
               KZ3_FSR31_DEFERRED_LIGHTING_OFFSET;
}

static inline int kz3_fsr31_capture_pair_matches(
    const kz3_fsr31_capture_key* opaque,
    const kz3_fsr31_capture_key* pre_upscale)
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
