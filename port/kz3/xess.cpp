/*
 * ENHANCEMENT: the XeSS-SR adapter. The PS3 game has no temporal upscaler; this path is opt-in and Bilinear stays the
 * default. Written from Intel's public XeSS SDK 3.0.2 developer guide and headers. Design and limits: docs/XESS.md.
 */
#include "xess.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

#include "rsx_live_draw.h"
#include "rsx_upscale.h"
#include "xess_api.h"
#include "xess_inputs.h"

namespace {

constexpr wchar_t kRuntimeDll[] = L"libxess.dll";
constexpr uint16_t kApiMajor = 2;
constexpr uint16_t kApiMinor = 0;
constexpr uint16_t kApiPatch = 2;

struct XessState {
    std::mutex mutex;
    HMODULE module = nullptr;
    kz3_xess_functions functions{};
    kz3_xess_context_handle context = nullptr;
    ID3D12Device* device = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    enum kz3_xess_quality quality = KZ3_XESS_QUALITY_AA;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* opaque_color = nullptr;
    ID3D12Resource* pre_upscale_color = nullptr;
    ID3D12Resource* compose_color = nullptr;
    ID3D12Resource* present_color = nullptr;
    ID3D12Resource* scene_depth = nullptr;
    ID3D12Resource* responsive_mask = nullptr;
    ID3D12DescriptorHeap* motion_heap = nullptr;
    ID3D12RootSignature* motion_root_signature = nullptr;
    ID3D12PipelineState* motion_pipeline = nullptr;
    ID3D12DescriptorHeap* responsive_heap = nullptr;
    ID3D12RootSignature* responsive_root_signature = nullptr;
    ID3D12PipelineState* responsive_pipeline = nullptr;
    D3D12_RESOURCE_STATES opaque_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES pre_upscale_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES compose_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES present_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES scene_depth_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool motion_is_compute_read = false;
    bool output_is_pixel_read = false;
    bool responsive_is_compute_read = false;
    bool depth_captured = false;
    /* One-frame input alignment (see kz3_xess_dispatch_camera). The captures land in these hold textures; after each
     * dispatch they are copied into scene_depth, opaque_color and pre_upscale_color, which the passes and XeSS read,
     * so the next dispatch can use this frame's inputs with next frame's colour. */
    ID3D12Resource* depth_hold = nullptr;
    ID3D12Resource* opaque_hold = nullptr;
    ID3D12Resource* pre_upscale_hold = nullptr;
    D3D12_RESOURCE_STATES depth_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES opaque_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES pre_upscale_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool have_previous = false;            /* the read textures hold the last successful dispatch's frame */
    bool previous_translucent = false;     /* and its opaque/scene pair was coherent and requested */
    kz3_xess_camera_state previous_camera{};
    float previous_jitter_x = 0.0f;
    float previous_jitter_y = 0.0f;
    kz3_xess_jitter_offset previous_execute_jitter{};
    uint64_t delayed_count = 0;
    uint64_t capture_serial = 0;
    uint64_t opaque_capture_serial = 0;
    uint64_t pre_upscale_capture_serial = 0;
    uint64_t dispatch_count = 0;
    uint64_t history_reset_count = 0;
    ID3D12Resource* capture_source = nullptr;
    /* The last arguments a context could not be built for: a permanent failure is not retried every frame. */
    bool failed = false;
    ID3D12Device* failed_device = nullptr;
    uint32_t failed_size[4] = {};
    std::string status = "XeSS-SR has not been initialized";
};

XessState g_xess;

struct CameraBridge {
    std::mutex mutex;
    kz3_xess_camera_bridge bridge{};
    bool handedness_logged = false;
};

CameraBridge g_camera;

void set_status(const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_xess.status = message;
}

void set_win32_status(const char* action)
{
    const DWORD error = GetLastError();
    char system_message[256] = {};
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        system_message,
        static_cast<DWORD>(sizeof(system_message)),
        nullptr);
    size_t length = strlen(system_message);
    while (length && (system_message[length - 1] == '\r' ||
                      system_message[length - 1] == '\n')) {
        system_message[--length] = '\0';
    }
    set_status(
        "%s failed (Win32 error %lu%s%s)",
        action,
        static_cast<unsigned long>(error),
        system_message[0] ? ": " : "",
        system_message);
}

const char* result_name(int result)
{
    switch (result) {
    case KZ3_XESS_SUCCESS:
        return "success";
    case KZ3_XESS_WARNING_OLD_DRIVER:
        return "old driver";
    case KZ3_XESS_ERROR_UNSUPPORTED_DEVICE:
        return "unsupported device";
    case KZ3_XESS_ERROR_UNSUPPORTED_DRIVER:
        return "unsupported driver";
    case KZ3_XESS_ERROR_UNINITIALIZED:
        return "uninitialized";
    case KZ3_XESS_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case KZ3_XESS_ERROR_DEVICE_OUT_OF_MEMORY:
        return "device out of memory";
    case KZ3_XESS_ERROR_DEVICE:
        return "device error";
    case KZ3_XESS_ERROR_NOT_IMPLEMENTED:
        return "not implemented";
    case KZ3_XESS_ERROR_INVALID_CONTEXT:
        return "invalid context";
    case KZ3_XESS_ERROR_OPERATION_IN_PROGRESS:
        return "operation in progress";
    case KZ3_XESS_ERROR_UNSUPPORTED:
        return "unsupported";
    case KZ3_XESS_ERROR_CANT_LOAD_LIBRARY:
        return "cannot load library";
    case KZ3_XESS_ERROR_WRONG_CALL_ORDER:
        return "wrong call order";
    default:
        return "unknown";
    }
}

