/*
 * D3D12 test for patches 2010-2012 (fragment-program decompiler output on a real D3D12 device), under Proton:
 *
 *   nix develop .#win --command tools/sandbox.sh -- tools/smoke/test_fp_d3d12.sh              (build)
 *   tools/agents/lock.sh gpu -- nix develop --command tools/proton-test.sh --seconds 25 \
 *       --name fp-d3d12 build/smoke-fp/test_fp_d3d12.exe                                     (run)
 *
 * Part 1, pack/unpack (2010, 2011):
 *
 * The packed word of PKB/PKG/PK2/... travels in a float register as raw bits (asfloat). With alpha byte 0 it is
 * a denormal or zero-exponent float, with alpha byte >= 0x80 a negative value or a NaN (alpha 1.0 -> exponent
 * 0xFF). A D3D12 driver may flush denormals or canonicalise NaNs on arithmetic or even on moves, which would
 * destroy the packed bytes. This test hand-assembles small fragment programs (pack -> [move] -> unpack), turns
 * them into HLSL with ps3recomp's rsx_fp_decompile_buffered_ex (the same entry point live-draw uses, constants
 * in cbuffer b1 so nothing is constant-folded), compiles ps_5_0 with D3DCompile (flags 0, as live-draw), draws
 * one pixel per input colour into an R32G32B32A32_FLOAT target, reads it back and compares with the CPU
 * result. Part 2, textures (2012): a 3D unit (cube-mask bit 16 + N, Texture3D 2x2x2 RGBA8, point sampling), a
 * shadow-compare unit (rsx_fp_apply_shadow_compare, zfunc GEQUAL, R32_FLOAT depth, comparison sampler at s22
 * as live-draw's root signature) and a depth-as-A8R8G8B8 unit (rsx_fp_apply_depth_argb, remap 0xAAE4).
 * Prints one [PASS]/[FAIL] line per program and input, then "N passed, M failed".
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rsx_fp_decompiler.h"

static int g_pass, g_fail;

/* ---- FP microcode assembly (same layout as tools/smoke/test_fp_pack.c) ---- */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define OUTREG(i)   ((u32)(i) << 1)
#define OUT_HALF    (1u << 7)
#define COND_WRITE  (1u << 8)
#define SRC_HALF    (1u << 8)
#define SWZ_IDENT   ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define COND_SWZ    ((0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define EX_ALL      ((7u << 18) | COND_SWZ)
#define EX_GT       ((4u << 18) | COND_SWZ)
#define T_TEMP 0u
#define T_CONST 2u
#define REG(i) ((u32)(i) << 2)
#define CTRL_32BIT 0x40u

enum { F_END = 1, F_DHALF = 2, F_SHALF = 4, F_SETCC = 8 };
#define F_UNIT(u) ((int)((u) + 1) << 8)   /* texture unit for TEX */

static u32 emit(u8* p, u32 op, u32 dst, u32 st, u32 sr, u32 exec, int fl)
{
    put_word(p + 0, OPC(op) | OUTREG(dst) | OUTMASK_ALL | ((fl & F_END) ? 1u : 0u) |
                    (((fl >> 8) & 31) ? (u32)(((fl >> 8) & 31) - 1) << 17 : 0u) |
                    ((fl & F_DHALF) ? OUT_HALF : 0u) | ((fl & F_SETCC) ? COND_WRITE : 0u));
    put_word(p + 4, st | REG(sr) | ((fl & F_SHALF) ? SRC_HALF : 0u) | SWZ_IDENT | exec);
    put_word(p + 8, 0);
    put_word(p + 12, 0);
    if (st != T_CONST) return 16;
    memset(p + 16, 0, 16);   /* buffered: the values come from the cbuffer */
    return 32;
}

