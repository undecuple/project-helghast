#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <d3d12.h>
#else
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12GraphicsCommandList ID3D12GraphicsCommandList;
typedef struct ID3D12Resource ID3D12Resource;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

enum kz3_fsr31_create_flags {
    KZ3_FSR31_CREATE_DEPTH_INVERTED = 1u << 0,
    KZ3_FSR31_CREATE_DEPTH_INFINITE = 1u << 1,
    KZ3_FSR31_CREATE_AUTO_EXPOSURE = 1u << 2,
    KZ3_FSR31_CREATE_DEBUG_CHECKING = 1u << 3,
};

enum kz3_fsr31_camera_flags {
    KZ3_FSR31_CAMERA_CURRENT_VALID = 1u << 0,
    KZ3_FSR31_CAMERA_PREVIOUS_VALID = 1u << 1,
    KZ3_FSR31_CAMERA_PLANES_VALID = 1u << 2,
    KZ3_FSR31_CAMERA_RESET = 1u << 3,
    KZ3_FSR31_CAMERA_MULTIPLE_VIEWS = 1u << 4,
};

typedef struct kz3_fsr31_camera_state {
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
} kz3_fsr31_camera_state;

typedef struct kz3_fsr31_dispatch_desc {
    ID3D12GraphicsCommandList* command_list;
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* motion_vectors;
    ID3D12Resource* reactive;
    ID3D12Resource* transparency_and_composition;
    ID3D12Resource* output;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float jitter_x;
    float jitter_y;
    float motion_vector_scale_x;
    float motion_vector_scale_y;
    float frame_time_ms;
    float camera_near;
    float camera_far;
    float camera_vertical_fov;
    float view_space_to_meters;
    float sharpness;
    uint32_t reset;
} kz3_fsr31_dispatch_desc;

typedef struct kz3_fsr31_camera_dispatch_desc {
    ID3D12GraphicsCommandList* command_list;
    ID3D12Resource* color;
    ID3D12Resource* depth;
    kz3_fsr31_camera_state camera;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float jitter_x;
    float jitter_y;
    float frame_time_ms;
    float sharpness;
    uint32_t reset;
    uint32_t use_reactive_mask;
} kz3_fsr31_camera_dispatch_desc;

/*
 * Input resources must be in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
 * and output must be in D3D12_RESOURCE_STATE_UNORDERED_ACCESS. The caller
 * retains ownership and performs any barriers needed after dispatch.
 */
int kz3_fsr31_is_supported(ID3D12Device* device);
int kz3_fsr31_initialize(
    ID3D12Device* device,
    uint32_t max_render_width,
    uint32_t max_render_height,
    uint32_t max_output_width,
    uint32_t max_output_height,
    uint32_t flags);
int kz3_fsr31_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y);
void kz3_fsr31_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov);
void kz3_fsr31_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane);
int kz3_fsr31_get_camera(kz3_fsr31_camera_state* out_state);
int kz3_fsr31_dispatch(const kz3_fsr31_dispatch_desc* desc);
/*
 * The capture source must be an RGBA8 render-resolution texture in
 * D3D12_RESOURCE_STATE_RENDER_TARGET. It is restored to that state before
 * returning. A pre-upscale capture is accepted only after an opaque capture
 * of the same resource in the current adapter context.
 */
int kz3_fsr31_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
int kz3_fsr31_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
/*
 * Generates depth-aware camera motion vectors and, when requested, a
 * reactive mask from the current capture pair before dispatching FSR into an
 * adapter-owned output. On success the returned resource is in
 * D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE and remains owned by the adapter.
 */
ID3D12Resource* kz3_fsr31_dispatch_camera(
    const kz3_fsr31_camera_dispatch_desc* desc);
void kz3_fsr31_shutdown(void);
const char* kz3_fsr31_status(void);

#if defined(__cplusplus)
}
#endif
