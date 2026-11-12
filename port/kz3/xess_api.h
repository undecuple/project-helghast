#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#include <d3d12.h>
#else
typedef struct ID3D12DescriptorHeap ID3D12DescriptorHeap;
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12GraphicsCommandList ID3D12GraphicsCommandList;
typedef struct ID3D12Heap ID3D12Heap;
typedef struct ID3D12PipelineLibrary ID3D12PipelineLibrary;
typedef struct ID3D12Resource ID3D12Resource;
#endif

/*
 * ENHANCEMENT: minimal XeSS-SR 2.0 ABI declarations used for dynamic loading. The public
 * API names and layouts are documented by Intel's XeSS SDK 3.0.2 developer
 * guide and headers:
 * https://github.com/intel/xess/tree/v3.0.2
 */

typedef struct kz3_xess_context* kz3_xess_context_handle;

#pragma pack(push, 8)

typedef struct kz3_xess_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
} kz3_xess_version;

typedef struct kz3_xess_2d {
    uint32_t x;
    uint32_t y;
} kz3_xess_2d;

typedef kz3_xess_2d kz3_xess_coord;

enum kz3_xess_quality {
    KZ3_XESS_QUALITY_ULTRA_PERFORMANCE = 100,
    KZ3_XESS_QUALITY_PERFORMANCE = 101,
    KZ3_XESS_QUALITY_BALANCED = 102,
    KZ3_XESS_QUALITY_QUALITY = 103,
    KZ3_XESS_QUALITY_ULTRA_QUALITY = 104,
    KZ3_XESS_QUALITY_ULTRA_QUALITY_PLUS = 105,
    KZ3_XESS_QUALITY_AA = 106,
};

enum kz3_xess_init_flags {
    KZ3_XESS_INIT_HIGH_RES_MV = 1u << 0,
    KZ3_XESS_INIT_INVERTED_DEPTH = 1u << 1,
    KZ3_XESS_INIT_EXPOSURE_SCALE_TEXTURE = 1u << 2,
    KZ3_XESS_INIT_RESPONSIVE_PIXEL_MASK = 1u << 3,
    KZ3_XESS_INIT_USE_NDC_VELOCITY = 1u << 4,
    KZ3_XESS_INIT_EXTERNAL_DESCRIPTOR_HEAP = 1u << 5,
    KZ3_XESS_INIT_LDR_INPUT_COLOR = 1u << 6,
    KZ3_XESS_INIT_JITTERED_MV = 1u << 7,
    KZ3_XESS_INIT_ENABLE_AUTOEXPOSURE = 1u << 8,
};

enum kz3_xess_result {
    KZ3_XESS_WARNING_NONEXISTING_FOLDER = 1,
    KZ3_XESS_WARNING_OLD_DRIVER = 2,
    KZ3_XESS_SUCCESS = 0,
    KZ3_XESS_ERROR_UNSUPPORTED_DEVICE = -1,
    KZ3_XESS_ERROR_UNSUPPORTED_DRIVER = -2,
    KZ3_XESS_ERROR_UNINITIALIZED = -3,
    KZ3_XESS_ERROR_INVALID_ARGUMENT = -4,
    KZ3_XESS_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    KZ3_XESS_ERROR_DEVICE = -6,
    KZ3_XESS_ERROR_NOT_IMPLEMENTED = -7,
    KZ3_XESS_ERROR_INVALID_CONTEXT = -8,
    KZ3_XESS_ERROR_OPERATION_IN_PROGRESS = -9,
    KZ3_XESS_ERROR_UNSUPPORTED = -10,
    KZ3_XESS_ERROR_CANT_LOAD_LIBRARY = -11,
    KZ3_XESS_ERROR_WRONG_CALL_ORDER = -12,
    KZ3_XESS_ERROR_UNKNOWN = -1000,
};

typedef struct kz3_xess_d3d12_execute_params {
    ID3D12Resource* color_texture;
    ID3D12Resource* velocity_texture;
    ID3D12Resource* depth_texture;
    ID3D12Resource* exposure_scale_texture;
    ID3D12Resource* responsive_pixel_mask_texture;
    ID3D12Resource* output_texture;
    float jitter_offset_x;
    float jitter_offset_y;
    float exposure_scale;
    uint32_t reset_history;
    uint32_t input_width;
    uint32_t input_height;
    kz3_xess_coord input_color_base;
    kz3_xess_coord input_motion_vector_base;
    kz3_xess_coord input_depth_base;
    kz3_xess_coord input_responsive_mask_base;
    kz3_xess_coord reserved0;
    kz3_xess_coord output_color_base;
    ID3D12DescriptorHeap* descriptor_heap;
    uint32_t descriptor_heap_offset;
} kz3_xess_d3d12_execute_params;

typedef struct kz3_xess_d3d12_init_params {
    kz3_xess_2d output_resolution;
    enum kz3_xess_quality quality_setting;
    uint32_t init_flags;
    uint32_t creation_node_mask;
    uint32_t visible_node_mask;
    ID3D12Heap* temp_buffer_heap;
    uint64_t buffer_heap_offset;
    ID3D12Heap* temp_texture_heap;
    uint64_t texture_heap_offset;
    ID3D12PipelineLibrary* pipeline_library;
} kz3_xess_d3d12_init_params;

#pragma pack(pop)

typedef int (*kz3_xess_get_version_fn)(kz3_xess_version*);
typedef int (*kz3_xess_create_context_fn)(
    ID3D12Device*, kz3_xess_context_handle*);
typedef int (*kz3_xess_get_optimal_input_resolution_fn)(
    kz3_xess_context_handle,
    const kz3_xess_2d*,
    enum kz3_xess_quality,
    kz3_xess_2d*,
    kz3_xess_2d*,
    kz3_xess_2d*);
typedef int (*kz3_xess_init_fn)(
    kz3_xess_context_handle,
    const kz3_xess_d3d12_init_params*);
typedef int (*kz3_xess_execute_fn)(
    kz3_xess_context_handle,
    ID3D12GraphicsCommandList*,
    const kz3_xess_d3d12_execute_params*);
typedef int (*kz3_xess_destroy_context_fn)(kz3_xess_context_handle);
typedef int (*kz3_xess_is_optimal_driver_fn)(kz3_xess_context_handle);

typedef struct kz3_xess_functions {
    kz3_xess_get_version_fn get_version;
    kz3_xess_create_context_fn create_context;
    kz3_xess_get_optimal_input_resolution_fn get_optimal_input_resolution;
    kz3_xess_init_fn init;
    kz3_xess_execute_fn execute;
    kz3_xess_destroy_context_fn destroy_context;
    kz3_xess_is_optimal_driver_fn is_optimal_driver;
} kz3_xess_functions;
