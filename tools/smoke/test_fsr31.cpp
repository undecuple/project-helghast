#include "fsr31.h"
#include "rsx_live_draw.h"
#include "rsx_upscale.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

namespace {

rsx_upscale_ops g_registered_ops{};
rsx_live_upscale_hooks g_registered_hooks{};
bool g_backend_registered = false;
bool g_hooks_registered = false;

} // namespace

extern "C" int rsx_upscale_register_backend(
    rsx_upscale_backend backend,
    const rsx_upscale_ops* ops)
{
    if (backend != RSX_UPSCALE_FSR3 || !ops) {
        return 0;
    }
    g_registered_ops = *ops;
    g_backend_registered = true;
    return 1;
}

extern "C" void rsx_live_draw_set_upscale_hooks(
    const rsx_live_upscale_hooks* hooks)
{
    if (hooks) {
        g_registered_hooks = *hooks;
        g_hooks_registered = true;
    } else {
        g_registered_hooks = {};
        g_hooks_registered = false;
    }
}

namespace {

template <typename T>
void release(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool check(HRESULT hr, const char* action)
{
    if (SUCCEEDED(hr)) {
        return true;
    }
    fprintf(stderr, "[test_fsr31] %s failed (HRESULT 0x%08lx)\n", action, hr);
    return false;
}

bool create_device(bool force_warp, ID3D12Device** out_device)
{
    HRESULT hr = E_FAIL;
    if (!force_warp) {
        hr = D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_12_0,
            IID_ID3D12Device,
            reinterpret_cast<void**>(out_device));
        if (SUCCEEDED(hr)) {
            puts("[test_fsr31] using the default hardware adapter");
            return true;
        }
    }

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* warp = nullptr;
    if (!check(
            CreateDXGIFactory1(
                IID_IDXGIFactory4,
                reinterpret_cast<void**>(&factory)),
            "CreateDXGIFactory1") ||
        !check(factory->EnumWarpAdapter(
            IID_IDXGIAdapter,
            reinterpret_cast<void**>(&warp)),
            "EnumWarpAdapter")) {
        release(warp);
        release(factory);
        return false;
    }
    hr = D3D12CreateDevice(
        warp,
        D3D_FEATURE_LEVEL_12_0,
        IID_ID3D12Device,
        reinterpret_cast<void**>(out_device));
    release(warp);
    release(factory);
    if (!check(hr, "D3D12CreateDevice(WARP)")) {
        return false;
    }
    puts("[test_fsr31] using WARP");
    return true;
}

bool create_texture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out_resource)
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
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return check(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initial_state,
            nullptr,
            IID_ID3D12Resource,
            reinterpret_cast<void**>(out_resource)),
        "CreateCommittedResource(texture)");
}