template <typename T>
void release(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void destroy_resources_locked()
{
    release(g_xess.responsive_pipeline);
    release(g_xess.responsive_root_signature);
    release(g_xess.responsive_heap);
    release(g_xess.motion_pipeline);
    release(g_xess.motion_root_signature);
    release(g_xess.motion_heap);
    release(g_xess.responsive_mask);
    release(g_xess.scene_depth);
    release(g_xess.depth_hold);
    release(g_xess.opaque_hold);
    release(g_xess.pre_upscale_hold);
    release(g_xess.present_color);
    release(g_xess.compose_color);
    release(g_xess.pre_upscale_color);
    release(g_xess.opaque_color);
    release(g_xess.output);
    release(g_xess.motion_vectors);
    g_xess.opaque_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.pre_upscale_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.compose_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.present_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.scene_depth_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.depth_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.opaque_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.pre_upscale_hold_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.have_previous = false;
    g_xess.previous_translucent = false;
    g_xess.delayed_count = 0;
    g_xess.motion_is_compute_read = false;
    g_xess.output_is_pixel_read = false;
    g_xess.responsive_is_compute_read = false;
    g_xess.depth_captured = false;
    g_xess.capture_serial = 0;
    g_xess.opaque_capture_serial = 0;
    g_xess.pre_upscale_capture_serial = 0;
    g_xess.dispatch_count = 0;
    g_xess.history_reset_count = 0;
    g_xess.capture_source = nullptr;
}

void destroy_context_locked()
{
    destroy_resources_locked();
    if (g_xess.context && g_xess.functions.destroy_context) {
        const int result =
            g_xess.functions.destroy_context(g_xess.context);
        if (result != KZ3_XESS_SUCCESS) {
            fprintf(
                stderr,
                "[xess] xessDestroyContext failed (%d: %s)\n",
                result,
                result_name(result));
        }
        g_xess.context = nullptr;
    }
    if (g_xess.device) {
        g_xess.device->Release();
        g_xess.device = nullptr;
    }
    g_xess.render_width = 0;
    g_xess.render_height = 0;
    g_xess.output_width = 0;
    g_xess.output_height = 0;
    g_xess.quality = KZ3_XESS_QUALITY_AA;
}

bool create_texture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    ID3D12Resource** out_resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = width;
    resource.Height = height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = format;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_ID3D12Resource,
        reinterpret_cast<void**>(out_resource));
    if (FAILED(hr)) {
        set_status(
            "creating a XeSS-SR texture failed (HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool compile_compute_shader(
    const char* source,
    const char* name,
    ID3DBlob** out_shader)
{
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(
        source,
        strlen(source),
        name,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        out_shader,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "compiling the XeSS-SR %s shader failed: %s",
                name,
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        errors->Release();
    }
    return SUCCEEDED(hr);
}

bool create_compute_pipeline(
    ID3D12Device* device,
    ID3DBlob* shader,
    D3D12_DESCRIPTOR_RANGE* ranges,
    UINT range_count,
    UINT constant_count,
    ID3D12RootSignature** out_root_signature,
    ID3D12PipelineState** out_pipeline)
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = range_count;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = constant_count;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;

    ID3DBlob* signature = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "serializing a XeSS-SR root signature failed: %s",
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        errors->Release();
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_ID3D12RootSignature,
            reinterpret_cast<void**>(out_root_signature));
    }
    release(signature);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = *out_root_signature;
        pipeline.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline.CS.BytecodeLength = shader->GetBufferSize();
        hr = device->CreateComputePipelineState(
            &pipeline,
            IID_ID3D12PipelineState,
            reinterpret_cast<void**>(out_pipeline));
    }
    if (FAILED(hr)) {
        set_status(
            "creating a XeSS-SR compute pipeline failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_motion_pipeline_locked(ID3D12Device* device)
{
    static constexpr char source[] =
        "cbuffer Camera : register(b0) {\n"
        "  float4 currentRight; float4 currentForward; float4 currentUp;\n"
        "  float4 previousRight; float4 previousForward; float4 previousUp;\n"
        "  float4 currentPosition; float4 previousPosition;\n"
        "  float4 fovTangent; float4 sizeJitter; float4 depthPlanes;\n"
        "};\n"
        "RWTexture2D<float2> motion : register(u0);\n"
        "Texture2D<float> sceneDepth : register(t0);\n"
        "Texture2D<float> responsive : register(t1);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "  uint2 size = (uint2)sizeJitter.xy;\n"
        "  if (id.x >= size.x || id.y >= size.y) return;\n"
        "  if (responsive.Load(int3(id.xy, 0)) > 0.9) {\n"
        "    motion[id.xy] = 0.0; return;\n"
        "  }\n"
        "  float depth = sceneDepth.Load(int3(id.xy, 0));\n"
        "  if (depth < 0.0 || depth > 1.0) {\n"
        "    motion[id.xy] = 0.0; return;\n"
        "  }\n"
        "  float denominator = depthPlanes.y -\n"
        "      depth * (depthPlanes.y - depthPlanes.x);\n"
        "  if (!(denominator > 0.0)) { motion[id.xy] = 0.0; return; }\n"
        "  float viewDepth = depthPlanes.x * depthPlanes.y / denominator;\n"
        "  float2 ndc = float2((id.x + 0.5) * 2.0 / size.x - 1.0,\n"
        "                      1.0 - (id.y + 0.5) * 2.0 / size.y);\n"
        "  float2 jitter = float2(2.0 * sizeJitter.z / size.x,\n"
        "                         -2.0 * sizeJitter.w / size.y);\n"
        "  float2 currentNdc = ndc - jitter;\n"
        "  float3 ray = currentForward.xyz +\n"
        "      currentRight.xyz * currentNdc.x * fovTangent.x +\n"
        "      currentUp.xyz * currentNdc.y * fovTangent.y;\n"
        "  float3 world = currentPosition.xyz + ray * viewDepth;\n"
        "  float3 previousRelative = world - previousPosition.xyz;\n"
        "  float forward = dot(previousRelative, previousForward.xyz);\n"
        "  if (forward <= 1e-5) { motion[id.xy] = 0.0; return; }\n"
        "  float2 previousNdc = float2(\n"
        "      dot(previousRelative, previousRight.xyz) /\n"
        "          (forward * fovTangent.z),\n"
        "      dot(previousRelative, previousUp.xyz) /\n"
        "          (forward * fovTangent.w));\n"
        "  motion[id.xy] = previousNdc - currentNdc;\n"
        "}\n";

    ID3DBlob* shader = nullptr;
    if (!compile_compute_shader(
            source, "kz3_xess_motion", &shader)) {
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    const bool pipeline_ok = create_compute_pipeline(
        device,
        shader,
        ranges,
        2,
        44,
        &g_xess.motion_root_signature,
        &g_xess.motion_pipeline);
    release(shader);
    if (!pipeline_ok) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 3;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(
        &heap,
        IID_ID3D12DescriptorHeap,
        reinterpret_cast<void**>(&g_xess.motion_heap));
    if (FAILED(hr)) {
        set_status(
            "creating the XeSS-SR motion descriptor heap failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_responsive_pipeline_locked(ID3D12Device* device)
{
    /* ASSUMPTION: experimental heuristics, not game-derived. 0.2 separates translucent composition from noise; 0.006 is
     * about 1.5/255, so only bytes the HUD actually changed count (the compose and present images are otherwise
     * identical). 1.0 marks a HUD pixel for the motion pass; XeSS clamps the mask to its default 0.8 either way. */
    static constexpr char source[] =
        "cbuffer Responsive : register(b0) {\n"
        "  uint width; uint height; uint translucent; uint hud;\n"
        "};\n"
        "RWTexture2D<float> responsive : register(u0);\n"
        "Texture2D<float4> opaqueColor : register(t0);\n"
        "Texture2D<float4> completeColor : register(t1);\n"
        "Texture2D<float4> composeColor : register(t2);\n"
        "Texture2D<float4> presentColor : register(t3);\n"
        "float biggest(float3 d) { return max(d.r, max(d.g, d.b)); }\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "  if (id.x >= width || id.y >= height) return;\n"
        "  int3 p = int3(id.xy, 0);\n"
        "  float value = 0.0;\n"
        "  if (translucent != 0 && biggest(abs(opaqueColor.Load(p).rgb -\n"
        "      completeColor.Load(p).rgb)) >= 0.2) value = 0.8;\n"
        "  if (hud != 0 && biggest(abs(composeColor.Load(p).rgb -\n"
        "      presentColor.Load(p).rgb)) >= 0.006) value = 1.0;\n"
        "  responsive[id.xy] = value;\n"
        "}\n";

    ID3DBlob* shader = nullptr;
    if (!compile_compute_shader(
            source, "kz3_xess_responsive", &shader)) {
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    const bool pipeline_ok = create_compute_pipeline(
        device,
        shader,
        ranges,
        2,
        4,
        &g_xess.responsive_root_signature,
        &g_xess.responsive_pipeline);
    release(shader);
    if (!pipeline_ok) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 5;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(
        &heap,
        IID_ID3D12DescriptorHeap,
        reinterpret_cast<void**>(&g_xess.responsive_heap));
    if (FAILED(hr)) {
        set_status(
            "creating the XeSS-SR responsive descriptor heap failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_resources_locked(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    if (!create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R16G16_FLOAT,
            &g_xess.motion_vectors) ||
        !create_texture(
            device,
            output_width,
            output_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.output) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.opaque_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.pre_upscale_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.compose_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.present_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R32_FLOAT,
            &g_xess.scene_depth) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R32_FLOAT,
            &g_xess.depth_hold) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.opaque_hold) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.pre_upscale_hold) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8_UNORM,
            &g_xess.responsive_mask) ||
        !create_motion_pipeline_locked(device) ||
        !create_responsive_pipeline_locked(device)) {
        destroy_resources_locked();
        return false;
    }

    /* Every descriptor points at a texture this adapter owns, so none is rewritten while a frame is in flight. */
    const UINT descriptor_size =
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE motion_handle =
        g_xess.motion_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC motion_uav{};
    motion_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    motion_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        g_xess.motion_vectors, nullptr, &motion_uav, motion_handle);
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
    depth_srv.Format = DXGI_FORMAT_R32_FLOAT;
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_srv.Texture2D.MipLevels = 1;
    motion_handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.scene_depth, &depth_srv, motion_handle);
    D3D12_SHADER_RESOURCE_VIEW_DESC mask_srv = depth_srv;
    mask_srv.Format = DXGI_FORMAT_R8_UNORM;
    motion_handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.responsive_mask, &mask_srv, motion_handle);

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        g_xess.responsive_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC responsive_uav{};
    responsive_uav.Format = DXGI_FORMAT_R8_UNORM;
    responsive_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        g_xess.responsive_mask, nullptr, &responsive_uav, handle);

    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Texture2D.MipLevels = 1;
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.opaque_color, &color_srv, handle);
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.pre_upscale_color, &color_srv, handle);
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.compose_color, &color_srv, handle);
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.present_color, &color_srv, handle);
    return true;
}