/* ---- CPU reference ---- */
static float sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float q8(float v) { return roundf(sat(v) * 255.0f) / 255.0f; }
static float qs8(float v) { v = v < -1 ? -1 : v > 1 ? 1 : v; return roundf(v * 127.0f) / 127.0f; }
static float q16(float v) { return roundf(sat(v) * 65535.0f) / 65535.0f; }
static float l2s(float c) { return sat(c < 0.0031308f ? c * 12.92f : 1.055f * powf(fabsf(c), 1.0f / 2.4f) - 0.055f); }
static float qh(float v) { /* fp32 -> fp16 -> fp32 (round to nearest even), normal range only */
    u32 u; memcpy(&u, &v, 4);
    u32 r = (u + 0xFFFu + ((u >> 13) & 1u)) & ~0x1FFFu;
    float f; memcpy(&f, &r, 4); return f;
}

typedef struct { const char* name; int kind; float tol; } Prog;
enum { K_U8, K_SRGB, K_H2, K_U16, K_S8 };

static void expect(int kind, const float c[4], float e[4])
{
    for (int i = 0; i < 4; i++) {
        switch (kind) {
        case K_U8:   e[i] = q8(c[i]); break;
        case K_SRGB: e[i] = q8(l2s(c[i])); break;
        case K_H2:   e[i] = qh(c[i & 1]); break;
        case K_U16:  e[i] = q16(c[i & 1]); break;
        case K_S8:   e[i] = qs8(c[i]); break;
        }
    }
}

/* Input colours: comment = packed PKB word and what it is as a float. */
static const float g_col[][4] = {
    {0.2f, 0.4f, 0.1f, 0.0f},    /* 0x001A6633 positive denormal */
    {0.2f, 0.4f, 0.6f, 0.0f},    /* 0x00996633 exponent 1 (smallest normal range) */
    {0.2f, 0.4f, 0.1f, 1.0f},    /* 0xFF1A6633 NaN (exponent 0xFF, mantissa != 0) */
    {0.0f, 0.0f, 0.0f, 1.0f},    /* 0xFF000000 -1.7e38 */
    {0.0f, 0.0f, 0.0f, 0.0f},    /* 0x00000000 +0 */
    {0.0f, 0.0f, 0.004f, 0.0f},  /* 0x00010000 tiny denormal */
    {1.0f, 1.0f, 1.0f, 1.0f},    /* 0xFFFFFFFF NaN */
    {0.5f, 0.5f, 0.1f, 0.5f},    /* 0x801A8080 negative denormal */
};
#define NCOL ((int)(sizeof g_col / sizeof g_col[0]))

/* Part 2 inputs (texture coordinate constant) and expected RGBA. */
typedef struct { float in[4]; float want[4]; } TexCase;
#define V8(a) ((a) / 255.0f)
static const TexCase g_tex[4][4] = {
    {   /* tex3d: voxel centres of the 2x2x2 texture (x fastest, then y, then z) */
        {{0.25f, 0.25f, 0.25f, 1}, {1, 0, 0, 1}},
        {{0.75f, 0.25f, 0.25f, 1}, {0, 1, 0, 1}},
        {{0.25f, 0.75f, 0.75f, 1}, {V8(64), V8(128), V8(192), 1}},
        {{0.75f, 0.75f, 0.75f, 1}, {V8(10), V8(20), V8(30), V8(40)}},
    },
    {   /* shadow-cmp: ref z = 0.5 against depths 0.3 | 0.7, zfunc GEQUAL -> pass if ref <= depth */
        {{0.25f, 0.5f, 0.5f, 1}, {0, 0, 0, 0}},
        {{0.75f, 0.5f, 0.5f, 1}, {1, 1, 1, 1}},
        {{0.25f, 0.5f, 0.2f, 1}, {1, 1, 1, 1}},
    },
    {   /* depth-argb: depth 0x123456 -> A 0x12 R 0x34 G 0x56 B (stencil) 0, identity remap -> RGBA */
        {{0.5f, 0.5f, 0, 1}, {V8(0x34), V8(0x56), 0, V8(0x12)}},
    },
    {   /* depth-argb-aa93 (KZ3's 124 binds of the 1280x720 Z24S8 zeta): RGB = depth bytes high to low, A = stencil */
        {{0.5f, 0.5f, 0, 1}, {V8(0x12), V8(0x34), V8(0x56), 0}},
    },
};
static const int g_ntex[4] = {4, 3, 1, 1};

