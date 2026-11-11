/*
 * Native GPU test (run by test_xess.ps1, needs the pinned libxess.dll): are the port's jitter and motion conventions the
 * ones XeSS-SR reconstructs with? Intel's guide does not state the NDC axis directions and suggests trying +1 and -1, so
 * this asks the runtime through its public API. A hard-edged rotated checker, whose sharp edges come back only through
 * correct accumulation, is fed for 64 frames and the last output is scored against supersampled ground truth on the
 * central quarter. Only the differences between variants mean anything.
 */
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xess_api.h"

namespace {

constexpr uint32_t kInputWidth = 1280;
constexpr uint32_t kInputHeight = 720;
constexpr uint32_t kOutputWidth = 1920;
constexpr uint32_t kOutputHeight = 1080;
constexpr int kFrames = 64;
constexpr uint32_t kFlagsPort =
    KZ3_XESS_INIT_RESPONSIVE_PIXEL_MASK |
    KZ3_XESS_INIT_USE_NDC_VELOCITY |
    KZ3_XESS_INIT_LDR_INPUT_COLOR;
constexpr uint32_t kFlagsPixels =
    KZ3_XESS_INIT_RESPONSIVE_PIXEL_MASK |
    KZ3_XESS_INIT_LDR_INPUT_COLOR;

ID3D12Device* g_device;
ID3D12CommandQueue* g_queue;
ID3D12CommandAllocator* g_allocator;
ID3D12GraphicsCommandList* g_list;
ID3D12Fence* g_fence;
HANDLE g_event;
uint64_t g_fence_value;
kz3_xess_functions g_xess;

[[noreturn]] void fail(const char* what, long code)
{
    fprintf(stderr, "[test_xess_temporal] %s failed (0x%08lx)\n", what, code);
    exit(1);
}

void require(HRESULT result, const char* what)
{
    if (FAILED(result)) {
        fail(what, result);
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

void submit_and_wait()
{
    require(g_list->Close(), "Close");
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    require(g_queue->Signal(g_fence, ++g_fence_value), "Signal");
    require(
        g_fence->SetEventOnCompletion(g_fence_value, g_event),
        "SetEventOnCompletion");
    if (WaitForSingleObject(g_event, 60000) != WAIT_OBJECT_0) {
        fail("fence wait", 0);
    }
    require(g_device->GetDeviceRemovedReason(), "device");
    require(g_allocator->Reset(), "allocator Reset");
    require(g_list->Reset(g_allocator, nullptr), "list Reset");
}

ID3D12Resource* make_texture(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ID3D12Resource* resource = nullptr;
    require(
        g_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            state,
            nullptr,
            IID_ID3D12Resource,
            reinterpret_cast<void**>(&resource)),
        "texture");
    return resource;
}

ID3D12Resource* make_buffer(
    uint64_t bytes,
    D3D12_HEAP_TYPE type,
    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    require(
        g_device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            state,
            nullptr,
            IID_ID3D12Resource,
            reinterpret_cast<void**>(&resource)),
        "buffer");
    return resource;
}

void barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = resource;
    transition.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = before;
    transition.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &transition);
}

/* A persistently mapped upload path into one input texture. */
struct Upload {
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* buffer = nullptr;
    unsigned char* mapped = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

    void open(ID3D12Resource* target)
    {
        texture = target;
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total = 0;
        g_device->GetCopyableFootprints(
            &desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
        buffer = make_buffer(
            total,
            D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        require(
            buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)),
            "Map");
    }

    unsigned char* row(uint32_t y)
    {
        return mapped + footprint.Offset + y * footprint.Footprint.RowPitch;
    }