bool texture_matches(
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format)
{
    if (!texture) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Width == width && desc.Height == height &&
           desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
           desc.Format == format && desc.SampleDesc.Count == 1;
}

/* Copies source (left in source_state) into destination and leaves destination readable by compute and XeSS. */
void copy_texture_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    ID3D12Resource* destination,
    D3D12_RESOURCE_STATES& destination_state)
{
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = source_state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = destination;
    barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = destination_state;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(2, barriers);
    command_list->CopyResource(destination, source);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = source_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(2, barriers);
    destination_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

bool capture_color_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height,
    bool opaque)
{
    if (!g_xess.context || !command_list ||
        !texture_matches(
            color, width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        width != g_xess.render_width ||
        height != g_xess.render_height) {
        set_status("XeSS-SR scene capture received invalid state or color");
        return false;
    }
    if (!opaque &&
        (!g_xess.opaque_capture_serial ||
         g_xess.capture_source != color ||
         g_xess.pre_upscale_capture_serial ==
             g_xess.opaque_capture_serial)) {
        set_status(
            "XeSS-SR pre-upscale capture has no matching opaque color");
        return false;
    }

    copy_texture_locked(
        command_list,
        color,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        opaque ? g_xess.opaque_hold : g_xess.pre_upscale_hold,
        opaque ? g_xess.opaque_hold_state :
                 g_xess.pre_upscale_hold_state);

    if (opaque) {
        if (!++g_xess.capture_serial) {
            ++g_xess.capture_serial;
        }
        g_xess.opaque_capture_serial = g_xess.capture_serial;
        g_xess.pre_upscale_capture_serial = 0;
        g_xess.capture_source = color;
    } else {
        g_xess.pre_upscale_capture_serial =
            g_xess.opaque_capture_serial;
    }
    return true;
}

/* The opaque and scene captures in the hold textures belong to one frame. */
bool hold_pair_coherent_locked()
{
    return g_xess.opaque_capture_serial &&
           g_xess.pre_upscale_capture_serial == g_xess.opaque_capture_serial;
}

/* Copies this frame's captures from the hold textures into the textures the passes and XeSS read. */
void promote_holds_locked(ID3D12GraphicsCommandList* command_list)
{
    copy_texture_locked(
        command_list, g_xess.depth_hold, g_xess.depth_hold_state,
        g_xess.scene_depth, g_xess.scene_depth_state);
    copy_texture_locked(
        command_list, g_xess.opaque_hold, g_xess.opaque_hold_state,
        g_xess.opaque_color, g_xess.opaque_color_state);
    copy_texture_locked(
        command_list, g_xess.pre_upscale_hold, g_xess.pre_upscale_hold_state,
        g_xess.pre_upscale_color, g_xess.pre_upscale_color_state);
}

/* KZ3_XESS_INPUT_DELAY=0 turns the one-frame input alignment off for one run (default on). Read once. */
bool input_delay_enabled()
{
    static const bool enabled = [] {
        const char* value = getenv("KZ3_XESS_INPUT_DELAY");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

bool generate_responsive_locked(
    ID3D12GraphicsCommandList* command_list,
    uint32_t width,
    uint32_t height,
    bool translucent,
    bool hud)
{
    if (!g_xess.context || !command_list ||
        !g_xess.responsive_mask || !g_xess.opaque_color ||
        !g_xess.pre_upscale_color || !g_xess.responsive_heap ||
        !g_xess.responsive_root_signature ||
        !g_xess.responsive_pipeline) {
        set_status("XeSS-SR responsive-mask resources are unavailable");
        return false;
    }
    if (g_xess.responsive_is_compute_read) {
        D3D12_RESOURCE_BARRIER writable{};
        writable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        writable.Transition.pResource = g_xess.responsive_mask;
        writable.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        writable.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        writable.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        command_list->ResourceBarrier(1, &writable);
        g_xess.responsive_is_compute_read = false;
    }

    const UINT constants[4] = {
        width, height, translucent ? 1u : 0u, hud ? 1u : 0u};
    ID3D12DescriptorHeap* heaps[] = {g_xess.responsive_heap};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetPipelineState(g_xess.responsive_pipeline);
    command_list->SetComputeRootSignature(
        g_xess.responsive_root_signature);
    command_list->SetComputeRootDescriptorTable(
        0,
        g_xess.responsive_heap->GetGPUDescriptorHandleForHeapStart());
    command_list->SetComputeRoot32BitConstants(
        1, 4, constants, 0);
    command_list->Dispatch(
        (width + 7u) / 8u,
        (height + 7u) / 8u,
        1);

    D3D12_RESOURCE_BARRIER readable{};
    readable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readable.Transition.pResource = g_xess.responsive_mask;
    readable.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    readable.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    readable.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &readable);
    g_xess.responsive_is_compute_read = true;
    return true;
}

template <typename T>
bool load_export(T* out_function, const char* name)
{
    const FARPROC address = GetProcAddress(g_xess.module, name);
    static_assert(sizeof(*out_function) == sizeof(address));
    memcpy(out_function, &address, sizeof(address));
    if (*out_function) {
        return true;
    }
    set_status("libxess.dll is missing the required %s export", name);
    return false;
}

bool load_runtime_locked()
{
    if (g_xess.module) {
        return true;
    }

    std::vector<wchar_t> path(32768);
    const DWORD length =
        GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        set_win32_status("GetModuleFileNameW");
        return false;
    }
    wchar_t* slash = wcsrchr(path.data(), L'\\');
    wchar_t* forward_slash = wcsrchr(path.data(), L'/');
    if (!slash || (forward_slash && forward_slash > slash)) {
        slash = forward_slash;
    }
    if (!slash) {
        set_status("the executable path has no directory");
        return false;
    }
    const size_t directory_length =
        static_cast<size_t>(slash - path.data() + 1);
    if (directory_length + std::size(kRuntimeDll) > path.size()) {
        set_status("the XeSS runtime path is too long");
        return false;
    }
    memcpy(
        path.data() + directory_length,
        kRuntimeDll,
        sizeof(kRuntimeDll));

    g_xess.module = LoadLibraryExW(
        path.data(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_xess.module) {
        const DWORD error = GetLastError();
        if (error == ERROR_MOD_NOT_FOUND &&
            GetFileAttributesW(path.data()) != INVALID_FILE_ATTRIBUTES) {
            /* The file is there, so one of its imports is not: Intel's guide lists the VC++ runtime DLLs. */
            set_status(
                "libxess.dll is present but a dependency is missing; "
                "install the Microsoft Visual C++ Redistributable 14.40 "
                "or later");
            return false;
        }
        SetLastError(error);
        set_win32_status("loading libxess.dll beside the executable");
        return false;
    }

    bool complete =
        load_export(
            &g_xess.functions.get_version, "xessGetVersion") &&
        load_export(
            &g_xess.functions.create_context,
            "xessD3D12CreateContext") &&
        load_export(
            &g_xess.functions.get_optimal_input_resolution,
            "xessGetOptimalInputResolution") &&
        load_export(&g_xess.functions.init, "xessD3D12Init") &&
        load_export(
            &g_xess.functions.execute, "xessD3D12Execute") &&
        load_export(
            &g_xess.functions.destroy_context,
            "xessDestroyContext");
    const FARPROC optimal_driver =
        GetProcAddress(g_xess.module, "xessIsOptimalDriver");
    static_assert(
        sizeof(g_xess.functions.is_optimal_driver) ==
        sizeof(optimal_driver));
    memcpy(
        &g_xess.functions.is_optimal_driver,
        &optimal_driver,
        sizeof(optimal_driver));
    if (!complete) {
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
        return false;
    }

    kz3_xess_version version{};
    const int result = g_xess.functions.get_version(&version);
    if (result != KZ3_XESS_SUCCESS ||
        version.major != kApiMajor ||
        version.minor != kApiMinor ||
        version.patch != kApiPatch ||
        version.reserved != 0) {
        set_status(
            "libxess.dll exposes API %u.%u.%u (result %d); "
            "XeSS SDK 3.0.2 API 2.0.2 is required",
            version.major,
            version.minor,
            version.patch,
            result);
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
        return false;
    }
    return true;
}

bool dimensions_valid(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    if (!render_width || !render_height ||
        !output_width || !output_height ||
        render_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        render_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_width < render_width ||
        output_height < render_height) {
        return false;
    }
    return kz3_xess_aspect_matches(
               render_width,
               render_height,
               output_width,
               output_height) != 0;
}

bool same_failure_locked(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    return g_xess.failed && g_xess.failed_device == device &&
           g_xess.failed_size[0] == render_width &&
           g_xess.failed_size[1] == render_height &&
           g_xess.failed_size[2] == output_width &&
           g_xess.failed_size[3] == output_height;
}

void note_failure_locked(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    g_xess.failed = true;
    g_xess.failed_device = device;
    g_xess.failed_size[0] = render_width;
    g_xess.failed_size[1] = render_height;
    g_xess.failed_size[2] = output_width;
    g_xess.failed_size[3] = output_height;
}

enum kz3_xess_quality quality_for_scale(
    uint32_t render_width,
    uint32_t output_width)
{
    const double scale =
        static_cast<double>(output_width) /
        static_cast<double>(render_width);
    if (scale <= 1.001) {
        return KZ3_XESS_QUALITY_AA;
    }
    if (scale <= 1.301) {
        return KZ3_XESS_QUALITY_ULTRA_QUALITY_PLUS;
    }
    if (scale <= 1.501) {
        return KZ3_XESS_QUALITY_ULTRA_QUALITY;
    }
    if (scale <= 1.701) {
        return KZ3_XESS_QUALITY_QUALITY;
    }
    if (scale <= 2.001) {
        return KZ3_XESS_QUALITY_BALANCED;
    }
    if (scale <= 2.301) {
        return KZ3_XESS_QUALITY_PERFORMANCE;
    }
    return KZ3_XESS_QUALITY_ULTRA_PERFORMANCE;
}

const char* quality_name(enum kz3_xess_quality quality)
{
    switch (quality) {
    case KZ3_XESS_QUALITY_ULTRA_PERFORMANCE:
        return "Ultra Performance";
    case KZ3_XESS_QUALITY_PERFORMANCE:
        return "Performance";
    case KZ3_XESS_QUALITY_BALANCED:
        return "Balanced";
    case KZ3_XESS_QUALITY_QUALITY:
        return "Quality";
    case KZ3_XESS_QUALITY_ULTRA_QUALITY:
        return "Ultra Quality";
    case KZ3_XESS_QUALITY_ULTRA_QUALITY_PLUS:
        return "Ultra Quality Plus";
    case KZ3_XESS_QUALITY_AA:
        return "Native AA";
    default:
        return "unknown";
    }
}

bool scale_supported(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    return static_cast<uint64_t>(output_width) <=
               static_cast<uint64_t>(render_width) * 3u &&
           static_cast<uint64_t>(output_height) <=
               static_cast<uint64_t>(render_height) * 3u;
}

bool context_matches_locked(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    return g_xess.context && g_xess.device == device &&
           g_xess.render_width == render_width &&
           g_xess.render_height == render_height &&
           g_xess.output_width == output_width &&
           g_xess.output_height == output_height;
}

float halton(uint64_t index, uint32_t base)
{
    float value = 0.0f;
    float fraction = 1.0f;
    while (index) {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return value;
}

}  // namespace

extern "C" int kz3_xess_would_recreate(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return g_xess.context && device &&
           dimensions_valid(
               render_width,
               render_height,
               output_width,
               output_height) &&
           scale_supported(
               render_width,
               render_height,
               output_width,
               output_height) &&
           !context_matches_locked(
               device,
               render_width,
               render_height,
               output_width,
               output_height);
}

extern "C" int kz3_xess_has_context(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return g_xess.context != nullptr;
}

extern "C" int kz3_xess_runtime_available(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return load_runtime_locked() ? 1 : 0;
}

extern "C" int kz3_xess_initialize(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!device ||
        !dimensions_valid(
            render_width,
            render_height,
            output_width,
            output_height)) {
        set_status(
            "XeSS-SR requires non-decreasing input/output dimensions "
            "with the same aspect ratio");
        return 0;
    }
    if (!scale_supported(
            render_width,
            render_height,
            output_width,
            output_height)) {
        set_status("XeSS-SR output exceeds the supported 3x scale");
        return 0;
    }
    if (context_matches_locked(
            device,
            render_width,
            render_height,
            output_width,
            output_height)) {
        return 1;
    }
    if (same_failure_locked(
            device,
            render_width,
            render_height,
            output_width,
            output_height)) {
        return 0;
    }

    destroy_context_locked();
    note_failure_locked(
        device,
        render_width,
        render_height,
        output_width,
        output_height);
    if (!load_runtime_locked()) {
        return 0;
    }

    int result =
        g_xess.functions.create_context(device, &g_xess.context);
    if (result != KZ3_XESS_SUCCESS || !g_xess.context) {
        g_xess.context = nullptr;
        set_status(
            "creating the XeSS-SR context failed (%d: %s)",
            result,
            result_name(result));
        return 0;
    }
    if (g_xess.functions.is_optimal_driver &&
        g_xess.functions.is_optimal_driver(g_xess.context) ==
            KZ3_XESS_WARNING_OLD_DRIVER) {
        fprintf(
            stderr,
            "[xess] warning: the graphics driver is not optimal for "
            "XeSS-SR\n");
    }

    const enum kz3_xess_quality quality =
        quality_for_scale(render_width, output_width);
    const kz3_xess_2d output = {output_width, output_height};
    kz3_xess_2d optimal{}, minimum{}, maximum{};
    result = g_xess.functions.get_optimal_input_resolution(
        g_xess.context,
        &output,
        quality,
        &optimal,
        &minimum,
        &maximum);
    if (result != KZ3_XESS_SUCCESS ||
        render_width < minimum.x || render_width > maximum.x ||
        render_height < minimum.y || render_height > maximum.y) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        set_status(
            "XeSS-SR %s rejects %ux%u input for %ux%u output "
            "(result %d, optimal %ux%u, range %ux%u..%ux%u)",
            quality_name(quality),
            render_width,
            render_height,
            output_width,
            output_height,
            result,
            optimal.x,
            optimal.y,
            minimum.x,
            minimum.y,
            maximum.x,
            maximum.y);
        return 0;
    }

    kz3_xess_d3d12_init_params init{};
    init.output_resolution = output;
    init.quality_setting = quality;
    init.init_flags =
        KZ3_XESS_INIT_RESPONSIVE_PIXEL_MASK |
        KZ3_XESS_INIT_USE_NDC_VELOCITY |
        KZ3_XESS_INIT_LDR_INPUT_COLOR;
    result = g_xess.functions.init(g_xess.context, &init);
    if (result != KZ3_XESS_SUCCESS) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        set_status(
            "initializing XeSS-SR failed (%d: %s)",
            result,
            result_name(result));
        return 0;
    }
    if (!create_resources_locked(
            device,
            render_width,
            render_height,
            output_width,
            output_height)) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        return 0;
    }

    device->AddRef();
    g_xess.device = device;
    g_xess.render_width = render_width;
    g_xess.render_height = render_height;
    g_xess.output_width = output_width;
    g_xess.output_height = output_height;
    g_xess.quality = quality;
    g_xess.failed = false;
    set_status(
        "XeSS-SR API 2.0.2 %s initialized for %ux%u -> %ux%u",
        quality_name(quality),
        render_width,
        render_height,
        output_width,
        output_height);
    return 1;
}