bool upload_input(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* texture,
    std::vector<ID3D12Resource*>& uploads,
    bool alternate_color = false)
{
    const D3D12_RESOURCE_DESC texture_desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(
        &texture_desc,
        0,
        1,
        0,
        &footprint,
        &rows,
        &row_bytes,
        &total_bytes);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    if (!check(
            device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_ID3D12Resource,
                reinterpret_cast<void**>(&upload)),
            "CreateCommittedResource(upload)")) {
        return false;
    }

    void* mapped = nullptr;
    if (!check(upload->Map(0, nullptr, &mapped), "Map(upload)")) {
        release(upload);
        return false;
    }
    memset(mapped, 0, static_cast<size_t>(total_bytes));
    unsigned char* pixels =
        static_cast<unsigned char*>(mapped) + footprint.Offset;
    if (texture_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (UINT y = 0; y < texture_desc.Height; ++y) {
            unsigned char* row = pixels + y * footprint.Footprint.RowPitch;
            for (UINT64 x = 0; x < texture_desc.Width; ++x) {
                row[x * 4 + 0] = alternate_color ? 224 : 48;
                row[x * 4 + 1] = alternate_color ? 48 : 112;
                row[x * 4 + 2] = alternate_color ? 80 : 208;
                row[x * 4 + 3] = 255;
            }
        }
    } else if (texture_desc.Format == DXGI_FORMAT_R32_FLOAT) {
        const float scene_depth = 0.5f;
        for (UINT y = 0; y < texture_desc.Height; ++y) {
            unsigned char* row = pixels + y * footprint.Footprint.RowPitch;
            for (UINT64 x = 0; x < texture_desc.Width; ++x) {
                memcpy(
                    row + x * sizeof(float),
                    &scene_depth,
                    sizeof(float));
            }
        }
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    uploads.push_back(upload);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    constexpr uint32_t render_width = 320;
    constexpr uint32_t render_height = 180;
    constexpr uint32_t output_width = 640;
    constexpr uint32_t output_height = 360;

    if (!kz3_fsr31_register() ||
        !g_backend_registered ||
        !g_hooks_registered ||
        !g_registered_ops.create ||
        !g_registered_ops.destroy ||
        !g_registered_ops.evaluate ||
        g_registered_hooks.backend != RSX_UPSCALE_FSR3 ||
        !g_registered_hooks.inspect_draw ||
        !g_registered_hooks.before_writeback) {
        fputs(
            "[test_fsr31] registered backend or renderer hooks are "
            "incomplete\n",
            stderr);
        return 1;
    }
    puts("[test_fsr31] registered backend and renderer hooks: ok");

    {
        float pose[16] = {};
        for (int i = 0; i < 4; ++i) {
            pose[i * 4 + i] = 1.0f;
        }
        kz3_fsr31_camera_state camera{};
        kz3_fsr31_note_camera_planes(100, 0.1f, 1000.0f);
        kz3_fsr31_note_camera(100, 1, pose, 1.4f, 0.9f);
        if (!kz3_fsr31_get_camera(&camera) ||
            !(camera.flags & KZ3_FSR31_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_fsr31] camera did not accept coherent projection "
                "planes\n",
                stderr);
            return 1;
        }
        kz3_fsr31_note_camera_planes(100, 0.2f, 1000.0f);
        if (!kz3_fsr31_get_camera(&camera) ||
            (camera.flags & KZ3_FSR31_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_fsr31] camera accepted conflicting projection "
                "planes\n",
                stderr);
            return 1;
        }
        kz3_fsr31_note_camera(101, 1, pose, 1.4f, 0.9f);
        kz3_fsr31_note_camera_planes(101, 0.1f, 1000.0f);
        if (!kz3_fsr31_get_camera(&camera) ||
            !(camera.flags & KZ3_FSR31_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_fsr31] camera did not recover on the next frame\n",
                stderr);
            return 1;
        }
        puts("[test_fsr31] projection-plane coherence: ok");
    }

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* scene = nullptr;
    ID3D12Resource* overlay = nullptr;
    ID3D12Resource* readback = nullptr;
    std::vector<ID3D12Resource*> uploads;
    HANDLE event = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    D3D12_RESOURCE_BARRIER barriers[3]{};
    ID3D12Resource* inputs[3]{};
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    kz3_fsr31_dispatch_desc dispatch{};
    kz3_fsr31_camera_dispatch_desc camera_dispatch{};
    ID3D12Resource* camera_output = nullptr;
    ID3D12Resource* registered_output = nullptr;
    void* registered_backend = nullptr;
    int result = 1;
    bool expect_unsupported = false;
    bool force_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--expect-unsupported") == 0) {
            expect_unsupported = true;
        } else if (strcmp(argv[i], "--warp") == 0) {
            force_warp = true;
        } else {
            fprintf(stderr, "[test_fsr31] unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!create_device(force_warp, &device)) {
        puts("[test_fsr31] SKIP: D3D12 is unavailable");
        return 0;
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        if (SUCCEEDED(device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS1,
                &options,
                sizeof(options)))) {
            printf(
                "[test_fsr31] wave operations: %s (%u..%u lanes)\n",
                options.WaveOps ? "yes" : "no",
                options.WaveLaneCountMin,
                options.WaveLaneCountMax);
        }
    }
    if (!kz3_fsr31_is_supported(device)) {
        if (expect_unsupported) {
            const rsx_upscale_config config = {
                RSX_UPSCALE_FSR3,
                0.2f};
            registered_backend =
                g_registered_ops.create(device, &config);
            if (registered_backend) {
                fputs(
                    "[test_fsr31] registered backend unexpectedly started "
                    "without its adjacent runtime\n",
                    stderr);
                goto done;
            }
            printf(
                force_warp
                    ? "[test_fsr31] software-adapter fallback: ok (%s)\n"
                    : "[test_fsr31] missing-runtime fallback: ok (%s)\n",
                kz3_fsr31_status());
            result = 0;
            goto done;
        }
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }
    if (expect_unsupported) {
        fputs(
            "[test_fsr31] FSR unexpectedly remained available without its "
            "adjacent runtime\n",
            stderr);
        goto done;
    }
    {
        const rsx_upscale_config config = {
            RSX_UPSCALE_FSR3,
            0.2f};
        registered_backend = g_registered_ops.create(device, &config);
    }
    if (!registered_backend) {
        fprintf(
            stderr,
            "[test_fsr31] registered backend creation failed: %s\n",
            kz3_fsr31_status());
        goto done;
    }
    if (!check(
            device->CreateCommandQueue(
                &queue_desc,
                IID_ID3D12CommandQueue,
                reinterpret_cast<void**>(&queue)),
            "CreateCommandQueue") ||
        !check(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_ID3D12CommandAllocator,
                reinterpret_cast<void**>(&allocator)),
            "CreateCommandAllocator") ||
        !check(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator,
                nullptr,
                IID_ID3D12GraphicsCommandList,
                reinterpret_cast<void**>(&list)),
            "CreateCommandList") ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &depth) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &motion) ||
        !create_texture(
            device,
            output_width,
            output_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            &output) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &scene) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &overlay) ||
        !upload_input(device, list, color, uploads) ||
        !upload_input(device, list, depth, uploads) ||
        !upload_input(device, list, motion, uploads) ||
        !upload_input(device, list, scene, uploads) ||
        !upload_input(device, list, overlay, uploads, true)) {
        goto done;
    }

    inputs[0] = color;
    inputs[1] = depth;
    inputs[2] = motion;
    for (size_t i = 0; i < 3; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = inputs[i];
        barriers[i].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    list->ResourceBarrier(3, barriers);

    {
        rsx_live_upscale_draw_info draw{};
        draw.frame = 1;
        draw.render_width = render_width;
        draw.render_height = render_height;
        draw.output_width = output_width;
        draw.output_height = output_height;
        draw.target_location = 0;
        draw.target_width = render_width;
        draw.target_height = render_height;
        draw.mrt_count = 4;
        draw.depth_slot = 1;
        draw.full_viewport = 1;
        draw.depth_test = 1;
        draw.command_list = list;
        draw.color_resource = color;
        rsx_live_upscale_draw_action action{};
        g_registered_hooks.inspect_draw(
            g_registered_hooks.user, &draw, &action);
        if (!action.select_depth || action.apply_jitter) {
            fputs(
                "[test_fsr31] first registered draw did not gate jitter "
                "on context readiness\n",
                stderr);
            goto done;
        }

        const rsx_upscale_image bridge_depth = {
            depth,
            render_width,
            render_height,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        rsx_upscale_eval bridge_eval{};
        bridge_eval.cmd_list = list;
        bridge_eval.input = {
            color,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        bridge_eval.output_width = output_width;
        bridge_eval.output_height = output_height;
        bridge_eval.depth = &bridge_depth;
        bridge_eval.frame_time_ms = 1000.0f / 30.0f;
        bridge_eval.reset_history = 1;
        rsx_upscale_result bridge_result{};
        if (!g_registered_ops.evaluate(
                registered_backend,
                &bridge_eval,
                0.2f,
                &bridge_result,
                nullptr,
                nullptr) ||
            !bridge_result.resource ||
            bridge_result.width != output_width ||
            bridge_result.height != output_height ||
            bridge_result.format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            bridge_result.state !=
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            fprintf(
                stderr,
                "[test_fsr31] registered backend dispatch failed: %s\n",
                kz3_fsr31_status());
            goto done;
        }
        registered_output =
            static_cast<ID3D12Resource*>(bridge_result.resource);

        draw.frame = 2;
        action = {};
        g_registered_hooks.inspect_draw(
            g_registered_hooks.user, &draw, &action);
        if (!action.select_depth || !action.apply_jitter) {
            fputs(
                "[test_fsr31] registered draw did not enable jitter after "
                "context creation\n",
                stderr);
            goto done;
        }
        puts("[test_fsr31] registered backend dispatch and jitter gating: ok");
    }

    if (force_warp) {
        camera_output = registered_output;
        goto prepare_readback;
    }

    barriers[0].Transition.pResource = scene;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.pResource = overlay;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(2, barriers);
    if (!kz3_fsr31_capture_opaque_color(
            list, scene, render_width, render_height)) {
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }
    barriers[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, barriers);
    list->CopyResource(scene, overlay);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, barriers);
    if (!kz3_fsr31_capture_pre_upscale_color(
            list, scene, render_width, render_height)) {
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }

    if (!kz3_fsr31_get_jitter(
            0,
            render_width,
            output_width,
            &jitter_x,
            &jitter_y)) {
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }

    dispatch.command_list = list;
    dispatch.color = color;
    dispatch.depth = depth;
    dispatch.motion_vectors = motion;
    dispatch.output = output;
    dispatch.render_width = render_width;
    dispatch.render_height = render_height;
    dispatch.output_width = output_width;
    dispatch.output_height = output_height;
    dispatch.jitter_x = jitter_x;
    dispatch.jitter_y = jitter_y;
    dispatch.motion_vector_scale_x = static_cast<float>(render_width);
    dispatch.motion_vector_scale_y = static_cast<float>(render_height);
    dispatch.frame_time_ms = 1000.0f / 30.0f;
    dispatch.camera_near = 0.1f;
    dispatch.camera_far = 1000.0f;
    dispatch.camera_vertical_fov = 1.0f;
    dispatch.view_space_to_meters = 1.0f;
    dispatch.reset = 1;
    if (!kz3_fsr31_dispatch(&dispatch)) {
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }

    camera_dispatch.command_list = list;
    camera_dispatch.color = color;
    camera_dispatch.depth = depth;
    camera_dispatch.camera.guest_flip = 1;
    camera_dispatch.camera.view_id = 1;
    camera_dispatch.camera.flags =
        KZ3_FSR31_CAMERA_CURRENT_VALID |
        KZ3_FSR31_CAMERA_PREVIOUS_VALID |
        KZ3_FSR31_CAMERA_PLANES_VALID |
        KZ3_FSR31_CAMERA_RESET;
    for (int i = 0; i < 4; ++i) {
        camera_dispatch.camera.current_pose[i * 4 + i] = 1.0f;
        camera_dispatch.camera.previous_pose[i * 4 + i] = 1.0f;
    }
    camera_dispatch.camera.current_pose[13] = 0.25f;
    camera_dispatch.camera.current_horizontal_fov = 1.4f;
    camera_dispatch.camera.current_vertical_fov = 0.9f;
    camera_dispatch.camera.previous_horizontal_fov = 1.4f;
    camera_dispatch.camera.previous_vertical_fov = 0.9f;
    camera_dispatch.camera.near_plane = 0.1f;
    camera_dispatch.camera.far_plane = 1000.0f;
    camera_dispatch.render_width = render_width;
    camera_dispatch.render_height = render_height;
    camera_dispatch.output_width = output_width;
    camera_dispatch.output_height = output_height;
    camera_dispatch.jitter_x = jitter_x;
    camera_dispatch.jitter_y = jitter_y;
    camera_dispatch.frame_time_ms = 1000.0f / 30.0f;
    camera_dispatch.reset = 1;
    camera_dispatch.use_reactive_mask = 1;
    camera_output = kz3_fsr31_dispatch_camera(&camera_dispatch);
    if (!camera_output) {
        fprintf(stderr, "[test_fsr31] %s\n", kz3_fsr31_status());
        goto done;
    }
prepare_readback:
    {
        const D3D12_RESOURCE_DESC desc = camera_output->GetDesc();
        if (desc.Width != output_width || desc.Height != output_height ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            fprintf(
                stderr,
                "[test_fsr31] unexpected camera output %llux%u format %u\n",
                static_cast<unsigned long long>(desc.Width),
                desc.Height,
                static_cast<unsigned>(desc.Format));
            goto done;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total_bytes = 0;
        device->GetCopyableFootprints(
            &desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total_bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!check(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_ID3D12Resource,
                    reinterpret_cast<void**>(&readback)),
                "CreateCommittedResource(readback)")) {
            goto done;
        }
        D3D12_RESOURCE_BARRIER ready{};
        ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ready.Transition.pResource = camera_output;
        ready.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ready.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ready.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &ready);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = camera_output;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
    }
    if (!check(list->Close(), "Close(command list)")) {
        goto done;
    }

    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!check(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_ID3D12Fence,
                reinterpret_cast<void**>(&fence)),
            "CreateFence")) {
        goto done;
    }
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        fprintf(stderr, "[test_fsr31] CreateEventW failed (%lu)\n", GetLastError());
        goto done;
    }
    if (!check(queue->Signal(fence, 1), "Signal(fence)") ||
        !check(
            fence->SetEventOnCompletion(1, event),
            "SetEventOnCompletion(fence)") ||
        WaitForSingleObject(event, 30000) != WAIT_OBJECT_0 ||
        !check(device->GetDeviceRemovedReason(), "FSR dispatch completion")) {
        goto done;
    }

    {
        const D3D12_RESOURCE_DESC desc = camera_output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total_bytes = 0;
        device->GetCopyableFootprints(
            &desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
        D3D12_RANGE range{0, static_cast<SIZE_T>(total_bytes)};
        void* mapped = nullptr;
        if (!check(readback->Map(0, &range, &mapped), "Map(readback)")) {
            goto done;
        }
        uint64_t nonblack = 0;
        const unsigned char* pixels =
            static_cast<const unsigned char*>(mapped) + footprint.Offset;
        for (UINT y = 0; y < output_height; ++y) {
            const unsigned char* row =
                pixels + y * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < output_width; ++x) {
                if (row[x * 4] || row[x * 4 + 1] || row[x * 4 + 2]) {
                    nonblack++;
                }
            }
        }
        readback->Unmap(0, nullptr);
        if (nonblack <=
            static_cast<uint64_t>(output_width) * output_height / 2u) {
            fprintf(
                stderr,
                "[test_fsr31] output is unexpectedly empty "
                "(%llu/%u non-black pixels)\n",
                static_cast<unsigned long long>(nonblack),
                output_width * output_height);
            goto done;
        }
        printf(
            "[test_fsr31] output readback: %llu/%u non-black pixels\n",
            static_cast<unsigned long long>(nonblack),
            output_width * output_height);
    }
    if (force_warp) {
        puts(
            "[test_fsr31] FSR 3.1.4 registered WARP dispatch and "
            "readback: ok");
    } else {
        printf(
            "[test_fsr31] FSR 3.1.4 external, depth-motion, and reactive "
            "dispatches: ok (jitter %.6f, %.6f)\n",
            jitter_x,
            jitter_y);
    }
    result = 0;

done:
    if (event) {
        CloseHandle(event);
    }
    if (registered_backend) {
        g_registered_ops.destroy(registered_backend, nullptr, nullptr);
    } else {
        kz3_fsr31_shutdown();
    }
    for (ID3D12Resource*& upload : uploads) {
        release(upload);
    }
    release(readback);
    release(output);
    release(motion);
    release(depth);
    release(color);
    release(overlay);
    release(scene);
    release(fence);
    release(list);
    release(allocator);
    release(queue);
    release(device);
    return result;
}
