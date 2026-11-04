#include "fsr31.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

namespace {

constexpr const char* kRequiredProvider = "3.1.4";
constexpr wchar_t kRuntimeDll[] = L"amd_fidelityfx_dx12.dll";

struct FsrState {
    std::mutex mutex;
    HMODULE module = nullptr;
    ffxFunctions functions{};
    ffxContext context = nullptr;
    ID3D12Device* device = nullptr;
    uint32_t max_render_width = 0;
    uint32_t max_render_height = 0;
    uint32_t max_output_width = 0;
    uint32_t max_output_height = 0;
    uint32_t flags = 0;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* opaque_color = nullptr;
    ID3D12Resource* pre_upscale_color = nullptr;
    ID3D12Resource* reactive_mask = nullptr;
    ID3D12DescriptorHeap* motion_heap = nullptr;
    ID3D12RootSignature* motion_root_signature = nullptr;
    ID3D12PipelineState* motion_pipeline = nullptr;
    D3D12_RESOURCE_STATES opaque_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES pre_upscale_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool motion_is_compute_read = false;
    bool output_is_pixel_read = false;
    bool reactive_is_compute_read = false;
    uint64_t capture_serial = 0;
    uint64_t opaque_capture_serial = 0;
    uint64_t pre_upscale_capture_serial = 0;
    ID3D12Resource* capture_source = nullptr;
    std::string status = "FSR 3.1 has not been initialized";
};

FsrState g_fsr;

struct CameraBridge {
    std::mutex mutex;
    kz3_fsr31_camera_state state{};
    uint64_t plane_flip = UINT64_MAX;
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    bool planes_ambiguous = false;
};

CameraBridge g_camera;

void set_status(const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_fsr.status = message;
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

void ffx_message(uint32_t type, const wchar_t* message)
{
    if (!message) {
        return;
    }
    char utf8[1024] = {};
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        message,
        -1,
        utf8,
        static_cast<int>(sizeof(utf8)),
        nullptr,
        nullptr);
    fprintf(
        stderr,
        "[fsr31] %s: %s\n",
        type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
        converted ? utf8 : "(message conversion failed)");
}

template <typename T>
void release(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void destroy_camera_resources_locked()
{
    release(g_fsr.motion_pipeline);
    release(g_fsr.motion_root_signature);
    release(g_fsr.motion_heap);
    release(g_fsr.reactive_mask);
    release(g_fsr.pre_upscale_color);
    release(g_fsr.opaque_color);
    release(g_fsr.output);
    release(g_fsr.motion_vectors);
    g_fsr.opaque_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_fsr.pre_upscale_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_fsr.motion_is_compute_read = false;
    g_fsr.output_is_pixel_read = false;
    g_fsr.reactive_is_compute_read = false;
    g_fsr.capture_serial = 0;
    g_fsr.opaque_capture_serial = 0;
    g_fsr.pre_upscale_capture_serial = 0;
    g_fsr.capture_source = nullptr;
}

void destroy_context_locked()
{
    destroy_camera_resources_locked();
    if (g_fsr.context && g_fsr.functions.DestroyContext) {
        const ffxReturnCode_t rc =
            g_fsr.functions.DestroyContext(&g_fsr.context, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            fprintf(
                stderr,
                "[fsr31] ffxDestroyContext failed (%u)\n",
                static_cast<unsigned>(rc));
        }
        g_fsr.context = nullptr;
    }
    if (g_fsr.device) {
        g_fsr.device->Release();
        g_fsr.device = nullptr;
    }
    g_fsr.max_render_width = 0;
    g_fsr.max_render_height = 0;
    g_fsr.max_output_width = 0;
    g_fsr.max_output_height = 0;
    g_fsr.flags = 0;
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
            "creating an FSR 3.1 texture failed (HRESULT 0x%08lX)",
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
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "  uint2 size = (uint2)sizeJitter.xy;\n"
        "  if (id.x >= size.x || id.y >= size.y) return;\n"
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
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        source,
        sizeof(source) - 1,
        "kz3_fsr31_motion",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "compiling the FSR 3.1 motion shader failed: %s",
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        errors->Release();
    }
    if (FAILED(hr)) {
        release(shader);
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 44;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;

    ID3DBlob* signature = nullptr;
    errors = nullptr;
    hr = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "serializing the FSR 3.1 motion root signature failed: %s",
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
            reinterpret_cast<void**>(&g_fsr.motion_root_signature));
    }
    release(signature);

    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = g_fsr.motion_root_signature;
        pipeline.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline.CS.BytecodeLength = shader->GetBufferSize();
        hr = device->CreateComputePipelineState(
            &pipeline,
            IID_ID3D12PipelineState,
            reinterpret_cast<void**>(&g_fsr.motion_pipeline));
    }
    release(shader);
    if (FAILED(hr)) {
        set_status(
            "creating the FSR 3.1 motion pipeline failed (HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(
        &heap,
        IID_ID3D12DescriptorHeap,
        reinterpret_cast<void**>(&g_fsr.motion_heap));
    if (FAILED(hr)) {
        set_status(
            "creating the FSR 3.1 motion descriptor heap failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_camera_resources_locked(
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
            &g_fsr.motion_vectors) ||
        !create_texture(
            device,
            output_width,
            output_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_fsr.output) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_fsr.opaque_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_fsr.pre_upscale_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8_UNORM,
            &g_fsr.reactive_mask) ||
        !create_motion_pipeline_locked(device)) {
        destroy_camera_resources_locked();
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        g_fsr.motion_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(
        g_fsr.motion_vectors,
        nullptr,
        &uav,
        handle);
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

bool capture_color_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height,
    bool opaque)
{
    if (!g_fsr.context || !command_list ||
        !texture_matches(
            color, width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        width != g_fsr.max_render_width ||
        height != g_fsr.max_render_height) {
        set_status("FSR 3.1 scene capture received invalid state or color");
        return false;
    }
    if (!opaque &&
        (!g_fsr.opaque_capture_serial ||
         g_fsr.capture_source != color ||
         g_fsr.pre_upscale_capture_serial ==
             g_fsr.opaque_capture_serial)) {
        set_status(
            "FSR 3.1 pre-upscale capture has no matching opaque color");
        return false;
    }

    ID3D12Resource* destination =
        opaque ? g_fsr.opaque_color : g_fsr.pre_upscale_color;
    D3D12_RESOURCE_STATES& destination_state =
        opaque ? g_fsr.opaque_color_state : g_fsr.pre_upscale_color_state;
    if (!destination) {
        set_status("FSR 3.1 scene-capture resources are unavailable");
        return false;
    }

    D3D12_RESOURCE_BARRIER before[2]{};
    before[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before[0].Transition.pResource = color;
    before[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    before[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    before[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before[1].Transition.pResource = destination;
    before[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    before[1].Transition.StateBefore = destination_state;
    before[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(2, before);
    command_list->CopyResource(destination, color);

    before[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    before[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    before[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    before[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(2, before);
    destination_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (opaque) {
        if (!++g_fsr.capture_serial) {
            ++g_fsr.capture_serial;
        }
        g_fsr.opaque_capture_serial = g_fsr.capture_serial;
        g_fsr.pre_upscale_capture_serial = 0;
        g_fsr.capture_source = color;
    } else {
        g_fsr.pre_upscale_capture_serial = g_fsr.opaque_capture_serial;
    }
    return true;
}

bool generate_reactive_locked(
    ID3D12GraphicsCommandList* command_list,
    uint32_t width,
    uint32_t height)
{
    if (!g_fsr.context || !command_list || !g_fsr.reactive_mask ||
        !g_fsr.opaque_color || !g_fsr.pre_upscale_color ||
        !g_fsr.opaque_capture_serial ||
        g_fsr.pre_upscale_capture_serial !=
            g_fsr.opaque_capture_serial) {
        set_status(
            "FSR 3.1 reactive-mask dispatch has no current scene captures");
        return false;
    }

    if (g_fsr.reactive_is_compute_read) {
        D3D12_RESOURCE_BARRIER writable{};
        writable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        writable.Transition.pResource = g_fsr.reactive_mask;
        writable.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        writable.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        writable.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        command_list->ResourceBarrier(1, &writable);
        g_fsr.reactive_is_compute_read = false;
    }

    ffxDispatchDescUpscaleGenerateReactiveMask reactive{};
    reactive.header.type =
        FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
    reactive.commandList = command_list;
    reactive.colorOpaqueOnly = ffxApiGetResourceDX12(
        g_fsr.opaque_color,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    reactive.colorPreUpscale = ffxApiGetResourceDX12(
        g_fsr.pre_upscale_color,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    reactive.outReactive = ffxApiGetResourceDX12(
        g_fsr.reactive_mask,
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
        FFX_API_RESOURCE_USAGE_UAV);
    reactive.renderSize = {width, height};
    reactive.scale = 1.0f;
    reactive.cutoffThreshold = 0.2f;
    reactive.binaryValue = 0.9f;
    reactive.flags =
        FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP |
        FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
        FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
    const ffxReturnCode_t rc =
        g_fsr.functions.Dispatch(&g_fsr.context, &reactive.header);
    if (rc != FFX_API_RETURN_OK) {
        set_status(
            "FidelityFX reactive-mask dispatch failed (%u)",
            static_cast<unsigned>(rc));
        return false;
    }

    D3D12_RESOURCE_BARRIER readable{};
    readable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readable.Transition.pResource = g_fsr.reactive_mask;
    readable.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    readable.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    readable.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &readable);
    g_fsr.reactive_is_compute_read = true;
    return true;
}

bool load_runtime_locked()
{
    if (g_fsr.module) {
        return true;
    }

    std::vector<wchar_t> path(32768);
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
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
    const size_t directory_length = static_cast<size_t>(slash - path.data() + 1);
    if (directory_length + std::size(kRuntimeDll) > path.size()) {
        set_status("the FidelityFX runtime path is too long");
        return false;
    }
    memcpy(
        path.data() + directory_length,
        kRuntimeDll,
        sizeof(kRuntimeDll));

    g_fsr.module = LoadLibraryExW(
        path.data(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_fsr.module) {
        set_win32_status("loading amd_fidelityfx_dx12.dll beside the executable");
        return false;
    }

    ffxLoadFunctions(&g_fsr.functions, g_fsr.module);
    if (!g_fsr.functions.CreateContext ||
        !g_fsr.functions.DestroyContext ||
        !g_fsr.functions.Configure ||
        !g_fsr.functions.Query ||
        !g_fsr.functions.Dispatch) {
        set_status("amd_fidelityfx_dx12.dll is missing required FFX API exports");
        FreeLibrary(g_fsr.module);
        g_fsr.module = nullptr;
        g_fsr.functions = {};
        return false;
    }
    return true;
}

bool find_provider_locked(ID3D12Device* device, uint64_t* out_version_id)
{
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = device;
    query.outputCount = &count;
    ffxReturnCode_t rc = g_fsr.functions.Query(nullptr, &query.header);
    if (rc != FFX_API_RETURN_OK || !count) {
        set_status(
            "querying FidelityFX upscaler providers failed (%u, count %llu)",
            static_cast<unsigned>(rc),
            static_cast<unsigned long long>(count));
        return false;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    rc = g_fsr.functions.Query(nullptr, &query.header);
    if (rc != FFX_API_RETURN_OK) {
        set_status(
            "enumerating FidelityFX upscaler providers failed (%u)",
            static_cast<unsigned>(rc));
        return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (names[i] && strcmp(names[i], kRequiredProvider) == 0) {
            *out_version_id = ids[i];
            return true;
        }
    }

    std::string available;
    for (uint64_t i = 0; i < count; ++i) {
        if (i) {
            available += ", ";
        }
        available += names[i] ? names[i] : "(unnamed)";
    }
    set_status(
        "FidelityFX upscaler provider %s is unavailable (found: %s)",
        kRequiredProvider,
        available.c_str());
    return false;
}

bool dimensions_valid(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    return render_width && render_height && output_width && output_height &&
           render_width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           render_height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           output_width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           output_height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

bool dispatch_resources_valid(const kz3_fsr31_dispatch_desc* desc)
{
    return desc && desc->command_list && desc->color && desc->depth &&
           desc->motion_vectors && desc->output &&
           dimensions_valid(
               desc->render_width,
               desc->render_height,
               desc->output_width,
               desc->output_height) &&
           desc->frame_time_ms > 0.0f && desc->camera_near > 0.0f &&
           desc->camera_far > 0.0f &&
           desc->camera_near != desc->camera_far &&
           desc->camera_vertical_fov > 0.0f &&
           desc->view_space_to_meters > 0.0f &&
           desc->sharpness >= 0.0f && desc->sharpness <= 1.0f;
}

bool pose_valid(const float pose[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        if (!std::isfinite(pose[i])) {
            return false;
        }
    }
    for (unsigned row = 0; row < 3; ++row) {
        const float* v = pose + row * 4;
        const float length_squared =
            v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (!(length_squared > 0.81f && length_squared < 1.21f)) {
            return false;
        }
    }
    return true;
}

bool camera_cut(
    const float previous[16],
    const float current[16])
{
    const float dx = current[12] - previous[12];
    const float dy = current[13] - previous[13];
    const float dz = current[14] - previous[14];
    if (dx * dx + dy * dy + dz * dz > 25.0f) {
        return true;
    }
    const float forward_dot = std::clamp(
        previous[4] * current[4] +
            previous[5] * current[5] +
            previous[6] * current[6],
        -1.0f,
        1.0f);
    return forward_dot < std::cos(1.5f);
}

bool projection_plane_matches(float left, float right)
{
    const float scale =
        std::max(1.0f, std::max(std::fabs(left), std::fabs(right)));
    return std::fabs(left - right) <= scale * 1.0e-4f;
}

int dispatch_locked(const kz3_fsr31_dispatch_desc* desc)
{
    if (!g_fsr.context) {
        set_status("FSR 3.1 dispatch requested without an initialized context");
        return 0;
    }
    if (!dispatch_resources_valid(desc)) {
        set_status("FSR 3.1 dispatch received invalid resources or parameters");
        return 0;
    }
    if (desc->render_width > g_fsr.max_render_width ||
        desc->render_height > g_fsr.max_render_height ||
        desc->output_width > g_fsr.max_output_width ||
        desc->output_height > g_fsr.max_output_height) {
        set_status("FSR 3.1 dispatch dimensions exceed the context limits");
        return 0;
    }

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = desc->command_list;
    dispatch.color = ffxApiGetResourceDX12(
        desc->color,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(
        desc->depth,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(
        desc->motion_vectors,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.reactive = ffxApiGetResourceDX12(
        desc->reactive,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.transparencyAndComposition = ffxApiGetResourceDX12(
        desc->transparency_and_composition,
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxApiGetResourceDX12(
        desc->output,
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
        FFX_API_RESOURCE_USAGE_UAV);
    dispatch.jitterOffset = {desc->jitter_x, desc->jitter_y};
    dispatch.motionVectorScale = {
        desc->motion_vector_scale_x,
        desc->motion_vector_scale_y};
    dispatch.renderSize = {desc->render_width, desc->render_height};
    dispatch.upscaleSize = {desc->output_width, desc->output_height};
    dispatch.enableSharpening = desc->sharpness > 0.0f;
    dispatch.sharpness = desc->sharpness;
    dispatch.frameTimeDelta = desc->frame_time_ms;
    dispatch.preExposure = 1.0f;
    dispatch.reset = desc->reset != 0;
    dispatch.cameraNear = desc->camera_near;
    dispatch.cameraFar = desc->camera_far;
    dispatch.cameraFovAngleVertical = desc->camera_vertical_fov;
    dispatch.viewSpaceToMetersFactor = desc->view_space_to_meters;

    const ffxReturnCode_t rc =
        g_fsr.functions.Dispatch(&g_fsr.context, &dispatch.header);
    if (rc != FFX_API_RETURN_OK) {
        set_status(
            "FidelityFX upscaler dispatch failed (%u)",
            static_cast<unsigned>(rc));
        return 0;
    }
    return 1;
}

} // namespace

extern "C" int kz3_fsr31_is_supported(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    if (!device) {
        set_status("FSR 3.1 support query received no D3D12 device");
        return 0;
    }
    if (!load_runtime_locked()) {
        return 0;
    }
    uint64_t version_id = 0;
    if (!find_provider_locked(device, &version_id)) {
        return 0;
    }
    set_status("FidelityFX upscaler provider %s is available", kRequiredProvider);
    return 1;
}

extern "C" int kz3_fsr31_initialize(
    ID3D12Device* device,
    uint32_t max_render_width,
    uint32_t max_render_height,
    uint32_t max_output_width,
    uint32_t max_output_height,
    uint32_t flags)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    if (!device) {
        set_status("FSR 3.1 initialization received no D3D12 device");
        return 0;
    }
    if (!dimensions_valid(
            max_render_width,
            max_render_height,
            max_output_width,
            max_output_height)) {
        set_status("FSR 3.1 initialization received invalid dimensions");
        return 0;
    }
    if (g_fsr.context && g_fsr.device == device &&
        g_fsr.max_render_width == max_render_width &&
        g_fsr.max_render_height == max_render_height &&
        g_fsr.max_output_width == max_output_width &&
        g_fsr.max_output_height == max_output_height &&
        g_fsr.flags == flags) {
        return 1;
    }

    destroy_context_locked();
    if (!load_runtime_locked()) {
        return 0;
    }

    uint64_t version_id = 0;
    if (!find_provider_locked(device, &version_id)) {
        return 0;
    }

    uint32_t ffx_flags = 0;
    if (flags & KZ3_FSR31_CREATE_DEPTH_INVERTED) {
        ffx_flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    }
    if (flags & KZ3_FSR31_CREATE_DEPTH_INFINITE) {
        ffx_flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    }
    if (flags & KZ3_FSR31_CREATE_AUTO_EXPOSURE) {
        ffx_flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    }
    if (flags & KZ3_FSR31_CREATE_DEBUG_CHECKING) {
        ffx_flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    }

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.flags = ffx_flags;
    create.maxRenderSize = {max_render_width, max_render_height};
    create.maxUpscaleSize = {max_output_width, max_output_height};
    create.fpMessage = ffx_message;

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    ffxOverrideVersion version{};
    version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version.versionId = version_id;

    create.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    const ffxReturnCode_t rc =
        g_fsr.functions.CreateContext(&g_fsr.context, &create.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !g_fsr.context) {
        g_fsr.context = nullptr;
        set_status(
            "creating FidelityFX upscaler context failed (%u)",
            static_cast<unsigned>(rc));
        return 0;
    }

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const ffxReturnCode_t provider_rc =
        g_fsr.functions.Query(&g_fsr.context, &provider.header);
    if (provider_rc != FFX_API_RETURN_OK || !provider.versionName ||
        strcmp(provider.versionName, kRequiredProvider) != 0) {
        g_fsr.functions.DestroyContext(&g_fsr.context, nullptr);
        g_fsr.context = nullptr;
        set_status(
            "FidelityFX created the wrong provider (%u, %s)",
            static_cast<unsigned>(provider_rc),
            provider.versionName ? provider.versionName : "unnamed");
        return 0;
    }

    if (!create_camera_resources_locked(
            device,
            max_render_width,
            max_render_height,
            max_output_width,
            max_output_height)) {
        g_fsr.functions.DestroyContext(&g_fsr.context, nullptr);
        g_fsr.context = nullptr;
        return 0;
    }

    device->AddRef();
    g_fsr.device = device;
    g_fsr.max_render_width = max_render_width;
    g_fsr.max_render_height = max_render_height;
    g_fsr.max_output_width = max_output_width;
    g_fsr.max_output_height = max_output_height;
    g_fsr.flags = flags;
    set_status(
        "FidelityFX upscaler %s initialized for %ux%u -> %ux%u",
        provider.versionName,
        max_render_width,
        max_render_height,
        max_output_width,
        max_output_height);
    return 1;
}

extern "C" int kz3_fsr31_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    if (!out_x || !out_y || !render_width || !output_width) {
        set_status("FSR 3.1 jitter query received invalid state or arguments");
        return 0;
    }
    if (!load_runtime_locked()) {
        return 0;
    }

    int32_t phase_count = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount phase{};
    phase.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phase.renderWidth = render_width;
    phase.displayWidth = output_width;
    phase.pOutPhaseCount = &phase_count;
    ffxReturnCode_t rc = g_fsr.functions.Query(nullptr, &phase.header);
    if (rc != FFX_API_RETURN_OK || phase_count <= 0) {
        set_status(
            "querying the FSR 3.1 jitter phase count failed (%u, count %d)",
            static_cast<unsigned>(rc),
            phase_count);
        return 0;
    }

    ffxQueryDescUpscaleGetJitterOffset jitter{};
    jitter.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jitter.index = static_cast<int32_t>(
        frame_index % static_cast<uint64_t>(phase_count));
    jitter.phaseCount = phase_count;
    jitter.pOutX = out_x;
    jitter.pOutY = out_y;
    rc = g_fsr.functions.Query(nullptr, &jitter.header);
    if (rc != FFX_API_RETURN_OK) {
        set_status(
            "querying the FSR 3.1 jitter offset failed (%u)",
            static_cast<unsigned>(rc));
        return 0;
    }
    return 1;
}

extern "C" void kz3_fsr31_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov)
{
    if (!pose || !pose_valid(pose) ||
        !std::isfinite(horizontal_fov) ||
        !std::isfinite(vertical_fov) ||
        horizontal_fov <= 0.0f || horizontal_fov >= 3.13f ||
        vertical_fov <= 0.0f || vertical_fov >= 3.13f) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    kz3_fsr31_camera_state& state = g_camera.state;
    if ((state.flags & KZ3_FSR31_CAMERA_CURRENT_VALID) &&
        state.guest_flip == guest_flip) {
        if (state.view_id != view_id) {
            state.flags |= KZ3_FSR31_CAMERA_MULTIPLE_VIEWS |
                           KZ3_FSR31_CAMERA_RESET;
            return;
        }
        memcpy(state.current_pose, pose, sizeof(state.current_pose));
        state.current_horizontal_fov = horizontal_fov;
        state.current_vertical_fov = vertical_fov;
        return;
    }

    kz3_fsr31_camera_state next{};
    next.guest_flip = guest_flip;
    next.view_id = view_id;
    memcpy(next.current_pose, pose, sizeof(next.current_pose));
    next.current_horizontal_fov = horizontal_fov;
    next.current_vertical_fov = vertical_fov;
    next.flags = KZ3_FSR31_CAMERA_CURRENT_VALID |
                 KZ3_FSR31_CAMERA_RESET;

    if ((state.flags & KZ3_FSR31_CAMERA_CURRENT_VALID) &&
        state.guest_flip + 1u == guest_flip &&
        state.view_id == view_id &&
        !(state.flags & KZ3_FSR31_CAMERA_MULTIPLE_VIEWS)) {
        memcpy(next.previous_pose, state.current_pose, sizeof(next.previous_pose));
        next.previous_horizontal_fov = state.current_horizontal_fov;
        next.previous_vertical_fov = state.current_vertical_fov;
        next.flags |= KZ3_FSR31_CAMERA_PREVIOUS_VALID;
        if (!camera_cut(next.previous_pose, next.current_pose)) {
            next.flags &= ~KZ3_FSR31_CAMERA_RESET;
        }
    }

    if (g_camera.plane_flip == guest_flip &&
        !g_camera.planes_ambiguous) {
        next.near_plane = g_camera.near_plane;
        next.far_plane = g_camera.far_plane;
        next.flags |= KZ3_FSR31_CAMERA_PLANES_VALID;
    }
    state = next;
}

extern "C" void kz3_fsr31_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane)
{
    if (!std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        near_plane < 0.001f || near_plane > 10.0f ||
        far_plane <= 10.0f || far_plane <= near_plane) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    if (g_camera.plane_flip != guest_flip) {
        g_camera.plane_flip = guest_flip;
        g_camera.near_plane = near_plane;
        g_camera.far_plane = far_plane;
        g_camera.planes_ambiguous = false;
    } else if (!projection_plane_matches(
                   g_camera.near_plane, near_plane) ||
               !projection_plane_matches(
                   g_camera.far_plane, far_plane)) {
        g_camera.planes_ambiguous = true;
    }
    if ((g_camera.state.flags & KZ3_FSR31_CAMERA_CURRENT_VALID) &&
        g_camera.state.guest_flip == guest_flip) {
        if (g_camera.planes_ambiguous) {
            g_camera.state.flags &= ~KZ3_FSR31_CAMERA_PLANES_VALID;
        } else {
            g_camera.state.near_plane = g_camera.near_plane;
            g_camera.state.far_plane = g_camera.far_plane;
            g_camera.state.flags |= KZ3_FSR31_CAMERA_PLANES_VALID;
        }
    }
}

extern "C" int kz3_fsr31_get_camera(kz3_fsr31_camera_state* out_state)
{
    if (!out_state) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    *out_state = g_camera.state;
    return (out_state->flags & KZ3_FSR31_CAMERA_CURRENT_VALID) != 0;
}

extern "C" int kz3_fsr31_dispatch(const kz3_fsr31_dispatch_desc* desc)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    return dispatch_locked(desc);
}

extern "C" int kz3_fsr31_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    return capture_color_locked(
        command_list, color, width, height, true);
}

extern "C" int kz3_fsr31_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    return capture_color_locked(
        command_list, color, width, height, false);
}

extern "C" ID3D12Resource* kz3_fsr31_dispatch_camera(
    const kz3_fsr31_camera_dispatch_desc* desc)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    if (!desc || !desc->command_list || !desc->color || !desc->depth ||
        !g_fsr.motion_vectors || !g_fsr.output ||
        !g_fsr.motion_heap || !g_fsr.motion_root_signature ||
        !g_fsr.motion_pipeline) {
        set_status("FSR 3.1 camera dispatch received invalid state or resources");
        return nullptr;
    }
    const uint32_t required_camera =
        KZ3_FSR31_CAMERA_CURRENT_VALID |
        KZ3_FSR31_CAMERA_PLANES_VALID;
    if ((desc->camera.flags & required_camera) != required_camera ||
        (desc->camera.flags & KZ3_FSR31_CAMERA_MULTIPLE_VIEWS)) {
        set_status("FSR 3.1 camera dispatch has no coherent single-view camera");
        return nullptr;
    }
    if (desc->render_width != g_fsr.max_render_width ||
        desc->render_height != g_fsr.max_render_height ||
        desc->output_width != g_fsr.max_output_width ||
        desc->output_height != g_fsr.max_output_height) {
        set_status("FSR 3.1 camera dispatch dimensions do not match its context");
        return nullptr;
    }
    if (!texture_matches(
            desc->color,
            desc->render_width,
            desc->render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !texture_matches(
            desc->depth,
            desc->render_width,
            desc->render_height,
            DXGI_FORMAT_R32_FLOAT)) {
        set_status(
            "FSR 3.1 camera dispatch color or depth format is incompatible");
        return nullptr;
    }

    const bool has_previous =
        (desc->camera.flags & KZ3_FSR31_CAMERA_PREVIOUS_VALID) != 0;
    if (!pose_valid(desc->camera.current_pose) ||
        (has_previous && !pose_valid(desc->camera.previous_pose)) ||
        !std::isfinite(desc->camera.near_plane) ||
        !std::isfinite(desc->camera.far_plane) ||
        !(desc->camera.near_plane > 0.0f) ||
        !(desc->camera.far_plane > desc->camera.near_plane)) {
        set_status("FSR 3.1 camera dispatch has an invalid pose or projection");
        return nullptr;
    }
    const float* previous_pose =
        has_previous ? desc->camera.previous_pose : desc->camera.current_pose;
    const float previous_horizontal_fov =
        has_previous ? desc->camera.previous_horizontal_fov
                     : desc->camera.current_horizontal_fov;
    const float previous_vertical_fov =
        has_previous ? desc->camera.previous_vertical_fov
                     : desc->camera.current_vertical_fov;
    const float tangents[4] = {
        std::tan(desc->camera.current_horizontal_fov * 0.5f),
        std::tan(desc->camera.current_vertical_fov * 0.5f),
        std::tan(previous_horizontal_fov * 0.5f),
        std::tan(previous_vertical_fov * 0.5f)};
    if (!std::isfinite(tangents[0]) || !std::isfinite(tangents[1]) ||
        !std::isfinite(tangents[2]) || !std::isfinite(tangents[3]) ||
        !(tangents[0] > 0.0f) || !(tangents[1] > 0.0f) ||
        !(tangents[2] > 0.0f) || !(tangents[3] > 0.0f)) {
        set_status("FSR 3.1 camera dispatch has an invalid field of view");
        return nullptr;
    }

    D3D12_RESOURCE_BARRIER before[2]{};
    UINT before_count = 0;
    if (g_fsr.motion_is_compute_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_fsr.motion_vectors;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (g_fsr.output_is_pixel_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_fsr.output;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (before_count) {
        desc->command_list->ResourceBarrier(before_count, before);
    }
    g_fsr.motion_is_compute_read = false;
    g_fsr.output_is_pixel_read = false;

    float constants[44]{};
    memcpy(constants, desc->camera.current_pose, 3 * 4 * sizeof(float));
    memcpy(constants + 12, previous_pose, 3 * 4 * sizeof(float));
    memcpy(constants + 24, desc->camera.current_pose + 12, 3 * sizeof(float));
    memcpy(constants + 28, previous_pose + 12, 3 * sizeof(float));
    memcpy(constants + 32, tangents, sizeof(tangents));
    constants[36] = static_cast<float>(desc->render_width);
    constants[37] = static_cast<float>(desc->render_height);
    constants[38] = desc->jitter_x;
    constants[39] = desc->jitter_y;
    constants[40] = desc->camera.near_plane;
    constants[41] = desc->camera.far_plane;

    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
    depth_srv.Format = DXGI_FORMAT_R32_FLOAT;
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_handle =
        g_fsr.motion_heap->GetCPUDescriptorHandleForHeapStart();
    depth_handle.ptr += g_fsr.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_fsr.device->CreateShaderResourceView(
        desc->depth, &depth_srv, depth_handle);

    ID3D12DescriptorHeap* heaps[] = {g_fsr.motion_heap};
    desc->command_list->SetDescriptorHeaps(1, heaps);
    desc->command_list->SetPipelineState(g_fsr.motion_pipeline);
    desc->command_list->SetComputeRootSignature(
        g_fsr.motion_root_signature);
    desc->command_list->SetComputeRootDescriptorTable(
        0,
        g_fsr.motion_heap->GetGPUDescriptorHandleForHeapStart());
    desc->command_list->SetComputeRoot32BitConstants(
        1,
        44,
        constants,
        0);
    desc->command_list->Dispatch(
        (desc->render_width + 7u) / 8u,
        (desc->render_height + 7u) / 8u,
        1);

    D3D12_RESOURCE_BARRIER motion_ready{};
    motion_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    motion_ready.Transition.pResource = g_fsr.motion_vectors;
    motion_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    motion_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    motion_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &motion_ready);
    g_fsr.motion_is_compute_read = true;

    ID3D12Resource* reactive = nullptr;
    if (desc->use_reactive_mask) {
        if (!generate_reactive_locked(
                desc->command_list,
                desc->render_width,
                desc->render_height)) {
            g_fsr.opaque_capture_serial = 0;
            g_fsr.pre_upscale_capture_serial = 0;
            g_fsr.capture_source = nullptr;
            return nullptr;
        }
        reactive = g_fsr.reactive_mask;
    }
    g_fsr.opaque_capture_serial = 0;
    g_fsr.pre_upscale_capture_serial = 0;
    g_fsr.capture_source = nullptr;

    kz3_fsr31_dispatch_desc upscale{};
    upscale.command_list = desc->command_list;
    upscale.color = desc->color;
    upscale.depth = desc->depth;
    upscale.motion_vectors = g_fsr.motion_vectors;
    upscale.reactive = reactive;
    upscale.output = g_fsr.output;
    upscale.render_width = desc->render_width;
    upscale.render_height = desc->render_height;
    upscale.output_width = desc->output_width;
    upscale.output_height = desc->output_height;
    upscale.jitter_x = desc->jitter_x;
    upscale.jitter_y = desc->jitter_y;
    upscale.motion_vector_scale_x =
        static_cast<float>(desc->render_width);
    upscale.motion_vector_scale_y =
        static_cast<float>(desc->render_height);
    upscale.frame_time_ms = desc->frame_time_ms;
    upscale.camera_near = desc->camera.near_plane;
    upscale.camera_far = desc->camera.far_plane;
    upscale.camera_vertical_fov = desc->camera.current_vertical_fov;
    upscale.view_space_to_meters = 1.0f;
    upscale.sharpness = desc->sharpness;
    upscale.reset =
        desc->reset ||
        (desc->camera.flags & KZ3_FSR31_CAMERA_RESET) ||
        !has_previous;
    if (!dispatch_locked(&upscale)) {
        return nullptr;
    }

    D3D12_RESOURCE_BARRIER output_ready{};
    output_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_ready.Transition.pResource = g_fsr.output;
    output_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &output_ready);
    g_fsr.output_is_pixel_read = true;
    return g_fsr.output;
}

extern "C" void kz3_fsr31_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    destroy_context_locked();
    if (g_fsr.module) {
        FreeLibrary(g_fsr.module);
        g_fsr.module = nullptr;
        g_fsr.functions = {};
    }
    set_status("FSR 3.1 is shut down");
}

extern "C" const char* kz3_fsr31_status(void)
{
    thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_fsr.mutex);
    copy = g_fsr.status;
    return copy.c_str();
}