extern "C" int kz3_xess_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y)
{
    if (!out_x || !out_y || !render_width ||
        output_width <= render_width) {
        std::lock_guard<std::mutex> lock(g_xess.mutex);
        set_status("XeSS-SR jitter query received invalid arguments");
        return 0;
    }
    const double scale =
        static_cast<double>(output_width) /
        static_cast<double>(render_width);
    /* Intel's guide: 8 phases times the squared scale, which is 72 at the 3x limit. */
    const uint32_t phase_count = std::clamp(
        static_cast<uint32_t>(
            std::ceil(8.0 * scale * scale)),
        8u,
        72u);
    const uint64_t index = frame_index % phase_count + 1u;
    *out_x = halton(index, 2) - 0.5f;
    *out_y = halton(index, 3) - 0.5f;
    return 1;
}

extern "C" void kz3_xess_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov)
{
    if (!pose || !kz3_xess_pose_valid(pose) ||
        !std::isfinite(horizontal_fov) ||
        !std::isfinite(vertical_fov) ||
        horizontal_fov <= 0.0f || horizontal_fov >= 3.13f ||
        vertical_fov <= 0.0f || vertical_fov >= 3.13f) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    if (!g_camera.handedness_logged) {
        g_camera.handedness_logged = true;
        fprintf(
            stderr,
            "[xess] camera basis: cross(right, forward).up = %+.3f "
            "(the reprojection expects +1)\n",
            static_cast<double>(kz3_xess_pose_handedness(pose)));
    }
    const uint64_t filled_before = g_camera.bridge.filled_flips;
    kz3_xess_bridge_note_camera(
        &g_camera.bridge,
        guest_flip,
        view_id,
        pose,
        horizontal_fov,
        vertical_fov);
    const uint64_t filled = g_camera.bridge.filled_flips;
    if (filled != filled_before && (filled <= 4u || filled % 100u == 0u)) {
        fprintf(
            stderr,
            "[xess] guest flip %llu: no camera was published for flip %llu; "
            "it is held at the previous camera (%llu so far)\n",
            static_cast<unsigned long long>(guest_flip),
            static_cast<unsigned long long>(guest_flip - 1u),
            static_cast<unsigned long long>(filled));
    }
}

