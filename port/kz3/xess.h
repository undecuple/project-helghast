#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <d3d12.h>
#else
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12GraphicsCommandList ID3D12GraphicsCommandList;
typedef struct ID3D12Resource ID3D12Resource;
typedef int D3D12_RESOURCE_STATES;
#endif

#include "xess_camera.h"

/* ENHANCEMENT: the C boundary between the KZ3 XeSS-SR adapter (xess.cpp) and the patched renderer. Docs: docs/XESS.md. */

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct kz3_xess_camera_dispatch_desc {
    ID3D12GraphicsCommandList* command_list;
    ID3D12Resource* color;
    kz3_xess_camera_state camera;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float jitter_x;
    float jitter_y;
    uint32_t reset;
    uint32_t use_responsive_mask;
    uint32_t use_hud_mask;
    float execute_jitter_x;
    float execute_jitter_y;
    uint32_t use_execute_jitter_override;
} kz3_xess_camera_dispatch_desc;

typedef struct kz3_xess_jitter_offset {
    float x;
    float y;
} kz3_xess_jitter_offset;

static inline kz3_xess_jitter_offset kz3_xess_execute_jitter(
    const kz3_xess_camera_dispatch_desc* desc)
{
    const kz3_xess_jitter_offset result = {
        desc->use_execute_jitter_override ? desc->execute_jitter_x : desc->jitter_x,
        desc->use_execute_jitter_override ? desc->execute_jitter_y : desc->jitter_y
    };
    return result;
}

int kz3_xess_initialize(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height);
/* 1 when kz3_xess_initialize with these arguments would release a live context and its textures. */
int kz3_xess_would_recreate(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height);
int kz3_xess_has_context(void);
int kz3_xess_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y);
void kz3_xess_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov);
void kz3_xess_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane);
int kz3_xess_get_camera(kz3_xess_camera_state* out_state);
int kz3_xess_get_camera_for_flip(
    uint64_t guest_flip,
    kz3_xess_camera_state* out_state);
int kz3_xess_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
int kz3_xess_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
/* The display surface right after the 3D compose: the frame without its HUD. */
int kz3_xess_capture_compose_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
/* The resolved R32_FLOAT scene depth; the resource is returned to depth_state. */
int kz3_xess_capture_depth(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* depth,
    D3D12_RESOURCE_STATES depth_state,
    uint32_t width,
    uint32_t height);
ID3D12Resource* kz3_xess_dispatch_camera(
    const kz3_xess_camera_dispatch_desc* desc);
void kz3_xess_shutdown(void);
const char* kz3_xess_status(void);

/* tools/smoke/test_xess.cpp reads these back to check what the compute passes wrote. */
ID3D12Resource* kz3_xess_debug_motion_vectors(void);
ID3D12Resource* kz3_xess_debug_responsive_mask(void);

#if defined(__cplusplus)
}
#endif