    void copy()
    {
        barrier(
            texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = buffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_list->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void close()
    {
        if (buffer) {
            buffer->Unmap(0, nullptr);
        }
        release(buffer);
    }
};

uint16_t half_from(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 255u) - 112;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    return static_cast<uint16_t>(
        sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float scene(float u, float v)
{
    const float a = (u * 0.9563f + v * 0.2924f) * (3.14159265f / 6.0f);
    const float b = (-u * 0.2924f + v * 0.9563f) * (3.14159265f / 6.0f);
    return std::sin(a) * std::sin(b) >= 0.0f ? 0.86f : 0.14f;
}

float halton(uint32_t index, uint32_t base)
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

struct Variant {
    const char* name;
    uint32_t flags;
    float motion_x;      /* the value stored in the velocity texture */
    float motion_y;
    float jitter_sign;   /* +1: a surface shows at P + J in y-down pixels, as the port's position offset does */
    float scene_x;       /* scene motion, input pixels per frame, y down */
    float scene_y;
};

double run_variant(const Variant& variant)
{
    constexpr auto kInput = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* color = make_texture(
        kInputWidth, kInputHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, kInput);
    ID3D12Resource* depth = make_texture(
        kInputWidth, kInputHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, kInput);
    ID3D12Resource* velocity = make_texture(
        kInputWidth, kInputHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kInput);
    ID3D12Resource* mask = make_texture(
        kInputWidth, kInputHeight, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, kInput);
    ID3D12Resource* output = make_texture(
        kOutputWidth,
        kOutputHeight,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Upload color_upload, depth_upload, velocity_upload, mask_upload;
    color_upload.open(color);
    depth_upload.open(depth);
    velocity_upload.open(velocity);
    mask_upload.open(mask);
    for (uint32_t y = 0; y < kInputHeight; ++y) {
        float* depth_row = reinterpret_cast<float*>(depth_upload.row(y));
        uint16_t* velocity_row =
            reinterpret_cast<uint16_t*>(velocity_upload.row(y));
        for (uint32_t x = 0; x < kInputWidth; ++x) {
            depth_row[x] = 0.5f;
            velocity_row[x * 2] = half_from(variant.motion_x);
            velocity_row[x * 2 + 1] = half_from(variant.motion_y);
        }
        memset(mask_upload.row(y), 0, kInputWidth);
    }
    depth_upload.copy();
    velocity_upload.copy();
    mask_upload.copy();
    submit_and_wait();

    kz3_xess_context_handle context = nullptr;
    int result = g_xess.create_context(g_device, &context);
    if (result != KZ3_XESS_SUCCESS) {
        fail("xessD3D12CreateContext", result);
    }
    kz3_xess_d3d12_init_params init{};
    init.output_resolution = {kOutputWidth, kOutputHeight};
    init.quality_setting = KZ3_XESS_QUALITY_ULTRA_QUALITY;
    init.init_flags = variant.flags;
    result = g_xess.init(context, &init);
    if (result != KZ3_XESS_SUCCESS) {
        fail("xessD3D12Init", result);
    }

    constexpr uint32_t phases = 18;
    for (int frame = 0; frame < kFrames; ++frame) {
        const uint32_t index = static_cast<uint32_t>(frame) % phases + 1u;
        const float jitter_x = halton(index, 2) - 0.5f;
        const float jitter_y = halton(index, 3) - 0.5f;
        const float shift_x = variant.scene_x * static_cast<float>(frame);
        const float shift_y = variant.scene_y * static_cast<float>(frame);
        for (uint32_t y = 0; y < kInputHeight; ++y) {
            unsigned char* row = color_upload.row(y);
            for (uint32_t x = 0; x < kInputWidth; ++x) {
                const float value = scene(
                    static_cast<float>(x) + 0.5f -
                        variant.jitter_sign * jitter_x - shift_x,
                    static_cast<float>(y) + 0.5f -
                        variant.jitter_sign * jitter_y - shift_y);
                const unsigned char byte =
                    static_cast<unsigned char>(value * 255.0f + 0.5f);
                row[x * 4 + 0] = byte;
                row[x * 4 + 1] = byte;
                row[x * 4 + 2] = byte;
                row[x * 4 + 3] = 255;
            }
        }
        color_upload.copy();
        kz3_xess_d3d12_execute_params execute{};
        execute.color_texture = color;
        execute.velocity_texture = velocity;
        execute.depth_texture = depth;
        execute.responsive_pixel_mask_texture = mask;
        execute.output_texture = output;
        execute.jitter_offset_x = jitter_x;
        execute.jitter_offset_y = jitter_y;
        execute.exposure_scale = 1.0f;
        execute.reset_history = frame == 0;
        execute.input_width = kInputWidth;
        execute.input_height = kInputHeight;
        result = g_xess.execute(context, g_list, &execute);
        if (result != KZ3_XESS_SUCCESS) {
            fail("xessD3D12Execute", result);
        }
        submit_and_wait();
    }

    const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    g_device->GetCopyableFootprints(
        &output_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
    ID3D12Resource* readback = make_buffer(
        total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(
        output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = output;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    g_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    submit_and_wait();

    unsigned char* pixels = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    require(
        readback->Map(0, &range, reinterpret_cast<void**>(&pixels)),
        "Map(readback)");
    const float shift_x =
        variant.scene_x * static_cast<float>(kFrames - 1);
    const float shift_y =
        variant.scene_y * static_cast<float>(kFrames - 1);
    const float to_input =
        static_cast<float>(kInputWidth) / static_cast<float>(kOutputWidth);
    double squared_error = 0.0;
    uint64_t samples = 0;
    for (uint32_t y = kOutputHeight / 4; y < kOutputHeight * 3 / 4; ++y) {
        const unsigned char* row =
            pixels + footprint.Offset + y * footprint.Footprint.RowPitch;
        for (uint32_t x = kOutputWidth / 4; x < kOutputWidth * 3 / 4; ++x) {
            float truth = 0.0f;
            for (int sub_x = 0; sub_x < 4; ++sub_x) {
                for (int sub_y = 0; sub_y < 4; ++sub_y) {
                    truth += scene(
                        (static_cast<float>(x) +
                         (static_cast<float>(sub_x) + 0.5f) / 4.0f) *
                                to_input -
                            shift_x,
                        (static_cast<float>(y) +
                         (static_cast<float>(sub_y) + 0.5f) / 4.0f) *
                                to_input -
                            shift_y);
                }
            }
            const double difference =
                static_cast<double>(row[x * 4]) / 255.0 -
                static_cast<double>(truth) / 16.0;
            squared_error += difference * difference;
            ++samples;
        }
    }
    readback->Unmap(0, nullptr);
    g_xess.destroy_context(context);
    release(readback);
    color_upload.close();
    depth_upload.close();
    velocity_upload.close();
    mask_upload.close();
    release(output);
    release(mask);
    release(velocity);
    release(depth);
    release(color);
    const double psnr =
        10.0 * std::log10(static_cast<double>(samples) / squared_error);
    printf("[test_xess_temporal] %-34s %6.2f dB\n", variant.name, psnr);
    return psnr;
}

template <typename T>
void load(T* out, HMODULE module, const char* name)
{
    const FARPROC address = GetProcAddress(module, name);
    if (!address) {
        fail(name, 0);
    }
    memcpy(out, &address, sizeof(address));
}

}  // namespace

int main()
{
    HMODULE module = LoadLibraryExW(
        L"libxess.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        fail("loading libxess.dll", static_cast<long>(GetLastError()));
    }
    load(&g_xess.create_context, module, "xessD3D12CreateContext");
    load(&g_xess.init, module, "xessD3D12Init");
    load(&g_xess.execute, module, "xessD3D12Execute");
    load(&g_xess.destroy_context, module, "xessDestroyContext");

    if (FAILED(D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_12_0,
            IID_ID3D12Device,
            reinterpret_cast<void**>(&g_device)))) {
        puts("[test_xess_temporal] SKIP: a D3D12 hardware device is unavailable");
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    require(
        g_device->CreateCommandQueue(
            &queue_desc,
            IID_ID3D12CommandQueue,
            reinterpret_cast<void**>(&g_queue)),
        "queue");
    require(
        g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_ID3D12CommandAllocator,
            reinterpret_cast<void**>(&g_allocator)),
        "allocator");
    require(
        g_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_allocator,
            nullptr,
            IID_ID3D12GraphicsCommandList,
            reinterpret_cast<void**>(&g_list)),
        "list");
    require(
        g_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_ID3D12Fence,
            reinterpret_cast<void**>(&g_fence)),
        "fence");
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    /* The scene moves right and down; a point now at P was at P - v, so current-to-previous is negative in y-down
     * pixels, and positive on Y in y-up NDC. */
    constexpr float vx = 1.25f;
    constexpr float vy = 2.75f;
    const float nx = 2.0f * vx / static_cast<float>(kInputWidth);
    const float ny = 2.0f * vy / static_cast<float>(kInputHeight);
    const double jitter_port = run_variant(
        {"static, jitter as the port", kFlagsPort, 0, 0, +1, 0, 0});
    const double jitter_negated = run_variant(
        {"static, jitter negated", kFlagsPort, 0, 0, -1, 0, 0});
    const double motion_port = run_variant(
        {"moving, NDC as the port", kFlagsPort, -nx, +ny, +1, vx, vy});
    const double motion_pixels = run_variant(
        {"moving, pixel units (control)", kFlagsPixels, -vx, -vy, +1, vx, vy});
    const double motion_y_flipped = run_variant(
        {"moving, NDC Y flipped", kFlagsPort, -nx, -ny, +1, vx, vy});
    const double motion_x_flipped = run_variant(
        {"moving, NDC X flipped", kFlagsPort, +nx, +ny, +1, vx, vy});
    const double motion_none = run_variant(
        {"moving, no motion vectors", kFlagsPort, 0, 0, +1, vx, vy});

    constexpr double kMargin = 3.0;
    const bool ok =
        jitter_port > jitter_negated + kMargin &&
        std::fabs(motion_port - motion_pixels) < 1.0 &&
        motion_port > motion_y_flipped + kMargin &&
        motion_port > motion_x_flipped + kMargin &&
        motion_port > motion_none + kMargin;
    printf("[test_xess_temporal] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