extern "C" void kz3_xess_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane)
{
    if (!std::isfinite(near_plane) ||
        !std::isfinite(far_plane) ||
        near_plane < 0.001f || near_plane > 10.0f ||
        far_plane <= 10.0f || far_plane <= near_plane) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    kz3_xess_bridge_note_planes(
        &g_camera.bridge, guest_flip, near_plane, far_plane);
}

extern "C" int kz3_xess_get_camera(
    kz3_xess_camera_state* out_state)
{
    if (!out_state) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    *out_state = g_camera.bridge.state;
    return
        (out_state->flags & KZ3_XESS_CAMERA_CURRENT_VALID) != 0;
}

extern "C" int kz3_xess_get_camera_for_flip(
    uint64_t guest_flip,
    kz3_xess_camera_state* out_state)
{
    if (!out_state) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    return kz3_xess_bridge_find(
        &g_camera.bridge, guest_flip, out_state);
}

extern "C" int kz3_xess_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return capture_color_locked(
        command_list, color, width, height, true);
}

extern "C" int kz3_xess_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return capture_color_locked(
        command_list, color, width, height, false);
}

extern "C" int kz3_xess_capture_compose_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!g_xess.context || !command_list ||
        !texture_matches(
            color, width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        width != g_xess.render_width ||
        height != g_xess.render_height) {
        set_status("XeSS-SR compose capture received invalid state or color");
        return 0;
    }
    copy_texture_locked(
        command_list,
        color,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        g_xess.compose_color,
        g_xess.compose_color_state);
    return 1;
}

extern "C" int kz3_xess_capture_depth(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* depth,
    D3D12_RESOURCE_STATES depth_state,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!g_xess.context || !command_list ||
        !texture_matches(depth, width, height, DXGI_FORMAT_R32_FLOAT) ||
        width != g_xess.render_width ||
        height != g_xess.render_height) {
        set_status("XeSS-SR depth capture received invalid state or depth");
        return 0;
    }
    copy_texture_locked(
        command_list,
        depth,
        depth_state,
        g_xess.depth_hold,
        g_xess.depth_hold_state);
    g_xess.depth_captured = true;
    return 1;
}

