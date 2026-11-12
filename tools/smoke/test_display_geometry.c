/* Platform-edge mocks only. The driver appends extracted SDK routing, geometry
 * publication and input functions, followed by test_display_cases.inc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "rsx_display_mode.h"
#include "rsx_hud169.h"
#include "rsx_present_geometry.h"
#include "rsx_ui_provenance.h"

typedef unsigned u32;
typedef unsigned long long u64;
typedef int HRESULT;
typedef int D3D12_RESOURCE_STATES;
#define S_OK 0
#define DXGI_STATUS_OCCLUDED 1
#define SUCCEEDED(x) ((x) >= 0)
#define DXGI_PRESENT_ALLOW_TEARING 2
#define D3D12_RESOURCE_DIMENSION_TEXTURE2D 3
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define DXGI_FORMAT_R16G16B16A16_FLOAT 10
#define DXGI_FORMAT_R32_FLOAT 41
#define D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET 1
#define D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE 8
#define D3D12_RESOURCE_STATE_PRESENT 0
#define D3D12_RESOURCE_STATE_RENDER_TARGET 1
#define D3D12_RESOURCE_STATE_COPY_SOURCE 2
#define D3D12_RESOURCE_STATE_COPY_DEST 3
#define D3D12_RESOURCE_BARRIER_TYPE_TRANSITION 0
#define D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES (~0u)
#define D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX 0
#define D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT 1
#define SRV_RING_TABLES 8
typedef struct {
    unsigned Count, Quality;
} SAMPLE_DESC;
typedef struct {
    uint64_t Width;
    unsigned Dimension, Height, DepthOrArraySize, MipLevels, Format, Flags;
    SAMPLE_DESC SampleDesc;
} D3D12_RESOURCE_DESC;
typedef struct ID3D12Resource ID3D12Resource;
typedef struct {
    void (*GetDesc)(ID3D12Resource*, D3D12_RESOURCE_DESC*);
} resource_vtable;
struct ID3D12Resource {
    resource_vtable* lpVtbl;
    D3D12_RESOURCE_DESC desc;
    int state;
};
typedef struct {
    int Type;
    struct {
        ID3D12Resource* pResource;
        unsigned Subresource;
        int StateBefore, StateAfter;
    } Transition;
} D3D12_RESOURCE_BARRIER;
typedef struct {
    unsigned Format, Width, Height, Depth, RowPitch;
} FOOTPRINT;
typedef struct {
    ID3D12Resource* pResource;
    int Type;
    unsigned SubresourceIndex;
    struct {
        uint64_t Offset;
        FOOTPRINT Footprint;
    } PlacedFootprint;
} D3D12_TEXTURE_COPY_LOCATION;
typedef struct list list;
typedef struct {
    void (*ResourceBarrier)(list*, unsigned, const D3D12_RESOURCE_BARRIER*);
    void (*CopyResource)(list*, ID3D12Resource*, ID3D12Resource*);
    void (*CopyTextureRegion)(list*, const D3D12_TEXTURE_COPY_LOCATION*, unsigned, unsigned, unsigned,
                              const D3D12_TEXTURE_COPY_LOCATION*, const void*);
    void (*ClearRenderTargetView)(list*, unsigned, const float*, unsigned, const void*);
} list_vtable;
struct list {
    list_vtable* lpVtbl;
};
typedef struct swap swap;
typedef struct {
    u32 (*GetCurrentBackBufferIndex)(swap*);
    HRESULT (*Present)(swap*, unsigned, unsigned);
} swap_vtable;
struct swap {
    swap_vtable* lpVtbl;
};
static struct {
    unsigned width, height, srv_ring_used;
    int movie_overlay_valid;
    rsx_ui_provenance movie_overlay_ui;
    ID3D12Resource* backbuf[1];
    ID3D12Resource* upload;
    ID3D12Resource* movie_upload;
    list* list;
    swap* swap;
} g;
static unsigned g_ld_frames, g_dm_bb_w, g_dm_bb_h;
static int g_dm_present_n, g_dm_wide_n, g_dm_scaling, g_dm_filter, g_dm_fs;
static int g_ld_vsync = 1, g_ld_tearing_ok, g_ld_vsync_logged = -1;
static int g_aspect = RSX_DM_ASPECT_SCREEN, pipeline_ok, movie_target_ok, slot_ok, present_result;
static int pipeline_calls, upscale_calls, upscale_enabled, upscale_null, state_invalidations;
#define LD_SM_INVALIDATE() ((void)state_invalidations++)
static int copies, textures, clears, blits, invalid_copy, invalid_texture, invalid_barrier, swap_calls;
static ID3D12Resource src_resource, bb_resource, gamma_resource, movie_resource, upload_resource, upscale_resource;
static ID3D12Resource* gamma_copy;
typedef struct {
    ID3D12Resource* tex;
    rsx_ui_provenance ui;
} surface_mock;
static surface_mock surface;
static const surface_mock* presented_surface = &surface;
static rsx_dm_rect last_rect;
typedef struct {
    int left, top, right, bottom;
} RECT;
static struct {
    void* hwnd;
    unsigned width, height;
} s_state;
static int s_dm_scaling = RSX_DM_FIT, s_dm_filter = RSX_DM_BILINEAR, s_present_suppressed = 1;
static RECT client;
static int GetClientRect(void* hwnd, RECT* out)
{
    (void)hwnd;
    *out = client;
    return 1;
}
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define AcquireSRWLockShared(p) ((void)pthread_rwlock_rdlock(p))
#define ReleaseSRWLockShared(p) ((void)pthread_rwlock_unlock(p))
#define AcquireSRWLockExclusive(p) ((void)pthread_rwlock_wrlock(p))
#define ReleaseSRWLockExclusive(p) ((void)pthread_rwlock_unlock(p))
static int rsx_display_get_aspect(void)
{
    return g_aspect;
}
static double rsx_display_view_ratio(void)
{
    return g_aspect == RSX_DM_ASPECT_SCREEN ? rsx_dm_aspect_ratio(g_dm_bb_w, g_dm_bb_h) : 1.;
}
static int ld_h169_on(void)
{
    return 1;
}
static int ld_present_slot(void)
{
    return slot_ok;
}
static int ld_dm_pipeline(void)
{
    pipeline_calls++;
    return pipeline_ok;
}
static ID3D12Resource* ld_dm_movie_target(u32 w, u32 h)
{
    (void)w;
    (void)h;
    return movie_target_ok ? &movie_resource : NULL;
}
static ID3D12Resource* ld_gamma_present(u32 t)
{
    (void)t;
    return gamma_copy;
}
static void ld_dm_sync_size(void) {}
typedef struct {
    int unused;
} rsx_upscale;
typedef struct {
    void* cmd_list;
    struct {
        void* resource;
        unsigned width, height, format, state;
    } input;
    unsigned output_width, output_height;
    float sharpness;
} rsx_upscale_eval;
typedef struct {
    void* resource;
    unsigned state;
} rsx_upscale_result;
static rsx_upscale test_upscale;
static rsx_upscale* ld_up_get(void)
{
    return upscale_enabled ? &test_upscale : NULL;
}
static int rsx_upscale_wants_pass(rsx_upscale* up, unsigned iw, unsigned ih, unsigned ow, unsigned oh)
{
    (void)up;
    return ow > iw || oh > ih;
}
static int rsx_upscale_evaluate(rsx_upscale* up, const rsx_upscale_eval* ev, rsx_upscale_result* out)
{
    (void)up;
    (void)ev;
    upscale_calls++;
    out->resource = upscale_null ? NULL : &upscale_resource;
    out->state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return 1;
}
static unsigned rtv_handle(u32 bbi)
{
    return bbi;
}
static void resource_desc(ID3D12Resource* r, D3D12_RESOURCE_DESC* out)
{
    *out = r->desc;
}
static void barrier(list* l, unsigned n, const D3D12_RESOURCE_BARRIER* b)
{
    (void)l;
    for (unsigned i = 0; i < n; i++) {
        ID3D12Resource* r = b[i].Transition.pResource;
        if (!r || r->state != b[i].Transition.StateBefore) invalid_barrier++;
        if (r) r->state = b[i].Transition.StateAfter;
    }
}
static int independently_compatible(ID3D12Resource* d, ID3D12Resource* s)
{
    return d && s && d->desc.Width == s->desc.Width && d->desc.Height == s->desc.Height &&
           d->desc.Dimension == s->desc.Dimension && d->desc.DepthOrArraySize == s->desc.DepthOrArraySize &&
           d->desc.MipLevels == s->desc.MipLevels && d->desc.Format == s->desc.Format &&
           d->desc.SampleDesc.Count == s->desc.SampleDesc.Count &&
           d->desc.SampleDesc.Quality == s->desc.SampleDesc.Quality;
}
static void copy_resource(list* l, ID3D12Resource* dst, ID3D12Resource* src)
{
    (void)l;
    copies++;
    if (!independently_compatible(dst, src) || dst->state != D3D12_RESOURCE_STATE_COPY_DEST ||
        src->state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        invalid_copy++;
}
static void copy_texture(list* l, const D3D12_TEXTURE_COPY_LOCATION* d, unsigned x, unsigned y, unsigned z,
                         const D3D12_TEXTURE_COPY_LOCATION* s, const void* box)
{
    (void)l;
    (void)x;
    (void)y;
    (void)z;
    (void)box;
    textures++;
    if (!d->pResource || d->pResource->desc.Width < s->PlacedFootprint.Footprint.Width ||
        d->pResource->desc.Height < s->PlacedFootprint.Footprint.Height ||
        d->pResource->desc.Format != s->PlacedFootprint.Footprint.Format ||
        d->pResource->state != D3D12_RESOURCE_STATE_COPY_DEST)
        invalid_texture++;
}
static void clear_target(list* l, unsigned rtv, const float* color, unsigned n, const void* rects)
{
    (void)l;
    (void)color;
    (void)n;
    (void)rects;
    clears++;
    if (rtv || !g.backbuf[0] || g.backbuf[0]->state != D3D12_RESOURCE_STATE_RENDER_TARGET) invalid_barrier++;
}
static u32 buffer_index(swap* s)
{
    (void)s;
    return 0;
}
static HRESULT swap_present(swap* s, unsigned interval, unsigned flags)
{
    (void)s;
    (void)interval;
    (void)flags;
    swap_calls++;
    return present_result;
}
static resource_vtable resource_vt = {resource_desc};
static list_vtable list_vt = {barrier, copy_resource, copy_texture, clear_target};
static list command_list = {&list_vt};
static swap_vtable swap_vt = {buffer_index, swap_present};
static swap swap_chain = {&swap_vt};