#define CHK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("[FAIL] %s = 0x%08lx (line %d)\n", #x, (unsigned long)_h, __LINE__); return 1; } } while (0)

static const char vs_src[] =
    "struct PSInput {\n"
    "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
    "    float4 fog : FOG;\n"
    "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
    "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
    "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"   /* PR #328 (9328): a PSInput declares TC8/9 only when read; trailing VS outputs past it link */
    "};\n"
    "PSInput main(uint id : SV_VertexID) { PSInput o = (PSInput)0;\n"
    "  float2 uv = float2((id << 1) & 2, id & 2); o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o; }\n";

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Programs: build microcode + HLSL. */
    static const Prog progs[] = {
        {"pkb-upb", K_U8, 1e-6f}, {"pkg-upb", K_SRGB, 1.01f / 255.0f}, {"pkb-mov-upb", K_U8, 1e-6f},
        {"pkb-half-upb", K_U8, 1e-6f}, {"pkb-pred-upb", K_U8, 1e-6f}, {"pk2-up2", K_H2, 2e-4f},
        {"pk16-up16", K_U16, 1e-6f}, {"pk4-up4", K_S8, 1e-6f},
        {"tex3d", 0, 0}, {"shadow-cmp", 0, 0}, {"depth-argb", 0, 0}, {"depth-argb-aa93", 0, 0},
    };
    const int NPACK = 8;
    const int NP = (int)(sizeof progs / sizeof progs[0]);
    static u8 code[12][512];
    static char hlsl[12][65536];
    u32 ncon[12];
    for (int i = 0; i < NP; i++) {
        u8* p = code[i]; u32 n = 0;
        memset(p, 0, 512);
        switch (i) {
        case 0: n += emit(p + n, 0x27, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x28, 0, T_TEMP, 1, EX_ALL, F_END); break;
        case 1: n += emit(p + n, 0x2C, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x28, 0, T_TEMP, 1, EX_ALL, F_END); break;
        case 2: n += emit(p + n, 0x27, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x01, 2, T_TEMP, 1, EX_ALL, 0);
                n += emit(p + n, 0x28, 0, T_TEMP, 2, EX_ALL, F_END); break;
        case 3: n += emit(p + n, 0x27, 2, T_CONST, 0, EX_ALL, F_DHALF);
                n += emit(p + n, 0x28, 0, T_TEMP, 2, EX_ALL, F_END | F_SHALF); break;
        case 4: n += emit(p + n, 0x27, 1, T_CONST, 0, EX_ALL, 0);
                n += emit(p + n, 0x01, 3, T_CONST, 0, EX_ALL, F_SETCC);       /* cc0 = c1 = (1,1,1,1) */
                n += emit(p + n, 0x01, 2, T_TEMP, 1, EX_GT, 0);               /* predicated move of the packed word */
                n += emit(p + n, 0x28, 0, T_TEMP, 2, EX_ALL, F_END); break;
        case 5: n += emit(p + n, 0x24, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x25, 0, T_TEMP, 1, EX_ALL, F_END); break;
        case 6: n += emit(p + n, 0x29, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x2A, 0, T_TEMP, 1, EX_ALL, F_END); break;
        case 7: n += emit(p + n, 0x13, 1, T_CONST, 0, EX_ALL, 0); n += emit(p + n, 0x14, 0, T_TEMP, 1, EX_ALL, F_END); break;
        default: n += emit(p + n, 0x17, 0, T_CONST, 0, EX_ALL, F_END | F_UNIT(i == 11 ? 2 : i - NPACK)); break;   /* TEX r0, c0, unit */
        }
        ncon[i] = 0;
        const u32 mask = i == 8 ? 1u << 16 : 0u;   /* unit 0 is 3D */
        if (rsx_fp_decompile_buffered_ex(p, n, CTRL_32BIT, mask, hlsl[i], sizeof hlsl[i], &ncon[i]) < 0) {
            printf("[FAIL] %s: decompile\n", progs[i].name); return 1;
        }
        if (i == 9) {
            u8 zf[16] = {0}; zf[1] = 6;   /* GEQUAL, as every KZ3 bind */
            if (rsx_fp_apply_shadow_compare(hlsl[i], sizeof hlsl[i], 1u << 1, zf) != 1) {
                printf("[FAIL] shadow-cmp: post-pass\n%s\n", hlsl[i]); return 1;
            }
        }
        if (i >= 10) {
            u16 rm[16] = {0}; rm[2] = i == 10 ? 0xAAE4 : 0xAA93;   /* 0xAA93: KZ3's depth-byte binds */
            if (rsx_fp_apply_depth_argb(hlsl[i], sizeof hlsl[i], 1u << 2, rm) != 1) {
                printf("[FAIL] depth-argb: post-pass\n%s\n", hlsl[i]); return 1;
            }
        }
        if (strstr(hlsl[i], "unhandled")) { printf("[FAIL] %s: unhandled opcode\n", progs[i].name); g_fail++; }
    }

    ID3D12Device* dev;
    CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev));
    ID3D12CommandQueue* q; D3D12_COMMAND_QUEUE_DESC qd = {0};
    CHK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q));
    ID3D12CommandAllocator* ca;
    CHK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&ca));

    /* Root signature: 32-bit root constants at b1 (fp_constants[N] + fp_alpha), max 4 constants + alpha. */
    D3D12_ROOT_PARAMETER rpa[3] = {0};
    rpa[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rpa[0].Constants.ShaderRegister = 1; rpa[0].Constants.Num32BitValues = 20;
    rpa[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE xr = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE sr = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0};
    rpa[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rpa[1].DescriptorTable.NumDescriptorRanges = 1; rpa[1].DescriptorTable.pDescriptorRanges = &xr;
    rpa[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rpa[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rpa[2].DescriptorTable.NumDescriptorRanges = 1; rpa[2].DescriptorTable.pDescriptorRanges = &sr;
    rpa[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    /* Comparison samplers s16..s23 as live-draw (2012): RSX zfunc F at s16+F, function reversed. */
    static const D3D12_COMPARISON_FUNC zf_rev[8] = {
        D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS };
    D3D12_STATIC_SAMPLER_DESC css[8] = {0};
    for (int f = 0; f < 8; f++) {
        css[f].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        css[f].AddressU = css[f].AddressV = css[f].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        css[f].ComparisonFunc = zf_rev[f]; css[f].MaxLOD = D3D12_FLOAT32_MAX;
        css[f].ShaderRegister = 16 + f; css[f].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {3, rpa, 8, css, 0};
    ID3DBlob *rsb, *err = NULL; ID3D12RootSignature* rs;
    CHK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err));
    CHK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                         &IID_ID3D12RootSignature, (void**)&rs));
    ID3DBlob* vsb;
    if (FAILED(D3DCompile(vs_src, sizeof vs_src - 1, "vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vsb, &err))) {
        printf("[FAIL] vs compile: %s\n", err ? (char*)ID3D10Blob_GetBufferPointer(err) : "?"); return 1;
    }
    ID3D12PipelineState* pso[12];
    for (int i = 0; i < NP; i++) {
        ID3DBlob* psb; err = NULL;
        if (FAILED(D3DCompile(hlsl[i], strlen(hlsl[i]), progs[i].name, NULL, NULL, "main", "ps_5_0", 0, 0, &psb, &err))) {
            printf("[FAIL] %s: ps compile: %s\n%s\n", progs[i].name,
                   err ? (char*)ID3D10Blob_GetBufferPointer(err) : "?", hlsl[i]);
            return 1;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = rs;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pd.SampleDesc.Count = 1;
        CHK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&pso[i]));
    }

    /* Render target NCOL x NP, readback buffer with 256-byte rows. */
    D3D12_HEAP_PROPERTIES hp_def = {D3D12_HEAP_TYPE_DEFAULT}, hp_rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = NCOL; td.Height = NP;   /* NP rows <= 16 */ td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt, *rb;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_def, 0, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                             &IID_ID3D12Resource, (void**)&rt));
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256 * NP; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_rb, 0, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void**)&rb));
    ID3D12DescriptorHeap* rh; D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, 0};
    CHK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&rh));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rh, &rtv);   /* mingw: out-param form */
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    /* Part 2 textures: t0 Texture3D 2x2x2 RGBA8, t1 R32_FLOAT 2x1 depths, t2 R32_FLOAT 1x1 depth. */
    static const u8 vox[8][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},
                                 {0,255,255,255},{255,0,255,255},{64,128,192,255},{10,20,30,40}};
    const float shadow_d[2] = {0.3f, 0.7f};
    const float zval = (float)0x123456 / 16777215.0f;
    ID3D12Resource *t3, *ts, *tz, *up;
    D3D12_HEAP_PROPERTIES hp_up = {D3D12_HEAP_TYPE_UPLOAD};
    bd.Width = 4096; bd.Flags = 0;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_up, 0, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                             &IID_ID3D12Resource, (void**)&up));
    u8* um; CHK(ID3D12Resource_Map(up, 0, NULL, (void**)&um));
    memset(um, 0, 4096);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++)            /* row pitch 256, slice pitch 512 */
        memcpy(um + z * 512 + y * 256, vox[z * 4 + y * 2], 8);
    memcpy(um + 1024, shadow_d, 8);
    memcpy(um + 2048, &zval, 4);
    D3D12_RESOURCE_DESC xd = td;
    xd.Flags = 0;
    xd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; xd.Width = 2; xd.Height = 2; xd.DepthOrArraySize = 2;
    xd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_def, 0, &xd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void**)&t3));
    xd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; xd.Width = 2; xd.Height = 1; xd.DepthOrArraySize = 1;
    xd.Format = DXGI_FORMAT_R32_FLOAT;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_def, 0, &xd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void**)&ts));
    xd.Width = 1;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp_def, 0, &xd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void**)&tz));
    ID3D12DescriptorHeap *sh, *mh;
    D3D12_DESCRIPTOR_HEAP_DESC shd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    CHK(ID3D12Device_CreateDescriptorHeap(dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&sh));
    D3D12_DESCRIPTOR_HEAP_DESC mhd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    CHK(ID3D12Device_CreateDescriptorHeap(dev, &mhd, &IID_ID3D12DescriptorHeap, (void**)&mh));
    const UINT sinc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT minc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE sc0, mc0;
    D3D12_GPU_DESCRIPTOR_HANDLE sg0, mg0;
    sh->lpVtbl->GetCPUDescriptorHandleForHeapStart(sh, &sc0);
    mh->lpVtbl->GetCPUDescriptorHandleForHeapStart(mh, &mc0);
    sh->lpVtbl->GetGPUDescriptorHandleForHeapStart(sh, &sg0);
    mh->lpVtbl->GetGPUDescriptorHandleForHeapStart(mh, &mg0);
    for (UINT k = 0; k < 16; k++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {0};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = {sc0.ptr + k * sinc};
        ID3D12Resource* res = k == 0 ? t3 : k == 1 ? ts : k == 2 ? tz : NULL;
        if (k == 0) { nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; nd.Texture3D.MipLevels = 1; }
        if (k == 1 || k == 2) {
            nd.Format = DXGI_FORMAT_R32_FLOAT;
            /* live-draw's srv_write_zdepth mapping: depth in RGB, alpha 1 */
            nd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0,
                D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        }
        ID3D12Device_CreateShaderResourceView(dev, res, &nd, hnd);
        D3D12_SAMPLER_DESC smp = {0};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX; smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_CPU_DESCRIPTOR_HANDLE mhh = {mc0.ptr + k * minc};
        ID3D12Device_CreateSampler(dev, &smp, mhh);
    }

    ID3D12GraphicsCommandList* cl;
    CHK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL,
                                       &IID_ID3D12GraphicsCommandList, (void**)&cl));
    float clear[4] = {-7, -7, -7, -7};
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
    {   /* upload the part 2 textures */
        D3D12_TEXTURE_COPY_LOCATION d0 = {0}, s0 = {0};
        s0.pResource = up; s0.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s0.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 2, 256};
        d0.pResource = t3; ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d0, 0, 0, 0, &s0, NULL);
        s0.PlacedFootprint.Offset = 1024;
        s0.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){DXGI_FORMAT_R32_FLOAT, 2, 1, 1, 256};
        d0.pResource = ts; ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d0, 0, 0, 0, &s0, NULL);
        s0.PlacedFootprint.Offset = 2048;
        s0.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
        d0.pResource = tz; ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d0, 0, 0, 0, &s0, NULL);
        D3D12_RESOURCE_BARRIER tb[3] = {0};
        ID3D12Resource* rs3[3] = {t3, ts, tz};
        for (int k = 0; k < 3; k++) {
            tb[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; tb[k].Transition.pResource = rs3[k];
            tb[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            tb[k].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 3, tb);
    }
    ID3D12DescriptorHeap* heaps[2] = {sh, mh};
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, sg0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 2, mg0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int i = 0; i < NP; i++) {
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso[i]);
        for (int c = 0; c < NCOL; c++) {
            float k[20] = {0};
            if (i < NPACK) memcpy(k, g_col[c], 16);
            else if (c < g_ntex[i - NPACK]) memcpy(k, g_tex[i - NPACK][c].in, 16);
            else continue;
            for (int j = 4; j < 8; j++) k[j] = 1.0f;   /* second constant (program 4's cc source) */
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 20, k, 0);
            D3D12_VIEWPORT vp = {(float)c, (float)i, 1, 1, 0, 1};
            D3D12_RECT sc = {c, i, c + 1, i + 1};
            ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
            ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = NCOL; dst.PlacedFootprint.Footprint.Height = NP;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CHK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)cl};
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
    ID3D12Fence* fe; CHK(ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void**)&fe));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHK(ID3D12CommandQueue_Signal(q, fe, 1));
    CHK(ID3D12Fence_SetEventOnCompletion(fe, 1, ev));
    WaitForSingleObject(ev, 10000);

    u8* m; D3D12_RANGE rr = {0, 256 * (SIZE_T)NP};
    CHK(ID3D12Resource_Map(rb, 0, &rr, (void**)&m));
    for (int i = 0; i < NP; i++) {
        for (int c = 0; c < NCOL; c++) {
            const float* g = (const float*)(m + 256 * i + 16 * c);
            if (i >= NPACK) {
                if (c >= g_ntex[i - NPACK]) continue;
                const TexCase* tc = &g_tex[i - NPACK][c];
                int ok = 1;
                for (int j = 0; j < 4; j++) if (!(fabsf(g[j] - tc->want[j]) <= 1e-3f)) ok = 0;
                printf("[%s] %s input %d (%.3f %.3f %.3f): got %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f\n",
                       ok ? "PASS" : "FAIL", progs[i].name, c, tc->in[0], tc->in[1], tc->in[2],
                       g[0], g[1], g[2], g[3], tc->want[0], tc->want[1], tc->want[2], tc->want[3]);
                if (ok) g_pass++; else g_fail++;
                continue;
            }
            float e[4]; expect(progs[i].kind, g_col[c], e);
            int ok = 1;
            for (int j = 0; j < 4; j++) if (!(fabsf(g[j] - e[j]) <= progs[i].tol)) ok = 0;
            printf("[%s] %s colour %d (%.3f %.3f %.3f %.3f): got %.6f %.6f %.6f %.6f want %.6f %.6f %.6f %.6f\n",
                   ok ? "PASS" : "FAIL", progs[i].name, c, g_col[c][0], g_col[c][1], g_col[c][2], g_col[c][3],
                   g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
            if (ok) g_pass++; else g_fail++;
        }
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) for (int i = 0; i < NP; i++) printf("---- %s\n%s\n", progs[i].name, hlsl[i]);
    return g_fail != 0;
}