extern "C" ID3D12Resource* kz3_xess_dispatch_camera(
    const kz3_xess_camera_dispatch_desc* desc)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!desc || !desc->command_list || !desc->color ||
        !g_xess.context ||
        !g_xess.motion_vectors || !g_xess.output ||
        !g_xess.motion_heap || !g_xess.motion_root_signature ||
        !g_xess.motion_pipeline) {
        set_status(
            "XeSS-SR camera dispatch received invalid state or resources");
        return nullptr;
    }
    /* One-frame input alignment (kz3_xess_inputs_delay_applies): this frame's colour shows the previous frame's
     * scene, so after a consecutive dispatch the previous frame's camera, jitter and captures go with it. */
    const bool delay = kz3_xess_inputs_delay_applies(
        input_delay_enabled(), g_xess.have_previous,
        g_xess.previous_camera.guest_flip, desc->camera.guest_flip);
    const kz3_xess_camera_state camera =
        delay ? g_xess.previous_camera : desc->camera;
    const float jitter_x = delay ? g_xess.previous_jitter_x : desc->jitter_x;
    const float jitter_y = delay ? g_xess.previous_jitter_y : desc->jitter_y;
    const kz3_xess_jitter_offset execute_jitter = delay ?
        g_xess.previous_execute_jitter : kz3_xess_execute_jitter(desc);
    const bool translucent = delay ?
        g_xess.previous_translucent : desc->use_responsive_mask != 0;
    const bool pair_now = hold_pair_coherent_locked();
    g_xess.have_previous = false;   /* set again by a successful dispatch */
    if (!std::isfinite(jitter_x) ||
        !std::isfinite(jitter_y) ||
        !std::isfinite(execute_jitter.x) ||
        !std::isfinite(execute_jitter.y)) {
        set_status("XeSS-SR camera dispatch has invalid jitter offsets");
        return nullptr;
    }
    const uint32_t required_camera =
        KZ3_XESS_CAMERA_CURRENT_VALID |
        KZ3_XESS_CAMERA_PLANES_VALID;
    if ((camera.flags & required_camera) != required_camera ||
        (camera.flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS)) {
        set_status(
            "XeSS-SR camera dispatch has no coherent single-view camera");
        return nullptr;
    }
    if (desc->render_width != g_xess.render_width ||
        desc->render_height != g_xess.render_height ||
        desc->output_width != g_xess.output_width ||
        desc->output_height != g_xess.output_height) {
        set_status(
            "XeSS-SR camera dispatch dimensions do not match its context");
        return nullptr;
    }
    if (!texture_matches(
            desc->color,
            desc->render_width,
            desc->render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM)) {
        set_status(
            "XeSS-SR camera dispatch color format is incompatible");
        return nullptr;
    }
    if (!g_xess.depth_captured) {
        set_status("XeSS-SR camera dispatch has no captured scene depth");
        return nullptr;
    }

    const bool has_previous =
        (camera.flags &
         KZ3_XESS_CAMERA_PREVIOUS_VALID) != 0;
    if (!kz3_xess_pose_valid(camera.current_pose) ||
        (has_previous &&
         !kz3_xess_pose_valid(camera.previous_pose)) ||
        !std::isfinite(camera.near_plane) ||
        !std::isfinite(camera.far_plane) ||
        !(camera.near_plane > 0.0f) ||
        !(camera.far_plane >
          camera.near_plane)) {
        set_status(
            "XeSS-SR camera dispatch has an invalid pose or projection");
        return nullptr;
    }
    const float* previous_pose = has_previous ?
        camera.previous_pose :
        camera.current_pose;
    const float previous_horizontal_fov = has_previous ?
        camera.previous_horizontal_fov :
        camera.current_horizontal_fov;
    const float previous_vertical_fov = has_previous ?
        camera.previous_vertical_fov :
        camera.current_vertical_fov;
    const float tangents[4] = {
        std::tan(camera.current_horizontal_fov * 0.5f),
        std::tan(camera.current_vertical_fov * 0.5f),
        std::tan(previous_horizontal_fov * 0.5f),
        std::tan(previous_vertical_fov * 0.5f)};
    if (!std::isfinite(tangents[0]) ||
        !std::isfinite(tangents[1]) ||
        !std::isfinite(tangents[2]) ||
        !std::isfinite(tangents[3]) ||
        !(tangents[0] > 0.0f) ||
        !(tangents[1] > 0.0f) ||
        !(tangents[2] > 0.0f) ||
        !(tangents[3] > 0.0f)) {
        set_status(
            "XeSS-SR camera dispatch has an invalid field of view");
        return nullptr;
    }

    D3D12_RESOURCE_BARRIER before[2]{};
    UINT before_count = 0;
    if (g_xess.motion_is_compute_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_xess.motion_vectors;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (g_xess.output_is_pixel_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_xess.output;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (before_count) {
        desc->command_list->ResourceBarrier(
            before_count, before);
    }
    g_xess.motion_is_compute_read = false;
    g_xess.output_is_pixel_read = false;

    if (!delay) {
        if (translucent && !pair_now) {
            set_status(
                "XeSS-SR responsive-mask dispatch has no current scene captures");
            return nullptr;
        }
        promote_holds_locked(desc->command_list);
    }

    /* The mask runs first: the motion pass reads its HUD pixels. */
    const bool use_hud = desc->use_hud_mask != 0;
    if (use_hud) {
        copy_texture_locked(
            desc->command_list,
            desc->color,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            g_xess.present_color,
            g_xess.present_color_state);
    }
    const bool responsive_ok = generate_responsive_locked(
        desc->command_list,
        desc->render_width,
        desc->render_height,
        translucent,
        use_hud);
    g_xess.opaque_capture_serial = 0;
    g_xess.pre_upscale_capture_serial = 0;
    g_xess.capture_source = nullptr;
    if (!responsive_ok) {
        return nullptr;
    }

    float constants[44]{};
    memcpy(
        constants,
        camera.current_pose,
        3 * 4 * sizeof(float));
    memcpy(
        constants + 12,
        previous_pose,
        3 * 4 * sizeof(float));
    memcpy(
        constants + 24,
        camera.current_pose + 12,
        3 * sizeof(float));
    memcpy(
        constants + 28,
        previous_pose + 12,
        3 * sizeof(float));
    memcpy(constants + 32, tangents, sizeof(tangents));
    constants[36] =
        static_cast<float>(desc->render_width);
    constants[37] =
        static_cast<float>(desc->render_height);
    constants[38] = jitter_x;
    constants[39] = jitter_y;
    constants[40] = camera.near_plane;
    constants[41] = camera.far_plane;

    ID3D12DescriptorHeap* heaps[] = {g_xess.motion_heap};
    desc->command_list->SetDescriptorHeaps(1, heaps);
    desc->command_list->SetPipelineState(g_xess.motion_pipeline);
    desc->command_list->SetComputeRootSignature(
        g_xess.motion_root_signature);
    desc->command_list->SetComputeRootDescriptorTable(
        0,
        g_xess.motion_heap->GetGPUDescriptorHandleForHeapStart());
    desc->command_list->SetComputeRoot32BitConstants(
        1, 44, constants, 0);
    desc->command_list->Dispatch(
        (desc->render_width + 7u) / 8u,
        (desc->render_height + 7u) / 8u,
        1);

    D3D12_RESOURCE_BARRIER motion_ready{};
    motion_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    motion_ready.Transition.pResource = g_xess.motion_vectors;
    motion_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    motion_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    motion_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &motion_ready);
    g_xess.motion_is_compute_read = true;

    kz3_xess_d3d12_execute_params execute{};
    execute.color_texture = desc->color;
    execute.velocity_texture = g_xess.motion_vectors;
    execute.depth_texture = g_xess.scene_depth;
    execute.responsive_pixel_mask_texture =
        g_xess.responsive_mask;
    execute.output_texture = g_xess.output;
    execute.jitter_offset_x = execute_jitter.x;
    execute.jitter_offset_y = execute_jitter.y;
    execute.exposure_scale = 1.0f;
    execute.reset_history =
        desc->reset ||
        (camera.flags & KZ3_XESS_CAMERA_RESET) ||
        !has_previous;
    execute.input_width = desc->render_width;
    execute.input_height = desc->render_height;
    if (g_xess.dispatch_count < 4u) {
        fprintf(
            stderr,
            "[xess] camera: guest flip %llu, view 0x%llX, flags 0x%X\n",
            static_cast<unsigned long long>(camera.guest_flip),
            static_cast<unsigned long long>(camera.view_id),
            camera.flags);
    }
    const int result = g_xess.functions.execute(
        g_xess.context, desc->command_list, &execute);
    g_xess.depth_captured = false;
    if (result != KZ3_XESS_SUCCESS) {
        set_status(
            "XeSS-SR dispatch failed (%d: %s)",
            result,
            result_name(result));
        return nullptr;
    }
    ++g_xess.dispatch_count;
    if (delay) {
        /* After XeSS read them: this frame's captures become the next dispatch's inputs. */
        promote_holds_locked(desc->command_list);
        ++g_xess.delayed_count;
    }
    g_xess.have_previous = true;
    g_xess.previous_translucent = desc->use_responsive_mask && pair_now;
    g_xess.previous_camera = desc->camera;
    g_xess.previous_jitter_x = desc->jitter_x;
    g_xess.previous_jitter_y = desc->jitter_y;
    g_xess.previous_execute_jitter = kz3_xess_execute_jitter(desc);
    if (execute.reset_history) {
        ++g_xess.history_reset_count;
    }
    if (g_xess.dispatch_count == 120u ||
        g_xess.dispatch_count % 600u == 0u) {
        fprintf(
            stderr,
            "[xess] history: %llu dispatches, %llu resets, %llu with inputs aligned to the previous frame\n",
            static_cast<unsigned long long>(g_xess.dispatch_count),
            static_cast<unsigned long long>(
                g_xess.history_reset_count),
            static_cast<unsigned long long>(g_xess.delayed_count));
    }

    D3D12_RESOURCE_BARRIER output_ready{};
    output_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_ready.Transition.pResource = g_xess.output;
    output_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &output_ready);
    g_xess.output_is_pixel_read = true;
    return g_xess.output;
}

namespace {

struct XessBridgeState {
    ID3D12Device* device;
    uint32_t frame;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t context_generation;
    uint32_t primary_depth_frame;
    uint32_t primary_depth_slot;
    uint32_t camera_frame;
    kz3_xess_camera_state camera;
    float jitter_x;
    float jitter_y;
    kz3_xess_pairing pairing;
    uint32_t camera_misses;
    uint32_t camera_repeats;
    uint32_t camera_jumps;
    int have_last_camera;
    uint64_t last_camera_flip;
    uint32_t last_dispatch_frame;
    uint32_t last_dispatch_context_generation;
    uintptr_t last_dispatch_resource;
    kz3_xess_capture_key opaque_capture;
    uint32_t responsive_frame;
    uint32_t responsive_context_generation;
    uint32_t compose_frame;
    uint32_t compose_context_generation;
    uintptr_t compose_resource;
    int jitter_hold;
};

XessBridgeState g_bridge{};

void bridge_reset()
{
    g_bridge = {};
    g_bridge.frame = UINT32_MAX;
    g_bridge.primary_depth_frame = UINT32_MAX;
    g_bridge.camera_frame = UINT32_MAX;
    g_bridge.last_dispatch_frame = UINT32_MAX;
    g_bridge.responsive_frame = UINT32_MAX;
    g_bridge.compose_frame = UINT32_MAX;
}

void bridge_output_size(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t requested_width,
    uint32_t requested_height,
    uint32_t* output_width,
    uint32_t* output_height)
{
    const uint32_t max_width =
        render_width <= UINT32_MAX / 3u ?
            render_width * 3u : UINT32_MAX;
    const uint32_t max_height =
        render_height <= UINT32_MAX / 3u ?
            render_height * 3u : UINT32_MAX;
    *output_width =
        requested_width < max_width ? requested_width : max_width;
    *output_height =
        requested_height < max_height ? requested_height : max_height;
}

bool bridge_context_matches(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    return kz3_xess_has_context() &&
           g_bridge.render_width == render_width &&
           g_bridge.render_height == render_height &&
           g_bridge.output_width == output_width &&
           g_bridge.output_height == output_height;
}

int bridge_camera_lag()
{
    static const int lag = [] {
        const char* value = getenv("KZ3_XESS_CAMERA_LAG");
        return value && *value ? atoi(value) : 0;
    }();
    return lag;
}

bool bridge_hud_mask_enabled()
{
    static const bool enabled = [] {
        const char* value = getenv("KZ3_XESS_HUD_MASK");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

int bridge_mip_bias(uint32_t render_width, uint32_t output_width)
{
    const char* value = getenv("KZ3_XESS_MIP_BIAS");
    if (!value || !*value) {
        return kz3_xess_mip_bias_fp(render_width, output_width);
    }
    const float bias = strtof(value, nullptr);
    return std::isfinite(bias) ?
        static_cast<int>(lroundf(bias * 256.0f)) : 0;
}

bool bridge_pick_camera(
    uint32_t frame,
    kz3_xess_camera_state* camera)
{
    kz3_xess_camera_state latest{};
    if (!camera || !kz3_xess_get_camera(&latest)) {
        return false;
    }
    *camera = latest;
    const int lag = bridge_camera_lag();
    if (lag >= 0) {
        const uint64_t wanted = kz3_xess_pair_flip(
            &g_bridge.pairing,
            latest.guest_flip,
            frame,
            static_cast<uint32_t>(lag));
        if (!kz3_xess_get_camera_for_flip(wanted, camera)) {
            *camera = latest;
            ++g_bridge.camera_misses;
        }
    }
    static uint32_t logs = 0;
    if (logs < 8u || frame % 600u == 0u) {
        ++logs;
        fprintf(
            stderr,
            "[xess] camera pairing: renderer frame %u, latest %llu, "
            "used %llu (repeats %u, jumps %u, misses %u)\n",
            frame,
            static_cast<unsigned long long>(latest.guest_flip),
            static_cast<unsigned long long>(camera->guest_flip),
            g_bridge.camera_repeats,
            g_bridge.camera_jumps,
            g_bridge.camera_misses);
    }
    return true;
}

kz3_xess_draw_info bridge_draw_info(
    const rsx_live_upscale_draw_info* info)
{
    return {
        info->frame,
        info->target_offset,
        info->target_width,
        info->target_height,
        info->mrt_count,
        info->depth_slot,
        info->sampled_render_target_offset,
        info->target_location == 0u,
        info->full_viewport,
        info->depth_test,
        info->blend,
        info->sampled_full_size_render_target};
}

bool bridge_is_compose(const rsx_live_upscale_draw_info* info)
{
    return info->display_target && info->first_display_draw &&
           info->depth_draw_seen && info->full_viewport;
}

void bridge_log_failure(uint32_t frame, const char* stage)
{
    static uint32_t logs = 0;
    if (logs++ < 8u || frame % 600u == 0u) {
        fprintf(
            stderr,
            "[xess] frame %u: %s failed; using Bilinear: %s\n",
            frame,
            stage,
            kz3_xess_status());
    }
}

void* bridge_create(void* device, const rsx_upscale_config*)
{
    if (!device) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_xess.mutex);
        if (!load_runtime_locked()) {
            fprintf(
                stderr,
                "[xess] XeSS-SR is unavailable: %s\n",
                g_xess.status.c_str());
            return nullptr;
        }
    }
    bridge_reset();
    g_bridge.device = static_cast<ID3D12Device*>(device);
    return &g_bridge;
}

void bridge_destroy(void*, void (*)(void*, void*), void*)
{
    kz3_xess_shutdown();
    bridge_reset();
}

int bridge_evaluate(
    void*,
    const rsx_upscale_eval* eval,
    float,
    rsx_upscale_result* result,
    void (*)(void*, void*),
    void*)
{
    if (!eval || !result || !eval->cmd_list ||
        !eval->input.resource || !eval->depth ||
        !eval->depth->resource || g_bridge.frame == UINT32_MAX) {
        return 0;
    }

    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bridge_output_size(
        eval->input.width,
        eval->input.height,
        eval->output_width,
        eval->output_height,
        &output_width,
        &output_height);
    if (output_width <= eval->input.width ||
        output_height <= eval->input.height ||
        !kz3_xess_aspect_matches(
            eval->input.width,
            eval->input.height,
            output_width,
            output_height) ||
        g_bridge.camera_frame != g_bridge.frame) {
        return 0;
    }

    const bool context_changed = !bridge_context_matches(
        eval->input.width,
        eval->input.height,
        output_width,
        output_height);
    if (!kz3_xess_initialize(
            g_bridge.device,
            eval->input.width,
            eval->input.height,
            output_width,
            output_height)) {
        bridge_log_failure(g_bridge.frame, "context initialization");
        return 0;
    }
    if (context_changed) {
        g_bridge.render_width = eval->input.width;
        g_bridge.render_height = eval->input.height;
        g_bridge.output_width = output_width;
        g_bridge.output_height = output_height;
        ++g_bridge.context_generation;
        g_bridge.responsive_frame = UINT32_MAX;
        g_bridge.compose_frame = UINT32_MAX;
    }

    auto* command_list =
        static_cast<ID3D12GraphicsCommandList*>(eval->cmd_list);
    auto* color =
        static_cast<ID3D12Resource*>(eval->input.resource);
    if (!kz3_xess_capture_depth(
            command_list,
            static_cast<ID3D12Resource*>(eval->depth->resource),
            static_cast<D3D12_RESOURCE_STATES>(eval->depth->state),
            eval->depth->width,
            eval->depth->height)) {
        bridge_log_failure(g_bridge.frame, "depth capture");
        return 0;
    }

    kz3_xess_camera_state camera = g_bridge.camera;
    const enum kz3_xess_camera_step camera_step =
        kz3_xess_camera_step_from(
            g_bridge.have_last_camera,
            g_bridge.last_camera_flip,
            camera.guest_flip);
    if (camera_step == KZ3_XESS_CAMERA_STEP_REPEAT) {
        kz3_xess_camera_hold(&camera);
        ++g_bridge.camera_repeats;
    } else if (camera_step == KZ3_XESS_CAMERA_STEP_JUMP) {
        ++g_bridge.camera_jumps;
    }

    const D3D12_RESOURCE_STATES input_state =
        static_cast<D3D12_RESOURCE_STATES>(eval->input.state);
    if (input_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = color;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = input_state;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        command_list->ResourceBarrier(1, &barrier);
    }

    kz3_xess_camera_dispatch_desc dispatch{};
    dispatch.command_list = command_list;
    dispatch.color = color;
    dispatch.camera = camera;
    dispatch.render_width = eval->input.width;
    dispatch.render_height = eval->input.height;
    dispatch.output_width = output_width;
    dispatch.output_height = output_height;
    dispatch.jitter_x = eval->jitter_x;
    dispatch.jitter_y = eval->jitter_y;
    dispatch.use_responsive_mask =
        g_bridge.responsive_frame == g_bridge.frame &&
        g_bridge.responsive_context_generation ==
            g_bridge.context_generation;
    dispatch.use_hud_mask =
        bridge_hud_mask_enabled() &&
        g_bridge.compose_frame == g_bridge.frame &&
        g_bridge.compose_context_generation ==
            g_bridge.context_generation &&
        g_bridge.compose_resource ==
            reinterpret_cast<uintptr_t>(color);
    dispatch.reset =
        eval->reset_history ||
        camera_step == KZ3_XESS_CAMERA_STEP_JUMP ||
        g_bridge.last_dispatch_frame == UINT32_MAX ||
        g_bridge.frame != g_bridge.last_dispatch_frame + 1u ||
        g_bridge.context_generation !=
            g_bridge.last_dispatch_context_generation ||
        g_bridge.last_dispatch_resource !=
            reinterpret_cast<uintptr_t>(color);

    ID3D12Resource* output =
        kz3_xess_dispatch_camera(&dispatch);
    if (input_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = color;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = input_state;
        command_list->ResourceBarrier(1, &barrier);
    }
    if (!output) {
        if (eval->jitter_x != 0.0f || eval->jitter_y != 0.0f) {
            g_bridge.jitter_hold = 1;
        }
        bridge_log_failure(g_bridge.frame, "dispatch");
        return 0;
    }

    result->resource = output;
    result->width = output_width;
    result->height = output_height;
    result->format = DXGI_FORMAT_R8G8B8A8_UNORM;
    result->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_bridge.last_dispatch_frame = g_bridge.frame;
    g_bridge.last_dispatch_context_generation =
        g_bridge.context_generation;
    g_bridge.last_dispatch_resource =
        reinterpret_cast<uintptr_t>(color);
    g_bridge.last_camera_flip = camera.guest_flip;
    g_bridge.have_last_camera = 1;
    g_bridge.jitter_hold = 0;

    static uint32_t success_logs = 0;
    if (success_logs++ < 4u) {
        fprintf(
            stderr,
            "[xess] frame %u: XeSS-SR 2.0.2 %ux%u -> %ux%u, "
            "depth-aware camera motion, responsive=%s, hud=%s, "
            "mip bias %.3f, jitter %.4f/%.4f, reset=%u\n",
            g_bridge.frame,
            eval->input.width,
            eval->input.height,
            output_width,
            output_height,
            dispatch.use_responsive_mask ? "generated" : "none",
            dispatch.use_hud_mask ? "masked" : "none",
            bridge_mip_bias(eval->input.width, output_width) / 256.0f,
            dispatch.jitter_x,
            dispatch.jitter_y,
            dispatch.reset);
    }
    return 1;
}

void bridge_inspect_draw(
    void*,
    const rsx_live_upscale_draw_info* info,
    rsx_live_upscale_draw_action* action)
{
    if (!info || !action || !g_bridge.device) {
        return;
    }
    if (g_bridge.frame != info->frame) {
        g_bridge.frame = info->frame;
        g_bridge.camera_frame = UINT32_MAX;
        g_bridge.jitter_x = 0.0f;
        g_bridge.jitter_y = 0.0f;
    }

    const kz3_xess_draw_info draw = bridge_draw_info(info);
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bridge_output_size(
        info->render_width,
        info->render_height,
        info->output_width,
        info->output_height,
        &output_width,
        &output_height);
    const bool context_ready = bridge_context_matches(
        info->render_width,
        info->render_height,
        output_width,
        output_height);
    if (kz3_xess_is_deferred_primary(
            &draw, info->render_width, info->render_height)) {
        g_bridge.primary_depth_frame = info->frame;
        g_bridge.primary_depth_slot = info->depth_slot;
        action->select_depth = 1;
    }
    if (kz3_xess_should_jitter(
            &draw,
            info->render_width,
            info->render_height,
            g_bridge.primary_depth_frame,
            g_bridge.primary_depth_slot)) {
        if (g_bridge.camera_frame != info->frame) {
            kz3_xess_camera_state camera{};
            if (output_width > info->render_width &&
                output_height > info->render_height &&
                bridge_pick_camera(info->frame, &camera) &&
                (camera.flags & KZ3_XESS_CAMERA_PLANES_VALID) &&
                !(camera.flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS) &&
                kz3_xess_get_jitter(
                    info->frame,
                    info->render_width,
                    output_width,
                    &g_bridge.jitter_x,
                    &g_bridge.jitter_y)) {
                g_bridge.camera = camera;
                g_bridge.camera_frame = info->frame;
            }
        }
        if (context_ready &&
            g_bridge.camera_frame == info->frame &&
            !g_bridge.jitter_hold) {
            action->apply_jitter = 1;
            action->jitter_x = g_bridge.jitter_x;
            action->jitter_y = g_bridge.jitter_y;
            action->mip_bias_fp =
                bridge_mip_bias(info->render_width, output_width);
        }
    }
    action->capture_after_draw =
        context_ready &&
        (kz3_xess_is_opaque_resolve(
             &draw, info->render_width, info->render_height) ||
         bridge_is_compose(info));
}

void bridge_after_draw(
    void*,
    const rsx_live_upscale_draw_info* info)
{
    if (!info || !info->command_list || !info->color_resource) {
        return;
    }
    const kz3_xess_draw_info draw = bridge_draw_info(info);
    if (kz3_xess_is_opaque_resolve(
            &draw, info->render_width, info->render_height)) {
        const kz3_xess_capture_key key = {
            info->frame,
            g_bridge.context_generation,
            info->target,
            info->target_width,
            info->target_height,
            reinterpret_cast<uintptr_t>(info->color_resource)};
        if ((g_bridge.opaque_capture.frame != key.frame ||
             g_bridge.opaque_capture.context_generation !=
                 key.context_generation ||
             g_bridge.opaque_capture.resource != key.resource) &&
            kz3_xess_capture_opaque_color(
                static_cast<ID3D12GraphicsCommandList*>(
                    info->command_list),
                static_cast<ID3D12Resource*>(
                    info->color_resource),
                info->target_width,
                info->target_height)) {
            g_bridge.opaque_capture = key;
        }
    }
    if (bridge_hud_mask_enabled() && bridge_is_compose(info) &&
        kz3_xess_capture_compose_color(
            static_cast<ID3D12GraphicsCommandList*>(
                info->command_list),
            static_cast<ID3D12Resource*>(info->color_resource),
            info->target_width,
            info->target_height)) {
        g_bridge.compose_frame = info->frame;
        g_bridge.compose_context_generation =
            g_bridge.context_generation;
        g_bridge.compose_resource =
            reinterpret_cast<uintptr_t>(info->color_resource);
    }
}

void bridge_before_writeback(
    void*,
    const rsx_live_upscale_surface_info* info,
    rsx_live_upscale_surface_action* action)
{
    if (!info || !action ||
        !kz3_xess_is_scene_target(
            info->offset,
            info->width,
            info->height,
            info->location == 0u,
            info->render_width,
            info->render_height)) {
        return;
    }
    if (!g_bridge.jitter_hold &&
        g_bridge.camera_frame == info->frame &&
        (g_bridge.jitter_x != 0.0f ||
         g_bridge.jitter_y != 0.0f)) {
        action->suppress_host_aa = 1;
    }
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    bridge_output_size(
        info->render_width,
        info->render_height,
        info->output_width,
        info->output_height,
        &output_width,
        &output_height);
    if (!info->command_list || !info->color_resource ||
        !bridge_context_matches(
            info->render_width,
            info->render_height,
            output_width,
            output_height)) {
        return;
    }
    const kz3_xess_capture_key key = {
        info->frame,
        g_bridge.context_generation,
        info->target,
        info->width,
        info->height,
        reinterpret_cast<uintptr_t>(info->color_resource)};
    if (!kz3_xess_capture_pair_matches(
            &g_bridge.opaque_capture, &key) ||
        !kz3_xess_capture_pre_upscale_color(
            static_cast<ID3D12GraphicsCommandList*>(
                info->command_list),
            static_cast<ID3D12Resource*>(info->color_resource),
            info->width,
            info->height)) {
        return;
    }
    g_bridge.responsive_frame = info->frame;
    g_bridge.responsive_context_generation =
        g_bridge.context_generation;
}

const rsx_upscale_ops kBridgeOps = {
    bridge_create,
    bridge_destroy,
    bridge_evaluate};

const rsx_live_upscale_hooks kBridgeHooks = {
    RSX_UPSCALE_XESS,
    nullptr,
    bridge_inspect_draw,
    bridge_after_draw,
    bridge_before_writeback};

}  // namespace

extern "C" int kz3_xess_register(void)
{
    if (!rsx_upscale_register_backend(
            RSX_UPSCALE_XESS, &kBridgeOps)) {
        return 0;
    }
    rsx_live_draw_set_upscale_hooks(&kBridgeHooks);
    return 1;
}

extern "C" void kz3_xess_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    destroy_context_locked();
    if (g_xess.module) {
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
    }
    g_xess.failed = false;
    set_status("XeSS-SR is shut down");
}

extern "C" const char* kz3_xess_status(void)
{
    thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    copy = g_xess.status;
    return copy.c_str();
}

extern "C" ID3D12Resource* kz3_xess_debug_motion_vectors(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return g_xess.motion_vectors;
}

extern "C" ID3D12Resource* kz3_xess_debug_responsive_mask(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return g_xess.responsive_mask;
}
